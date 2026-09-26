// Host test for the in-game menu: the real main.c (hooks, OSD), menu.c, config.c (parse and
// save), io.c, osd.c and patch_hook.c against stand-ins for the Vita (menu_stubs.c)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "menu_stubs.h"

// Reach the hooks, which are static
#include "../src/main.c"

extern vg_io_status_t stub_patch_status;
extern vg_module_match_t stub_patch_match;
extern vg_feature_state_t stub_patch_caps[FEATURE_INVALID];
extern int stub_patch_injects;
int vg_hook_sceCtrlReadBufferPositive_peekPatched(int port, SceCtrlData *pad_data, int count);

#define NID_DISPLAY  0x7A410B64
#define NID_PEEK_NEG 0x104ED1A7
#define NID_PEEK_POS 0xA9C3CED6
#define NID_READ_POS 0x67E7AB83
#define NID_READ_NEG 0x15F96FB0
#define NID_READ_EXT 0xE2D99296
#define NID_TOUCH    0xFF082DF0
#define NID_READ_POS2 0xC4226A3E

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...) do { \
        g_checks++; \
        if (!(cond)) { \
            g_failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

// The game's side of the hooked imports

static int g_display_calls = 0;
static const SceDisplayFrameBuf *g_display_last = NULL;

static int original_set_framebuf(const SceDisplayFrameBuf *param, int sync) {
    g_display_calls++;
    g_display_last = param;
    return 0;
}

static void fill_pad(SceCtrlData *ctrl, int count, bool negative) {
    for (int i = 0; i < count; i++) {
        ctrl[i] = stub_pad;
        if (negative)
            ctrl[i].buttons = ~stub_pad.buttons;
    }
}

static int original_ctrl_positive(int port, SceCtrlData *ctrl, int count) {
    fill_pad(ctrl, count, false);
    return count;
}

static int original_ctrl_negative(int port, SceCtrlData *ctrl, int count) {
    fill_pad(ctrl, count, true);
    return count;
}

// The "2" reads report the L/R triggers as L1/R1
static int original_ctrl_positive2(int port, SceCtrlData *ctrl, int count) {
    fill_pad(ctrl, count, false);
    for (int i = 0; i < count; i++) {
        uint32_t b = ctrl[i].buttons & ~(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER);
        ctrl[i].buttons = b | (stub_pad.buttons & SCE_CTRL_LTRIGGER ? SCE_CTRL_L1 : 0)
                | (stub_pad.buttons & SCE_CTRL_RTRIGGER ? SCE_CTRL_R1 : 0);
    }
    return count;
}

static SceUInt32 g_touch_reports = 2;

static int original_touch(SceUInt32 port, SceTouchData *data, SceUInt32 count) {
    for (SceUInt32 i = 0; i < count; i++) {
        memset(&data[i], 0, sizeof(data[i]));
        data[i].reportNum = g_touch_reports;
    }
    return count;
}

static const struct {
    uint32_t nid;
    void *original;
} g_imports[] = {
    {NID_DISPLAY, original_set_framebuf},
    {0x104ED1A7, original_ctrl_negative}, {0x81A89660, original_ctrl_negative},
    {0xA9C3CED6, original_ctrl_positive}, {0x15F81E8C, original_ctrl_positive2},
    {0x15F96FB0, original_ctrl_negative}, {0x27A0C5FB, original_ctrl_negative},
    {0x67E7AB83, original_ctrl_positive}, {0xC4226A3E, original_ctrl_positive2},
    {0xA59454D3, original_ctrl_positive}, {0x860BF292, original_ctrl_positive2},
    {0xE2D99296, original_ctrl_positive}, {0xA7178860, original_ctrl_positive2},
    {0xFF082DF0, original_touch}, {0x169A1D58, original_touch},
};

// Files

static void run(const char *command) {
    if (system(command) != 0) {
        printf("command failed: %s\n", command);
        exit(1);
    }
}

static void fs_reset() {
    char command[1200];
    snprintf(command, sizeof(command), "rm -rf %s && mkdir -p %s/data/VitaGrafix", stub_root, stub_root);
    run(command);
}

static void write_file(const char *vita_path, const char *content) {
    char path[1024];
    stub_map_path(vita_path, path, sizeof(path));
    char command[1200];
    snprintf(command, sizeof(command), "mkdir -p \"$(dirname %s)\"", path);
    run(command);
    FILE *f = fopen(path, "wb");
    fputs(content, f);
    fclose(f);
}

// A fresh copy each time, freed at exit
static char *g_reads[256];
static int g_read_count = 0;

static const char *read_file(const char *vita_path) {
    char path[1024];
    stub_map_path(vita_path, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *content = calloc(1, 16384);
    fread(content, 1, 16383, f);
    fclose(f);
    if (g_read_count < 256)
        g_reads[g_read_count++] = content;
    return content;
}

static bool file_exists(const char *vita_path) {
    char path[1024];
    struct stat st;
    stub_map_path(vita_path, path, sizeof(path));
    return stat(path, &st) == 0;
}

#define CONFIG_TXT  "ux0:data/VitaGrafix/config.txt"
#define TITLE_TXT   "ux0:data/VitaGrafix/config/PCSA00549.txt"
#define TITLE_TMP   TITLE_TXT ".vgtmp"
#define TITLE_OLD   TITLE_TXT ".vgold"
#define OURS        "[PCSA00549,eboot.bin,0x91ABDB4F]\n"

// Boot the plugin with these files

static void boot(const char *config_txt, const char *title_txt) {
    fs_reset();
    if (config_txt)
        write_file(CONFIG_TXT, config_txt);
    if (title_txt)
        write_file(TITLE_TXT, title_txt);

    stub_reset_hooks();
    for (size_t i = 0; i < sizeof(g_imports) / sizeof(g_imports[0]); i++)
        stub_set_original(g_imports[i].nid, g_imports[i].original);

    memset(&g_main, 0, sizeof(g_main));
    memset(&stub_pad, 0, sizeof(stub_pad));
    stub_pad.lx = stub_pad.ly = stub_pad.rx = stub_pad.ry = 128;
    stub_log[0] = '\0';
    stub_vblank_waits = 0;
    g_display_calls = 0;
    module_start(0, NULL);
}

// One presented frame

static uint32_t *g_fb = NULL;
static SceDisplayFrameBuf g_fb_param;
#define FB_MARK 0x12345678u

static void fb_setup(int width, int height, int pitch) {
    free(g_fb);
    g_fb = malloc((size_t)pitch * height * 4); // exactly the frame, so ASan sees any overrun
    for (int i = 0; i < pitch * height; i++)
        g_fb[i] = FB_MARK;
    g_fb_param = (SceDisplayFrameBuf){sizeof(SceDisplayFrameBuf), g_fb, pitch, 0, width, height};
}

static int fb_changed() {
    int changed = 0;
    for (unsigned int i = 0; i < g_fb_param.pitch * g_fb_param.height; i++) {
        if (g_fb[i] != FB_MARK)
            changed++;
        g_fb[i] = FB_MARK;
    }
    return changed;
}

// Pixels right of the frame width (the pitch padding) must stay untouched
static bool fb_padding_clean() {
    for (unsigned int y = 0; y < g_fb_param.height; y++) {
        for (unsigned int x = g_fb_param.width; x < g_fb_param.pitch; x++) {
            if (g_fb[y * g_fb_param.pitch + x] != FB_MARK)
                return false;
        }
    }
    return true;
}

static int frame() {
    int (*set_framebuf)(const SceDisplayFrameBuf *, int) = stub_import(NID_DISPLAY);
    set_framebuf(&g_fb_param, 1);
    stub_now += 16667;
    return fb_changed();
}

static void pad(uint32_t buttons) {
    stub_pad.buttons = buttons;
}

// Press, and let a frame see it
static void press(uint32_t buttons) {
    pad(buttons);
    frame();
}

static void tap(uint32_t buttons) {
    press(buttons);
    press(0);
}

static void open_menu() {
    press(SCE_CTRL_SELECT);
    press(SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER);
    press(0);
}

static SceCtrlData game_read(uint32_t nid) {
    int (*read)(int, SceCtrlData *, int) = stub_import(nid);
    SceCtrlData ctrl[2];
    memset(ctrl, 0xEE, sizeof(ctrl));
    int ret = read(0, ctrl, 2);
    (void)ret;
    return ctrl[1];
}

static SceUInt32 game_touch() {
    int (*touch)(SceUInt32, SceTouchData *, SceUInt32) = stub_import(NID_TOUCH);
    SceTouchData data[1];
    touch(0, data, 1);
    return data[0].reportNum;
}

static const char *CONFIG_BASIC =
    "[MAIN]\n"
    "OSD=1\n";

static void test_install() {
    printf("-- hook install\n");

    boot(CONFIG_BASIC, NULL);
    CHECK(stub_is_hooked(NID_DISPLAY), "display hook");
    CHECK(g_main.menu_ready, "menu ready");
    CHECK(stub_hooks_installed == 1 + INPUT_HOOK_NUM, "all input hooks, got %d", stub_hooks_installed);
    CHECK(strstr(stub_log, "Menu ready, 14 input hooks") != NULL, "logged");
    module_stop(0, NULL);
    CHECK(stub_hooks_released == stub_hooks_installed, "all released (%d of %d)", stub_hooks_released, stub_hooks_installed);

    boot("[MAIN]\nOSD=0\n", NULL);
    CHECK(stub_hooks_installed == 0 && !g_main.menu_ready, "OSD=0: no hooks");

    stub_patch_match = MODULE_NID_MISMATCH;
    boot(CONFIG_BASIC, NULL);
    CHECK(stub_hooks_installed == 1 && !g_main.menu_ready, "other version: OSD only, got %d", stub_hooks_installed);
    stub_patch_match = MODULE_MATCH;

    vg_feature_state_t caps[FEATURE_INVALID];
    memcpy(caps, stub_patch_caps, sizeof(caps));
    for (int i = 0; i < FEATURE_INVALID; i++)
        stub_patch_caps[i] = FT_UNSUPPORTED;
    boot(CONFIG_BASIC, NULL);
    CHECK(stub_hooks_installed == 1 && !g_main.menu_ready, "nothing to configure: OSD only");
    memcpy(stub_patch_caps, caps, sizeof(caps));

    stub_patch_status.code = IO_ERROR_PARSE_INVALID_TOKEN;
    boot(CONFIG_BASIC, NULL);
    CHECK(stub_hooks_installed == 1 && !g_main.menu_ready, "patch error: OSD only");
    stub_patch_status.code = IO_OK;

    stub_patch_match = MODULE_TITLE_MISMATCH;
    boot(CONFIG_BASIC, NULL);
    CHECK(stub_hooks_installed == 0, "other game: nothing");
    stub_patch_match = MODULE_MATCH;
}

static void test_closed_passthrough() {
    printf("-- closed menu leaves input alone\n");
    boot(CONFIG_BASIC, NULL);
    fb_setup(960, 544, 960);
    frame();

    pad(SCE_CTRL_CROSS | SCE_CTRL_START | SCE_CTRL_HEADPHONE);
    stub_pad.lx = 3; stub_pad.ry = 250; stub_pad.cross = 99;
    for (size_t i = 1; i < sizeof(g_imports) / sizeof(g_imports[0]) - 2; i++) {
        SceCtrlData expect[2], got = game_read(g_imports[i].nid);
        ((int (*)(int, SceCtrlData *, int))g_imports[i].original)(0, expect, 2);
        CHECK(!memcmp(&got, &expect[1], sizeof(got)), "import 0x%08X untouched", g_imports[i].nid);
    }
    CHECK(game_touch() == g_touch_reports, "touch untouched");

    SceCtrlData got;
    vg_hook_sceCtrlReadBufferPositive_peekPatched(0, &got, 1);
    CHECK(got.buttons == stub_pad.buttons && got.lx == 3, "peekPatched untouched");
}

static void test_open_masks() {
    printf("-- open menu hides input\n");
    boot(CONFIG_BASIC, NULL);
    fb_setup(960, 544, 960);
    frame();

    open_menu();
    CHECK(vg_menu_is_open(), "SELECT+R opens");
    int waits = stub_vblank_waits;
    CHECK(frame() > 0, "menu drawn");
    CHECK(stub_vblank_waits == waits + 1, "one vblank wait per frame while open");

    uint32_t status = SCE_CTRL_HEADPHONE | SCE_CTRL_INTERCEPTED;
    pad(SCE_CTRL_CROSS | SCE_CTRL_LEFT | status);
    stub_pad.lx = 0; stub_pad.ly = 255; stub_pad.rx = 7; stub_pad.ry = 200; stub_pad.cross = 255; stub_pad.lt = 80;

    SceCtrlData got = game_read(NID_READ_POS);
    CHECK(got.buttons == status, "positive: only status bits, got 0x%X", got.buttons);
    CHECK(got.lx == 128 && got.ly == 128 && got.rx == 128 && got.ry == 128, "sticks centred");
    CHECK(got.cross == 0 && got.lt == 0, "pressure cleared");

    got = game_read(NID_PEEK_NEG);
    uint32_t expect = ~status | (~stub_pad.buttons & status);
    CHECK(got.buttons == expect, "negative: nothing pressed, got 0x%X want 0x%X", got.buttons, expect);

    got = game_read(NID_READ_EXT);
    CHECK(got.buttons == status && got.lx == 128, "Ext read masked");

    CHECK(game_touch() == 0, "touch hidden");

    vg_hook_sceCtrlReadBufferPositive_peekPatched(0, &got, 1);
    CHECK(got.buttons == status && got.ly == 128, "peekPatched masked");

    // Nothing the menu reads reaches the game, even while it is being used
    press(SCE_CTRL_DOWN | status);
    got = game_read(NID_PEEK_POS);
    CHECK(got.buttons == status, "D-pad hidden");
}

static void test_close_held() {
    printf("-- buttons held on close stay hidden until released\n");
    boot(CONFIG_BASIC, NULL);
    fb_setup(960, 544, 960);
    frame();
    open_menu();

    press(SCE_CTRL_SELECT);
    press(SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER);
    CHECK(!vg_menu_is_open(), "SELECT+R closes");

    pad(SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER | SCE_CTRL_CROSS);
    stub_pad.lx = 10;
    SceCtrlData got = game_read(NID_READ_POS);
    CHECK(got.buttons == SCE_CTRL_CROSS, "only the new press, got 0x%X", got.buttons);
    CHECK(got.lx == 10, "sticks back");
    got = game_read(NID_READ_NEG);
    CHECK(got.buttons == ~(uint32_t)SCE_CTRL_CROSS, "negative too, got 0x%X", got.buttons);

    press(SCE_CTRL_SELECT | SCE_CTRL_CROSS); // R released
    pad(SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER | SCE_CTRL_CROSS); // and pressed again
    got = game_read(NID_READ_POS);
    CHECK(got.buttons == (SCE_CTRL_RTRIGGER | SCE_CTRL_CROSS), "R again after release, got 0x%X", got.buttons);

    press(0);
    pad(SCE_CTRL_SELECT);
    got = game_read(NID_READ_POS);
    CHECK(got.buttons == SCE_CTRL_SELECT, "all released: everything back");
    CHECK(game_touch() == g_touch_reports, "touch back");

    // The "2" reads see the held R as R1: still hidden
    open_menu();
    press(SCE_CTRL_SELECT);
    press(SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER);
    CHECK(!vg_menu_is_open(), "closed again");
    got = game_read(NID_READ_POS2);
    CHECK(got.buttons == 0, "R held: hidden from a Positive2 read too, got 0x%X", got.buttons);
    press(0);
    pad(SCE_CTRL_RTRIGGER);
    CHECK(game_read(NID_READ_POS2).buttons == SCE_CTRL_R1, "R back as R1 once released");

    // Status bits are never held back
    press(0);
    open_menu();
    press(SCE_CTRL_START | SCE_CTRL_HEADPHONE);
    stub_run_threads();
    press(SCE_CTRL_HEADPHONE);
    for (int i = 0; i < 10; i++)
        press(SCE_CTRL_HEADPHONE);
    pad(SCE_CTRL_HEADPHONE);
    CHECK(game_read(NID_READ_POS).buttons == SCE_CTRL_HEADPHONE, "HEADPHONE still reported after a close");
    pad(SCE_CTRL_START | SCE_CTRL_HEADPHONE);
    CHECK(game_read(NID_READ_POS).buttons == (SCE_CTRL_START | SCE_CTRL_HEADPHONE), "nothing blocked once START is up");
}

static void test_stale() {
    printf("-- no frames: the game gets its input back\n");
    boot(CONFIG_BASIC, NULL);
    fb_setup(960, 544, 960);
    frame();
    open_menu();

    pad(SCE_CTRL_CROSS);
    CHECK(game_read(NID_READ_POS).buttons == 0, "masked while presenting");
    stub_now += 600000;
    CHECK(game_read(NID_READ_POS).buttons == SCE_CTRL_CROSS, "input after 0.6 s without a frame");
    CHECK(game_touch() == g_touch_reports, "touch after 0.6 s without a frame");
    pad(0);
    frame();
    pad(SCE_CTRL_CROSS);
    CHECK(vg_menu_is_open() && game_read(NID_READ_POS).buttons == 0, "masked again once frames resume");
}

static void test_values() {
    printf("-- values\n");
    stub_patch_caps[FEATURE_FPS] = FT_ENABLED;
    boot("[MAIN]\nOSD=1\n[PCSA00549]\nIB=960x544,720x408\n", NULL);
    fb_setup(960, 544, 960);
    frame();
    vg_config_t *config = vg_config_get();
    open_menu();

    // FB row, Off: the first press turns it on at what it shows
    CHECK(config->fb_enabled == FT_DISABLED, "FB off");
    tap(SCE_CTRL_RIGHT);
    CHECK(config->fb_enabled == FT_ENABLED && config->fb.width == 960, "RIGHT turns FB on at 960x544");
    tap(SCE_CTRL_LEFT);
    CHECK(config->fb.width == 720 && config->fb.height == 408, "then LEFT steps to 720x408");
    tap(SCE_CTRL_RIGHT);
    CHECK(config->fb.width == 960, "and back");

    // IB row: a two-slot list survives CROSS off and on
    tap(SCE_CTRL_DOWN);
    CHECK(config->ib_count == 2, "IB list of two");
    tap(SCE_CTRL_CROSS);
    CHECK(config->ib_enabled == FT_DISABLED, "CROSS turns IB off");
    tap(SCE_CTRL_CROSS);
    CHECK(config->ib_enabled == FT_ENABLED && config->ib_count == 2 && config->ib[1].width == 720,
            "and on again with both slots (count %d, slot 1 %dx%d)", config->ib_count, config->ib[1].width, config->ib[1].height);

    // One resolution for every slot; the config's list shows its first value
    tap(SCE_CTRL_RIGHT);
    CHECK(config->ib_count == 1 && config->ib[0].width == 960 && config->ib[15].width == 960,
            "RIGHT: 960x544 for every slot (count %d, slot 15 %d)", config->ib_count, config->ib[15].width);
    tap(SCE_CTRL_LEFT);
    CHECK(config->ib[0].width == 720 && config->ib[0].height == 408 && config->ib[1].width == 720, "LEFT 720x408 everywhere");
    tap(SCE_CTRL_LEFT);
    tap(SCE_CTRL_LEFT);
    CHECK(config->ib[0].width == 480 && config->ib[0].height == 272, "LEFT LEFT 480x272");
    tap(SCE_CTRL_LEFT);
    CHECK(config->ib[0].width == 480, "and stays there");
    for (int i = 0; i < 4; i++)
        tap(SCE_CTRL_RIGHT);
    CHECK(config->ib_count == 1 && config->ib[0].width == 960, "back up to 960x544");

    // L/R do nothing on the IB row now
    tap(SCE_CTRL_RTRIGGER);
    tap(SCE_CTRL_LTRIGGER);
    CHECK(config->ib_count == 1 && config->ib[0].width == 960, "L/R leave IB alone");

    // FPS row, Off: turns on at 60 (no step)
    tap(SCE_CTRL_DOWN);
    CHECK(config->fps_enabled == FT_DISABLED, "FPS off");
    tap(SCE_CTRL_LEFT);
    CHECK(config->fps_enabled == FT_ENABLED && config->fps == FPS_60, "LEFT turns FPS on at 60");
    tap(SCE_CTRL_LEFT);
    CHECK(config->fps == FPS_30, "then steps to 30");

    // Cancel puts everything back
    tap(SCE_CTRL_UP);
    tap(SCE_CTRL_LEFT);
    CHECK(config->ib_count == 1, "(IB set to one value)");
    press(SCE_CTRL_SELECT);
    press(SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER);
    press(0);
    CHECK(config->fb_enabled == FT_DISABLED && config->fps_enabled == FT_DISABLED && config->ib_count == 2
            && config->ib[0].width == 960 && config->ib[1].width == 720, "SELECT+R discards the changes");
    CHECK(vg_menu_get_notice() == MENU_NOTICE_NONE, "no notice on cancel");
    CHECK(!file_exists(TITLE_TXT), "nothing saved");
    stub_patch_caps[FEATURE_FPS] = FT_UNSUPPORTED;

    // An IB from the config that is not a preset stays selectable, in size order
    boot("[PCSA00549]\nIB=1280x720\n", NULL);
    frame();
    open_menu();
    tap(SCE_CTRL_DOWN);
    tap(SCE_CTRL_RIGHT);
    CHECK(config->ib[0].width == 1280, "1280x720 kept at the top, got %d", config->ib[0].width);
    tap(SCE_CTRL_LEFT);
    CHECK(config->ib[0].width == 960, "LEFT 960x544");
    tap(SCE_CTRL_RIGHT);
    CHECK(config->ib[0].width == 1280 && config->ib[0].height == 720, "RIGHT back to 1280x720");

    boot("[PCSA00549]\nIB=700x400\n", NULL);
    frame();
    open_menu();
    tap(SCE_CTRL_DOWN);
    tap(SCE_CTRL_RIGHT);
    CHECK(config->ib[0].width == 720, "700x400 sits between 720x408 and 640x368: RIGHT 720x408");
    tap(SCE_CTRL_LEFT);
    CHECK(config->ib[0].width == 700 && config->ib[0].height == 400, "LEFT back to 700x400");
    tap(SCE_CTRL_LEFT);
    CHECK(config->ib[0].width == 640, "LEFT 640x368");
}

static void test_save_flow() {
    printf("-- START saves on a thread\n");
    boot("[MAIN]\nLOG=0\n[PCSA00549]\nIB=720x408\nMSAA=2\n", NULL);
    fb_setup(960, 544, 960);
    frame();
    for (int i = 0; i < 400; i++)
        frame(); // banner gone
    CHECK(frame() == 0, "banner gone after 5 s");

    open_menu();
    tap(SCE_CTRL_DOWN);
    tap(SCE_CTRL_LEFT); // IB 640x368

    int threads = stub_threads_run;
    press(SCE_CTRL_START);
    CHECK(!vg_menu_is_open(), "START closes");
    CHECK(vg_menu_get_notice() == MENU_NOTICE_SAVING, "saving");
    CHECK(stub_threads_run == threads, "not on the display thread");
    CHECK(game_read(NID_READ_POS).buttons == 0, "START hidden from the game");
    CHECK(frame() > 0, "notice drawn");

    press(SCE_CTRL_START); // still held, the menu is closed: nothing happens
    stub_run_threads();
    CHECK(stub_threads_run == threads + 1, "worker ran");
    frame();
    CHECK(vg_menu_get_notice() == MENU_NOTICE_SAVED, "saved");
    CHECK(frame() > 0, "saved notice drawn");
    for (int i = 0; i < 400; i++)
        frame();
    CHECK(frame() == 0, "notice gone after 5 s");

    press(0);
    pad(SCE_CTRL_START);
    CHECK(game_read(NID_READ_POS).buttons == SCE_CTRL_START, "START reaches the game once released and pressed again");

    const char *saved = read_file(TITLE_TXT);
    CHECK(saved && !strcmp(saved, "LOG=off\nFB=off\nIB=640x368\nMSAA=2\n"), "title file written:\n%s", saved);
    CHECK(!strcmp(read_file(CONFIG_TXT), "[MAIN]\nLOG=0\n[PCSA00549]\nIB=720x408\nMSAA=2\n"), "config.txt untouched");
    CHECK(!file_exists(TITLE_TMP) && !file_exists(TITLE_OLD), "no leftovers");

    // Next boot uses it
    boot(read_file(CONFIG_TXT), read_file(TITLE_TXT));
    vg_config_t *config = vg_config_get();
    CHECK(config->ib[0].width == 640 && config->msaa == MSAA_2X && config->msaa_enabled == FT_ENABLED,
            "reboot applies it");
    CHECK(config->log_enabled == FT_DISABLED, "LOG=0 still honoured");

    // START while a save is still running is ignored
    fb_setup(960, 544, 960);
    frame();
    open_menu();
    press(SCE_CTRL_START);
    press(0);
    open_menu();
    press(SCE_CTRL_START);
    CHECK(vg_menu_is_open(), "second START ignored while saving");
    stub_run_threads();
    press(0);
    press(SCE_CTRL_START);
    CHECK(!vg_menu_is_open(), "accepted once the first save is done");
    stub_run_threads();
    frame();

    // A thread that cannot start reports a failure
    open_menu();
    stub_fail_create_thread = 1;
    press(SCE_CTRL_START);
    stub_fail_create_thread = 0;
    CHECK(vg_menu_get_notice() == MENU_NOTICE_SAVE_FAILED, "failed to start");
    CHECK(frame() > 0, "failure drawn");

    // module_stop waits for a running save
    press(0);
    open_menu();
    press(SCE_CTRL_START);
    int before = stub_threads_run;
    module_stop(0, NULL);
    CHECK(stub_threads_run == before + 1, "module_stop let the save finish");
}

// Save from a parsed config directly

static const char *save_now() {
    CHECK(vg_config_save_start(), "save started");
    stub_run_threads();
    return vg_config_save_get_state() == CONFIG_SAVE_OK ? read_file(TITLE_TXT) : NULL;
}

static void boot_quiet(const char *config_txt, const char *title_txt) {
    boot(config_txt, title_txt);
}

static bool same_effective(const vg_config_t *a, const vg_config_t *b) {
    return a->enabled == b->enabled && a->osd_enabled == b->osd_enabled && a->log_enabled == b->log_enabled
        && a->fb_enabled == b->fb_enabled && (a->fb_enabled != FT_ENABLED || !memcmp(&a->fb, &b->fb, sizeof(a->fb)))
        && a->ib_enabled == b->ib_enabled && (a->ib_enabled != FT_ENABLED || !memcmp(a->ib, b->ib, sizeof(a->ib)))
        && a->fps_enabled == b->fps_enabled && (a->fps_enabled != FT_ENABLED || a->fps == b->fps)
        && a->msaa_enabled == b->msaa_enabled && (a->msaa_enabled != FT_ENABLED || a->msaa == b->msaa);
}

static void test_writer() {
    printf("-- config writer\n");

    // First save: the title's file replaces config.txt, so it keeps what config.txt gave
    // the game that the menu does not set (LOG); config.txt itself is never written
    const char *config_txt =
        "[MAIN]\n"
        "LOG=0\n"
        "[XXXXxxxxx]\n"
        "MSAA=2\n"
        "[PCSA00549]\n"
        "FPS=30 # only this title\n"
        "IB=720x408\n";
    boot_quiet(config_txt, NULL);
    vg_config_t before = *vg_config_get();
    const char *out = save_now();
    CHECK(out && !strcmp(out, "LOG=off\nFB=off\nIB=720x408\nMSAA=2\n"), "new file:\n%s", out);
    CHECK(!strcmp(read_file(CONFIG_TXT), config_txt), "config.txt untouched");
    boot_quiet(config_txt, out);
    vg_config_t after = *vg_config_get();
    CHECK(same_effective(&before, &after), "same effective config after the first save");

    // Options before the first header: replaced where the first of them is
    const char *title =
        "# my notes\n"
        "FB=720x408\n"
        "LOG=1 # keep me\n"
        "IB=640x368\n"
        "\n"
        "[MAIN]\n"
        "LOG=0\n"
        "FB=480x272\n";
    boot_quiet(NULL, title);
    vg_config_get()->fb = (vg_res_t){640, 368};
    out = save_now();
    const char *expect =
        "# my notes\n"
        "FB=640x368\n"
        "IB=640x368\n"
        "MSAA=off\n"
        "LOG=1 # keep me\n"
        "\n"
        "[MAIN]\n"
        "LOG=0\n"
        "FB=480x272\n";
    CHECK(out && !strcmp(out, expect), "in place:\n%s", out);

    // Saving again changes nothing
    char first[4096];
    snprintf(first, sizeof(first), "%s", out ? out : "");
    boot_quiet(NULL, first);
    CHECK(vg_config_get()->fb.width == 640 && vg_config_get_status()->code == IO_OK, "applies, [MAIN] FB still masked");
    out = save_now();
    CHECK(out && !strcmp(out, first), "second save is stable:\n%s", out);

    // A matching section later in the file: the options go there, the rest keeps its order
    title = "FB=720x408\nOSD=0\n[PCSA00549]\nOSD=1\nIB=640x368\n";
    boot_quiet(NULL, title);
    CHECK(vg_config_get()->osd_enabled == FT_ENABLED, "(OSD=1 at boot)");
    vg_config_get()->ib[0] = (vg_res_t){720, 408};
    out = save_now();
    CHECK(out && !strcmp(out, "FB=720x408\nOSD=0\n[PCSA00549]\nFB=720x408\nIB=720x408\nMSAA=off\nOSD=1\n"), "into the section:\n%s", out);
    snprintf(first, sizeof(first), "%s", out ? out : "");
    boot_quiet(NULL, first);
    CHECK(vg_config_get()->osd_enabled == FT_ENABLED && vg_config_get()->ib[0].width == 720, "OSD still 1, IB applied");
    out = save_now();
    CHECK(out && !strcmp(out, first), "and stable from then on");

    // Only comments and a [MAIN]: the options go before [MAIN]
    boot_quiet(NULL, "# notes\n[MAIN]\nLOG=0\n");
    out = save_now();
    CHECK(out && !strcmp(out, "# notes\nFB=off\nIB=off\nMSAA=off\n[MAIN]\nLOG=0\n"), "before [MAIN]:\n%s", out);

    // CRLF and no newline at the end
    boot_quiet(NULL, "IB=720x408\r\n[MAIN]\r\nLOG=0");
    out = save_now();
    CHECK(out && !strcmp(out, "FB=off\nIB=720x408\nMSAA=off\n[MAIN]\r\nLOG=0\n"), "CRLF file:\n%s", out);
    boot_quiet(NULL, out);
    CHECK(vg_config_get_status()->code == IO_OK && vg_config_get()->ib[0].width == 720 && vg_config_get()->log_enabled == FT_DISABLED,
            "and it parses");

    // A header with a stray CR is read the way the parser reads it
    boot_quiet(NULL, "IB=720x408\n[PCSA00549]\rtrailing\nIB=544x304\n");
    vg_config_get()->ib[0] = (vg_res_t){640, 368};
    out = save_now();
    boot_quiet(NULL, out);
    CHECK(vg_config_get()->ib[0].width == 640, "CR header: the saved IB is used, got %d", vg_config_get()->ib[0].width);

    // A NUL in a line is copied as it is
    static const char nul_file[] = "LOG=0\0x\nIB=640x368\n";
    fs_reset();
    char path[1024];
    stub_map_path(TITLE_TXT, path, sizeof(path));
    char command[1200];
    snprintf(command, sizeof(command), "mkdir -p \"$(dirname %s)\"", path);
    run(command);
    FILE *f = fopen(path, "wb");
    fwrite(nul_file, 1, sizeof(nul_file) - 1, f);
    fclose(f);
    vg_config_parse();
    CHECK(vg_config_get_status()->code == IO_OK, "NUL file parses");
    CHECK(vg_config_save_start(), "started");
    stub_run_threads();
    vg_config_parse();
    CHECK(vg_config_get_status()->code == IO_OK && vg_config_get()->log_enabled == FT_DISABLED, "and still parses after a save");

    // Sections for other executables and versions keep overriding the title-wide options
    snprintf(stub_self_path, sizeof(stub_self_path), "ux0:/app/PCSA00549/game.self");
    boot_quiet(NULL, "[PCSA00549,minigame.self]\nFB=480x272\n[PCSA00549,eboot.bin,0x11111111]\nFB=640x368\n");
    vg_config_get()->fb_enabled = FT_ENABLED;
    vg_config_get()->fb = (vg_res_t){720, 408};
    out = save_now();
    CHECK(out && !strncmp(out, "FB=720x408\n", 11), "title-wide options first:\n%s", out);
    snprintf(g_main.sce_info.path, sizeof(g_main.sce_info.path), "ux0:/app/PCSA00549/minigame.self");
    g_main.tai_info.module_nid = 0x22222222;
    vg_config_parse();
    CHECK(vg_config_get()->fb.width == 480, "minigame.self keeps its FB, got %d", vg_config_get()->fb.width);
    snprintf(g_main.sce_info.path, sizeof(g_main.sce_info.path), "ux0:/app/PCSA00549/eboot.bin");
    g_main.tai_info.module_nid = 0x11111111;
    vg_config_parse();
    CHECK(vg_config_get()->fb.width == 640, "version 0x11111111 keeps its FB, got %d", vg_config_get()->fb.width);
    snprintf(g_main.sce_info.path, sizeof(g_main.sce_info.path), "ux0:/app/PCSA00549/game.self");
    g_main.tai_info.module_nid = 0x91ABDB4F;
    vg_config_parse();
    CHECK(vg_config_get()->fb.width == 720, "this game gets the menu's FB, got %d", vg_config_get()->fb.width);
    snprintf(stub_self_path, sizeof(stub_self_path), "ux0:/patch/PCSA00549/eboot.bin");

    // config.txt is never created
    fs_reset();
    vg_config_parse();
    CHECK(vg_config_get_status()->code == IO_OK && !file_exists(CONFIG_TXT), "no config.txt: defaults, and none created");

    // A line longer than the parser takes: fail, change nothing
    char long_line[2048];
    memset(long_line, '#', 1500);
    strcpy(long_line + 1500, "\n[PCSA00549]\n");
    boot_quiet(NULL, "[PCSA00549]\nIB=720x408\n");
    write_file(TITLE_TXT, long_line);
    CHECK(vg_config_save_start(), "started");
    stub_run_threads();
    CHECK(vg_config_save_get_state() == CONFIG_SAVE_FAILED, "overlong line fails");
    CHECK(!strcmp(read_file(TITLE_TXT), long_line) && !file_exists(TITLE_TMP), "file untouched");
}

static void test_writer_failures() {
    printf("-- config writer failures\n");
    const char *original = OURS "IB=720x408\n# notes\n";

    boot_quiet(NULL, original);
    vg_config_get()->ib[0].width = 640;
    stub_fail_write_at = 1;
    CHECK(vg_config_save_start(), "started");
    stub_run_threads();
    stub_fail_write_at = 0;
    CHECK(vg_config_save_get_state() == CONFIG_SAVE_FAILED, "write failure reported");
    CHECK(!strcmp(read_file(TITLE_TXT), original) && !file_exists(TITLE_TMP), "original kept, temp removed");

    stub_fail_rename_at = 1; // moving the old file aside
    CHECK(vg_config_save_start(), "started");
    stub_run_threads();
    stub_fail_rename_at = 0;
    CHECK(vg_config_save_get_state() == CONFIG_SAVE_FAILED, "first rename failure reported");
    CHECK(!strcmp(read_file(TITLE_TXT), original) && !file_exists(TITLE_TMP), "original kept");

    stub_fail_rename_at = 2; // moving the new file in
    CHECK(vg_config_save_start(), "started");
    stub_run_threads();
    stub_fail_rename_at = 0;
    CHECK(vg_config_save_get_state() == CONFIG_SAVE_FAILED, "second rename failure reported");
    CHECK(read_file(TITLE_TXT) && !strcmp(read_file(TITLE_TXT), original), "original restored");
    CHECK(!file_exists(TITLE_TMP) && !file_exists(TITLE_OLD), "no leftovers");

    stub_fail_open_write = 1;
    CHECK(vg_config_save_start(), "started");
    stub_run_threads();
    stub_fail_open_write = 0;
    CHECK(vg_config_save_get_state() == CONFIG_SAVE_FAILED, "temp file cannot be created");

    // There but unreadable: never replaced
    stub_fail_open_read = 1;
    CHECK(vg_config_save_start(), "started");
    stub_run_threads();
    stub_fail_open_read = 0;
    CHECK(vg_config_save_get_state() == CONFIG_SAVE_FAILED && !strcmp(read_file(TITLE_TXT), original),
            "unreadable file left alone");

    // The rollback failed too (only .vgold is left): the next save puts it back first
    CHECK(vg_config_save_start(), "started");
    stub_renames_before_failing = 1; // moving the new file in fails, and so does the rollback
    stub_run_threads();
    stub_renames_before_failing = -1;
    CHECK(!file_exists(TITLE_TXT) && file_exists(TITLE_OLD), "(only the old file is left)");
    CHECK(vg_config_save_start(), "started");
    stub_run_threads();
    CHECK(vg_config_save_get_state() == CONFIG_SAVE_OK, "saved");
    CHECK(read_file(TITLE_TXT) && strstr(read_file(TITLE_TXT), "# notes"), "old content kept:\n%s", read_file(TITLE_TXT));
    CHECK(!file_exists(TITLE_OLD), "no leftovers");

    // Stopped between the two renames: the next boot finishes the swap
    fs_reset();
    write_file(CONFIG_TXT, "[PCSA00549]\nIB=480x272\n");
    write_file(TITLE_OLD, original);
    write_file(TITLE_TMP, OURS "IB=640x368\n");
    vg_config_parse();
    CHECK(vg_config_get()->ib[0].width == 640, "new file recovered, got %d", vg_config_get()->ib[0].width);
    CHECK(file_exists(TITLE_TXT) && !file_exists(TITLE_OLD) && !file_exists(TITLE_TMP), "swap finished");

    fs_reset();
    write_file(CONFIG_TXT, "[PCSA00549]\nIB=480x272\n");
    write_file(TITLE_OLD, original);
    vg_config_parse();
    CHECK(vg_config_get()->ib[0].width == 720, "old file restored without a temp file");

    fs_reset();
    write_file(CONFIG_TXT, "[PCSA00549]\nIB=480x272\n");
    write_file(TITLE_TMP, OURS "IB=640x368\n");
    vg_config_parse();
    CHECK(vg_config_get()->ib[0].width == 480 && !file_exists(TITLE_TXT), "a lone temp file is ignored");

    // A user's own .bak is nothing to do with saving
    fs_reset();
    write_file(CONFIG_TXT, "[PCSA00549]\nIB=480x272\n");
    write_file(TITLE_TXT ".bak", "[PCSA00549]\nIB=640x368\n");
    vg_config_parse();
    CHECK(vg_config_get()->ib[0].width == 480 && !file_exists(TITLE_TXT), ".bak not restored");
    CHECK(vg_config_save_start(), "started");
    stub_run_threads();
    CHECK(file_exists(TITLE_TXT ".bak"), ".bak not deleted by a save");
}

static void test_osd_timer() {
    printf("-- OSD timer\n");

    // No menu (patch for another version): the banner shows for 5 s, then the hook only forwards
    stub_patch_match = MODULE_NID_MISMATCH;
    boot(CONFIG_BASIC, NULL);
    stub_patch_match = MODULE_MATCH;
    fb_setup(960, 544, 960);
    CHECK(frame() > 0, "banner");
    for (int i = 0; i < 400; i++)
        frame();
    CHECK(frame() == 0 && g_main.osd_done, "latched after 5 s");
    stub_now += 1ull << 32; // the 32-bit process timer wraps every ~71.6 min
    CHECK(frame() == 0, "still hidden after the 32-bit timer wraps");

    // With the menu: after the banner the menu still opens
    boot("[PCSA00549]\nIB=720x408\n", NULL);
    fb_setup(960, 544, 960);
    CHECK(frame() > 0, "banner");
    for (int i = 0; i < 400; i++)
        frame();
    CHECK(frame() == 0, "hidden after 5 s");
    stub_now += 1ull << 32;
    CHECK(frame() == 0, "hidden after the wrap");
    open_menu();
    CHECK(vg_menu_is_open() && frame() > 0, "menu opens and draws after the banner");

    // Bad frames and a busy OSD
    int calls = g_display_calls;
    int (*set_framebuf)(const SceDisplayFrameBuf *, int) = stub_import(NID_DISPLAY);
    set_framebuf(NULL, 1);
    CHECK(g_display_calls == calls + 1 && g_display_last == NULL, "NULL forwarded");
    SceDisplayFrameBuf zero = g_fb_param;
    zero.width = 0;
    set_framebuf(&zero, 1);
    CHECK(fb_changed() == 0 && g_display_calls == calls + 2, "zero width forwarded, not drawn");
    g_main.osd_busy = true;
    CHECK(frame() == 0 && g_display_calls == calls + 3, "busy: forwarded, not drawn");
    g_main.osd_busy = false;
    CHECK(frame() > 0, "drawn again");
}

static void test_error_file() {
    printf("-- the error screen names the broken file\n");
    boot("[PCSA00549]\nIB=720x408\n", "FB=720x408\nOSD=1\nFB=960x544\n=544\n");
    CHECK(vg_config_get_status()->code != IO_OK && vg_config_get_status()->line == 4, "title file line 4");
    CHECK(!strcmp(vg_config_get_error_path(), TITLE_TXT), "path %s", vg_config_get_error_path());
    CHECK(stub_hooks_installed == 1, "no patches, no menu: OSD only");
    fb_setup(960, 544, 960);
    CHECK(frame() > 0, "error drawn");

    boot("[PCSA00549]\nIB=720x408\n=544\n", NULL);
    CHECK(vg_config_get_status()->line == 3 && !strcmp(vg_config_get_error_path(), CONFIG_TXT), "config.txt line 3");
}

static void test_draw_bounds() {
    printf("-- drawing stays inside the frame\n");
    static const int sizes[][3] = {
        {960, 544, 960}, {960, 544, 1024}, {720, 408, 768}, {640, 368, 640}, {480, 272, 512},
        {256, 144, 256}, {64, 32, 64}, {1, 1, 1},
    };

    stub_patch_caps[FEATURE_FPS] = FT_ENABLED;
    boot("[PCSA00549]\nIB=960x544,720x408\n", NULL);
    stub_patch_caps[FEATURE_FPS] = FT_UNSUPPORTED;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        fb_setup(sizes[i][0], sizes[i][1], sizes[i][2]);
        frame(); // banner or notice
        CHECK(fb_padding_clean(), "%dx%d banner inside", sizes[i][0], sizes[i][1]);
        if (!vg_menu_is_open())
            open_menu();
        frame();
        CHECK(vg_menu_is_open(), "open");
        fb_changed();
        tap(SCE_CTRL_DOWN);
        frame();
        CHECK(fb_padding_clean(), "%dx%d menu inside", sizes[i][0], sizes[i][1]);
    }

    // The error banner with the log
    stub_patch_status.code = IO_ERROR_PARSE_INVALID_TOKEN;
    boot(CONFIG_BASIC, NULL);
    for (int i = 0; i < 200; i++)
        vg_log_printf("[TEST] a long log line to fill the error screen %d\n", i);
    vg_log_read(g_osd_buffer, STRING_BUFFER_SIZE);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        fb_setup(sizes[i][0], sizes[i][1], sizes[i][2]);
        CHECK(frame() > 0 || sizes[i][0] < 32, "error drawn");
        CHECK(fb_padding_clean(), "%dx%d error inside", sizes[i][0], sizes[i][1]);
    }
    stub_patch_status.code = IO_OK;
}

int main(int argc, char **argv) {
    if (argc > 1)
        snprintf(stub_root, sizeof(stub_root), "%s", argv[1]);

    test_install();
    test_closed_passthrough();
    test_open_masks();
    test_close_held();
    test_stale();
    test_values();
    test_save_flow();
    test_writer();
    test_writer_failures();
    test_osd_timer();
    test_error_file();
    test_draw_bounds();

    free(g_fb);
    for (int i = 0; i < g_read_count; i++)
        free(g_reads[i]);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
