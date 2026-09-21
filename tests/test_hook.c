/*
 * Host tests for the '>' hook directives in src/patch_hook.c, in particular
 * '>rateDivide()': what parses, what is refused, what gets installed at each
 * frame rate setting, and which calls a rate divided hook actually makes.
 *
 * src/patch_hook.c and src/io.c are compiled against the stub vitasdk/taihen
 * headers in tests/stubs; taiHEN itself is faked here so the installed hook
 * body can be called directly.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vitasdk.h>
#include <taihen.h>

#include "../src/io.h"
#include "../src/config.h"
#include "../src/patch.h"
#include "../src/patch_hook.h"
#include "../src/main.h"
#include "../src/interpreter/interpreter.h"

// ---------------------------------------------------------------- plugin glue

vg_main_t g_main;

static vg_config_t g_config;
vg_config_t *vg_config_get() { return &g_config; }

static bool g_verbose = false;
void vg_log_printf(const char *format, ...) {
    if (!g_verbose)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

// src/io.c needs these to link; no test here touches the filesystem
SceUID sceIoOpen(const char *file, int flags, int mode) { (void)file; (void)flags; (void)mode; return -1; }
int sceIoClose(SceUID fd) { (void)fd; return 0; }
int sceIoRead(SceUID fd, void *data, SceSize size) { (void)fd; (void)data; (void)size; return 0; }
int sceIoWrite(SceUID fd, const void *data, SceSize size) { (void)fd; (void)data; (void)size; return size; }
long long sceIoLseek(SceUID fd, long long offset, int whence) { (void)fd; (void)offset; (void)whence; return 0; }
int sceIoMkdir(const char *dir, int mode) { (void)dir; (void)mode; return 0; }
int sceDisplayWaitVblankStartMulti(unsigned int vcount) { (void)vcount; return 0; }
int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad_data, int count) { (void)port; (void)pad_data; return count; }
int sceCtrlPeekBufferPositive2(int port, SceCtrlData *pad_data, int count) { (void)port; (void)pad_data; return count; }

// ------------------------------------------------------------------ fake taiHEN

#define FAKE_HOOK_MAX 64
#define FAKE_ORIGINAL_RET 0x5EED

typedef struct {
    SceUID uid;
    bool is_import;
    uint32_t nid;
    int segidx;
    uint32_t offset;
    int thumb;
    const void *func;
    struct _tai_hook_user node;
    bool released;
} fake_hook_t;

static fake_hook_t g_fake[FAKE_HOOK_MAX];
static int g_fake_num;
static int g_fake_fail_next;       // next taiHook* call fails when set

// Stands in for the hooked game function
static int g_original_calls;
static int g_original_last_args[4];
static int g_import_original_calls;
static int fake_import_original(int a0, int a1, int a2, int a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    g_import_original_calls++;
    return 0;
}
static int fake_original(int a0, int a1, int a2, int a3) {
    g_original_calls++;
    g_original_last_args[0] = a0;
    g_original_last_args[1] = a1;
    g_original_last_args[2] = a2;
    g_original_last_args[3] = a3;
    return FAKE_ORIGINAL_RET;
}

static fake_hook_t *fake_hook_new(const void *hook_func, tai_hook_ref_t *p_hook, bool is_import) {
    fake_hook_t *hook = &g_fake[g_fake_num];
    memset(hook, 0, sizeof(*hook));
    hook->uid = 0x1000 + g_fake_num;
    hook->func = hook_func;
    hook->node.next = 0;
    hook->node.func = (void *)hook_func;
    hook->node.old = is_import ? (void *)&fake_import_original : (void *)&fake_original;
    hook->is_import = is_import;
    *p_hook = (tai_hook_ref_t)&hook->node;
    g_fake_num++;
    return hook;
}

SceUID taiHookFunctionImport(tai_hook_ref_t *p_hook, void *module, uint32_t library_nid,
        uint32_t func_nid, const void *hook_func) {
    (void)module; (void)library_nid;
    if (g_fake_fail_next) { g_fake_fail_next = 0; return -1; }
    if (g_fake_num >= FAKE_HOOK_MAX) return -1;
    fake_hook_t *hook = fake_hook_new(hook_func, p_hook, true);
    hook->nid = func_nid;
    return hook->uid;
}

SceUID taiHookFunctionOffset(tai_hook_ref_t *p_hook, SceUID modid, int segidx, uint32_t offset,
        int thumb, const void *hook_func) {
    (void)modid;
    if (g_fake_fail_next) { g_fake_fail_next = 0; return -1; }
    if (g_fake_num >= FAKE_HOOK_MAX) return -1;
    fake_hook_t *hook = fake_hook_new(hook_func, p_hook, false);
    hook->segidx = segidx;
    hook->offset = offset;
    hook->thumb = thumb;
    return hook->uid;
}

int taiHookRelease(SceUID tai_uid, tai_hook_ref_t hook) {
    (void)hook;
    for (int i = 0; i < g_fake_num; i++) {
        if (g_fake[i].uid == tai_uid) {
            g_fake[i].released = true;
            return 0;
        }
    }
    return -1;
}

// What src/main.c's module_stop does with the hooks it owns
static void release_all_hooks() {
    for (uint32_t i = g_main.rate_hook_num; i > 0; i--) {
        if (g_main.rate_hook[i - 1].uid >= 0) {
            taiHookRelease(g_main.rate_hook[i - 1].uid, g_main.rate_hook[i - 1].ref);
            g_main.rate_hook[i - 1].uid = -1;
            g_main.rate_hook[i - 1].divisor = 0;
        }
    }
    g_main.rate_hook_num = 0;
    for (uint8_t i = MAX_HOOK_NUM; i > 0; i--) {
        if (g_main.hook[i - 1] >= 0)
            taiHookRelease(g_main.hook[i - 1], g_main.hook_ref[i - 1]);
    }
}

// --------------------------------------------------------------------- harness

static int g_fails;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

static int fake_offset_hook_num() {
    int n = 0;
    for (int i = 0; i < g_fake_num; i++) {
        if (!g_fake[i].is_import)
            n++;
    }
    return n;
}

static int fake_import_hook_num(uint32_t nid) {
    int n = 0;
    for (int i = 0; i < g_fake_num; i++) {
        if (g_fake[i].is_import && g_fake[i].nid == nid)
            n++;
    }
    return n;
}

// Mirrors vg_patch_set_interpreter_context() for the frame rate part
static void set_fps(vg_feature_state_t enabled, vg_fps_t fps) {
    intp_vg_context_t context = {0};
    context.fb_width = 960;
    context.fb_height = 544;
    for (int i = 0; i < INTP_VG_MAX_RES_COUNT; i++) {
        context.ib_width[i] = 960;
        context.ib_height[i] = 544;
    }
    switch (fps) {
        case FPS_30: context.vblank = 2; context.fps_limit = 30; break;
        case FPS_20: context.vblank = 3; context.fps_limit = 20; break;
        case FPS_60:
        default:     context.vblank = 1; context.fps_limit = 60; break;
    }
    context.msaa = 2;
    context.msaa_enabled = true;
    intp_set_vg_context(&context);

    memset(&g_config, 0, sizeof(g_config));
    g_config.enabled = FT_ENABLED;
    g_config.fps_enabled = enabled;
    g_config.fps = fps;
}

static void reset(vg_feature_state_t fps_enabled, vg_fps_t fps) {
    memset(&g_main, 0, sizeof(g_main));
    for (int i = 0; i < MAX_HOOK_NUM; i++) {
        g_main.hook[i] = -1;
    }
    for (int i = 0; i < MAX_RATE_HOOK_NUM; i++) {
        g_main.rate_hook[i].uid = -1;
    }
    memset(g_fake, 0, sizeof(g_fake));
    g_fake_num = 0;
    g_fake_fail_next = 0;
    g_original_calls = 0;
    g_import_original_calls = 0;
    set_fps(fps_enabled, fps);
}

static vg_io_status_code_t parse(const char *line) {
    return vg_hook_parse_patch(line).code;
}

// Calls the body that was hooked over the game function, as the game would
static int call_game_function(int slot_index, int a0) {
    int seen = -1;
    for (int i = 0; i < g_fake_num; i++) {
        if (g_fake[i].is_import)
            continue;
        if (++seen == slot_index)
            return ((int (*)(int, int, int, int))g_fake[i].func)(a0, 0, 0, 0);
    }
    CHECK(false, "no rate divided hook at index %d", slot_index);
    return 0;
}

// Calls the frame counter hook, as sceDisplaySetFrameBuf would
static void present_frame() {
    for (int i = 0; i < g_fake_num; i++) {
        if (g_fake[i].is_import && g_fake[i].nid == 0x7A410B64) {
            SceDisplayFrameBuf fb = {0};
            ((int (*)(const SceDisplayFrameBuf *, int))g_fake[i].func)(&fb, 1);
            return;
        }
    }
    CHECK(false, "no frame counter hook installed");
}

// ----------------------------------------------------------------------- tests

static void test_existing_directives() {
    reset(FT_ENABLED, FPS_30);
    CHECK(parse(">sceDisplaySetFrameBuf_withWait()") == IO_OK, "withWait parses");
    CHECK(fake_import_hook_num(0x7A410B64) == 1, "withWait hooks at FPS=30");

    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">sceDisplaySetFrameBuf_withWait()") == IO_OK, "withWait parses at 60");
    CHECK(g_fake_num == 0, "withWait does not hook at FPS=60");

    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">sceCtrlReadBufferPositive_peekPatched()") == IO_OK, "peekPatched parses");
    CHECK(fake_import_hook_num(0x67E7AB83) == 1, "peekPatched hooks at FPS=60");
    CHECK(parse(">sceCtrlReadBufferPositive2_peekPatched()") == IO_OK, "peekPatched2 parses");
    CHECK(fake_import_hook_num(0xC4226A3E) == 1, "peekPatched2 hooks at FPS=60");

    // A repeat must not overwrite the slot, or its TAI_CONTINUE re-enters itself
    CHECK(parse(">sceCtrlReadBufferPositive_peekPatched()") == IO_OK, "repeat is not an error");
    CHECK(fake_import_hook_num(0x67E7AB83) == 1, "repeat does not hook twice");

    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">nonsense()") == IO_ERROR_PARSE_INVALID_TOKEN, "unknown directive is refused");
}

static void test_rate_divide_install() {
    // A 30 FPS game unlocked to 60: 1 call in 2
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x143CB8, fps_limit / 30, ret=1)") == IO_OK, "rateDivide parses");
    CHECK(fake_offset_hook_num() == 1, "rateDivide installs one offset hook");
    CHECK(fake_import_hook_num(0x7A410B64) == 1, "rateDivide installs the frame counter");
    CHECK(g_main.rate_hook_num == 1, "one slot used");
    CHECK(g_main.rate_hook[0].divisor == 2, "divisor is 2 at FPS=60 (got %u)", g_main.rate_hook[0].divisor);
    CHECK(g_main.rate_hook[0].ret_value == 1, "substitute return is 1");
    CHECK(g_main.rate_hook[0].segment == 0 && g_main.rate_hook[0].offset == 0x143CB8, "address recorded");
    CHECK(g_fake[1].segidx == 0 && g_fake[1].offset == 0x143CB8, "hooked by segment and offset");
    CHECK(g_fake[1].thumb == 1, "thumb by default");
    CHECK(g_main.rate_hook[0].uid >= 0, "slot holds the hook uid");

    // The same game at its own rate: nothing is hooked at all
    reset(FT_ENABLED, FPS_30);
    CHECK(parse(">rateDivide(0:0x143CB8, fps_limit / 30, ret=1)") == IO_OK, "parses at FPS=30");
    CHECK(g_fake_num == 0, "nothing is hooked at the game's native rate");
    CHECK(g_main.rate_hook_num == 0, "no slot used at the native rate");

    // Below the native rate the game is already slower than it should be
    reset(FT_ENABLED, FPS_20);
    CHECK(parse(">rateDivide(0:0x143CB8, fps_limit / 30, ret=1)") == IO_OK, "parses at FPS=20");
    CHECK(g_fake_num == 0, "nothing is hooked at FPS=20 for a 30 FPS game");

    // A 20 FPS game unlocked to 60 needs 1 call in 3
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, fps_limit / 20, ret=1)") == IO_OK, "20 FPS game parses");
    CHECK(g_main.rate_hook[0].divisor == 3, "divisor is 3 (got %u)", g_main.rate_hook[0].divisor);

    // The vblank form of the same thing for a 30 FPS game
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2 / vblank, ret=1)") == IO_OK, "vblank form parses");
    CHECK(g_main.rate_hook[0].divisor == 2, "vblank form gives 2 at FPS=60 (got %u)", g_main.rate_hook[0].divisor);
    reset(FT_ENABLED, FPS_30);
    CHECK(parse(">rateDivide(0:0x1000, 2 / vblank, ret=1)") == IO_OK, "vblank form parses at 30");
    CHECK(g_fake_num == 0, "vblank form is inert at the native rate");

    // A ratio that is not a whole number of frames cannot be divided at all:
    // a 20 FPS game asked to run at 30 rounds down to 1 and stays unhooked
    reset(FT_ENABLED, FPS_30);
    CHECK(parse(">rateDivide(0:0x1000, fps_limit / 20, ret=1)") == IO_OK, "1.5x ratio parses");
    CHECK(g_fake_num == 0, "a 1.5x ratio installs nothing");

    // The whole feature is off when the frame rate option is
    reset(FT_DISABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x143CB8, fps_limit / 30, ret=1)") == IO_OK, "parses with FPS off");
    CHECK(g_fake_num == 0, "nothing is hooked while the FPS option is off");

    // ARM target
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(1:0x2000, 2, void, arm)") == IO_OK, "arm form parses");
    CHECK(g_fake[1].thumb == 0 && g_fake[1].segidx == 1, "arm target in segment 1");

    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide( 0:0x2000 , 2 , ret = 1 + 1 , thumb )") == IO_OK, "whitespace is tolerated");
    CHECK(g_main.rate_hook[0].ret_value == 2, "expression as the substitute value");

    // Failure from taiHEN is reported, not swallowed
    reset(FT_ENABLED, FPS_60);
    g_fake_fail_next = 1;
    CHECK(parse(">rateDivide(0:0x1000, 2, void)") == IO_ERROR_TAI_GENERIC, "frame counter failure is reported");
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, void)") == IO_OK, "first hook ok");
    g_fake_fail_next = 1;
    CHECK(parse(">rateDivide(0:0x2000, 2, void)") == IO_ERROR_TAI_GENERIC, "offset hook failure is reported");
    CHECK(g_main.rate_hook_num == 1, "a failed hook does not take a slot");
}

static void test_rate_divide_parse_errors() {
    reset(FT_ENABLED, FPS_60);
    struct { const char *line; vg_io_status_code_t code; const char *what; } cases[] = {
        {">rateDivide", IO_ERROR_PARSE_INVALID_TOKEN, "no argument list"},
        {">rateDivide()", IO_ERROR_PARSE_INVALID_TOKEN, "empty argument list"},
        {">rateDivide(0x143CB8, 2, void)", IO_ERROR_PARSE_INVALID_TOKEN, "offset without a segment"},
        {">rateDivide(0:0x143CB8)", IO_ERROR_PARSE_INVALID_TOKEN, "no divisor"},
        {">rateDivide(0:0x143CB8, 2)", IO_ERROR_PARSE_INVALID_TOKEN, "return handling left unsaid"},
        {">rateDivide(0:0x143CB8, 2,)", IO_ERROR_PARSE_INVALID_TOKEN, "empty return handling"},
        {">rateDivide(0:0x143CB8, 2, 1)", IO_ERROR_PARSE_INVALID_TOKEN, "bare value is not a return spec"},
        {">rateDivide(0:0x143CB8, 2, rot=1)", IO_ERROR_PARSE_INVALID_TOKEN, "misspelled return spec"},
        {">rateDivide(0:0x143CB8, 2, ret)", IO_ERROR_PARSE_INVALID_TOKEN, "ret without a value"},
        {">rateDivide(0:0x143CB8, 2, ret=)", IO_ERROR_INTERPRETER_ERROR, "ret with an empty value"},
        {">rateDivide(0:0x143CB8, 2, voids)", IO_ERROR_PARSE_INVALID_TOKEN, "void with trailing junk"},
        {">rateDivide(0:0x143CB8, 2, void", IO_ERROR_PARSE_INVALID_TOKEN, "unclosed argument list"},
        {">rateDivide(0:0x143CB8, 2, void) 1", IO_ERROR_PARSE_INVALID_TOKEN, "junk after the directive"},
        {">rateDivide(0:0x143CB8, 2, void, neon)", IO_ERROR_PARSE_INVALID_TOKEN, "unknown instruction set"},
        {">rateDivide(0:0x143CB8, 2, void, arm x)", IO_ERROR_PARSE_INVALID_TOKEN, "junk after the instruction set"},
        {">rateDivide(0:0x143CB8, 99, void)", IO_ERROR_PARSE_INVALID_TOKEN, "divisor above the limit"},
        {">rateDivide(0:0x143CB8, 0 - 1, void)", IO_ERROR_PARSE_INVALID_TOKEN, "negative divisor"},
        {">rateDivide(0:0x143CB8, 2.0, void)", IO_ERROR_PARSE_INVALID_TOKEN, "float divisor"},
        {">rateDivide(0:0x143CB8, fps_limit /, void)", IO_ERROR_INTERPRETER_ERROR, "broken divisor expression"},
        {">rateDivide(0:0x143CB8, ib_wi(99), void)", IO_ERROR_INTERPRETER_ERROR, "interpreter error is reported"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset(FT_ENABLED, FPS_60);
        vg_io_status_code_t code = parse(cases[i].line);
        CHECK(code == cases[i].code, "%s: expected %d, got %d (%s)",
                    cases[i].what, cases[i].code, code, cases[i].line);
        CHECK(g_fake_num == 0 || code == IO_OK, "%s: nothing is hooked on a parse error", cases[i].what);
    }

    // A line that parses must still be accepted after a comment is stripped
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x143CB8, 2, void) # minigame 45") == IO_OK, "trailing comment is allowed");
}

static void test_rate_divide_calls() {
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x143CB8, fps_limit / 30, ret=1)") == IO_OK, "install for call test");

    // Frame 0: the call is made and the game gets the real return value
    CHECK(call_game_function(0, 0x11) == FAKE_ORIGINAL_RET, "frame 0 call reaches the game");
    CHECK(g_original_calls == 1, "frame 0 calls the original once");
    CHECK(g_original_last_args[0] == 0x11, "arguments are carried through");

    // Every call in the same frame agrees, including a second one
    CHECK(call_game_function(0, 0x11) == FAKE_ORIGINAL_RET, "second call in frame 0 also runs");
    CHECK(g_original_calls == 2, "two calls in frame 0");

    // Frame 1: skipped, and the substitute value is returned
    present_frame();
    CHECK(g_main.frame == 1, "the frame counter advances");
    CHECK(g_import_original_calls == 1, "the frame counter chains to the original");
    CHECK(g_original_calls == 2, "presenting a frame is not a game function call");
    CHECK(call_game_function(0, 0x22) == 1, "frame 1 returns the substitute");
    CHECK(call_game_function(0, 0x22) == 1, "every call in frame 1 is skipped");
    CHECK(g_original_calls == 2, "frame 1 makes no call");

    // Frame 2: back to a real call
    present_frame();
    CHECK(call_game_function(0, 0x33) == FAKE_ORIGINAL_RET, "frame 2 calls the game again");
    CHECK(g_original_calls == 3, "half of the frames call the original");

    // 10 more frames, 5 more calls
    for (int i = 0; i < 10; i++) {
        present_frame();
        call_game_function(0, 0);
    }
    CHECK(g_original_calls == 3 + 5, "1 call in 2 over 10 frames (got %d)", g_original_calls - 3);

    // 'void' hands back 0
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x12465E, 2, void)") == IO_OK, "install void target");
    present_frame();
    CHECK(call_game_function(0, 0) == 0, "a skipped void call returns 0");
    CHECK(g_original_calls == 0, "the skipped void call is not made");

    // 1 call in 3
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 3, ret=1)") == IO_OK, "install 1 in 3");
    for (int i = 0; i < 9; i++) {
        call_game_function(0, 0);
        present_frame();
    }
    CHECK(g_original_calls == 3, "1 call in 3 over 9 frames (got %d)", g_original_calls);

    // Two hooks share the frame counter and skip the same frames
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=1)") == IO_OK, "install first");
    CHECK(parse(">rateDivide(0:0x2000, 2, ret=1)") == IO_OK, "install second");
    CHECK(fake_import_hook_num(0x7A410B64) == 1, "the frame counter is installed once");
    CHECK(fake_offset_hook_num() == 2, "two offset hooks");
    call_game_function(0, 0);
    call_game_function(1, 0);
    CHECK(g_original_calls == 2, "both run in frame 0");
    present_frame();
    call_game_function(0, 0);
    call_game_function(1, 0);
    CHECK(g_original_calls == 2, "both skip frame 1");
}

static void test_slots() {
    // The same site twice would leak the first hook and re-enter it forever
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x143CB8, 2, ret=1)") == IO_OK, "first install");
    CHECK(parse(">rateDivide(0:0x143CB8, 2, ret=1)") == IO_OK, "repeat is not an error");
    CHECK(fake_offset_hook_num() == 1, "the same site is not hooked twice");
    CHECK(g_main.rate_hook_num == 1, "the repeat takes no slot");
    // ...but a different site in the same segment is a different hook
    CHECK(parse(">rateDivide(0:0x143CBA, 2, ret=1)") == IO_OK, "neighbouring site installs");
    CHECK(fake_offset_hook_num() == 2, "two distinct sites");
    // ...and so is the same offset in another segment
    CHECK(parse(">rateDivide(1:0x143CB8, 2, ret=1)") == IO_OK, "same offset in segment 1 installs");
    CHECK(fake_offset_hook_num() == 3, "segment is part of the identity");

    // The pool has a fixed size and says so rather than running past it
    reset(FT_ENABLED, FPS_60);
    char line[128];
    for (int i = 0; i < MAX_RATE_HOOK_NUM; i++) {
        snprintf(line, sizeof(line), ">rateDivide(0:0x%X, 2, void)", 0x1000 + i * 4);
        CHECK(parse(line) == IO_OK, "slot %d installs", i);
    }
    CHECK(g_main.rate_hook_num == MAX_RATE_HOOK_NUM, "the pool is full");
    snprintf(line, sizeof(line), ">rateDivide(0:0x%X, 2, void)", 0x1000 + MAX_RATE_HOOK_NUM * 4);
    CHECK(parse(line) == IO_ERROR_TOO_MANY_PATCHES, "one past the pool is refused");

    // Every slot has its own wrapper, so no two slots share a hook body
    for (int i = 0; i < g_fake_num; i++) {
        for (int j = i + 1; j < g_fake_num; j++) {
            CHECK(g_fake[i].func != g_fake[j].func, "hook bodies %d and %d are distinct", i, j);
        }
    }

    // ...and every slot's own wrapper drives its own slot
    for (int i = 0; i < MAX_RATE_HOOK_NUM; i++) {
        g_original_calls = 0;
        present_frame();                       // odd frame: everything skips
        CHECK(call_game_function(i, 0) == 0, "slot %d skips", i);
        CHECK(g_original_calls == 0, "slot %d makes no call while skipping", i);
        present_frame();                       // even frame: everything runs
        CHECK(call_game_function(i, 0) == FAKE_ORIGINAL_RET, "slot %d runs", i);
        CHECK(g_original_calls == 1, "slot %d calls the original once", i);
    }

    // module_stop releases every hook it took
    release_all_hooks();
    for (int i = 0; i < g_fake_num; i++) {
        CHECK(g_fake[i].released, "hook %d released", i);
    }
    CHECK(g_main.rate_hook_num == 0, "the pool is empty again");
}

// The address parser is shared by patch lines and by hook directives
static void test_parse_address() {
    struct { const char *line; bool ok; uint8_t segment; uint32_t offset; int end; } cases[] = {
        {"0:0x143CB8 t1_mov(r0,1)", true,  0, 0x143CB8, 10},
        {"1:0x0 nop",               true,  1, 0x0,      5},
        {"255:4294967295 nop",      true,  255, 0xFFFFFFFF, 14},
        {"12:0x10,",                true,  12, 0x10,     7},
        {"0:0x10)",                 true,  0, 0x10,      6},
        {"256:0x10 nop",            false, 0, 0,         0},
        {"0x0:0x10 nop",            false, 0, 0,         0},
        {"0 0x10 nop",              false, 0, 0,         0},
        {":0x10 nop",               false, 0, 0,         0},
        {"0: nop",                  false, 0, 0,         0},
        {"0:4294967296 nop",        false, 0, 0,         0},
        {"0:-1 nop",                false, 0, 0,         0},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t segment = 0xAA;
        uint32_t offset = 0xDEADBEEF;
        int pos = 0;
        vg_io_status_t ret = vg_io_parse_address(cases[i].line, &pos, &segment, &offset);
        CHECK((ret.code == IO_OK) == cases[i].ok, "address '%s' %s", cases[i].line,
                    cases[i].ok ? "parses" : "is refused");
        if (!cases[i].ok || ret.code != IO_OK)
            continue;
        CHECK(segment == cases[i].segment, "address '%s': segment %u", cases[i].line, segment);
        CHECK(offset == cases[i].offset, "address '%s': offset 0x%X", cases[i].line, offset);
        CHECK(pos == cases[i].end, "address '%s': stops at %d (expected %d)",
                    cases[i].line, pos, cases[i].end);
    }
}

// The shape a real patch uses: a 30 FPS game whose per frame minigame updates
// must keep their own rate while the display runs at 60 (offsets from the
// PCSG00246 study - updates whose result is read return 1, the second per
// frame functions have their result discarded)
static void test_patch_shaped_lines() {
    static const char *lines[] = {
        ">rateDivide(0:0x143CB8, fps_limit / 30, ret=1)  # minigame 45 update",
        ">rateDivide(0:0x12276C, fps_limit / 30, ret=1)  # minigame 3 update",
        ">rateDivide(0:0x12465E, fps_limit / 30, void)   # minigame 3, result discarded",
        ">rateDivide(0:0x1434B0, fps_limit / 30, ret=1)  # minigame 44 update",
        ">rateDivide(0:0x44A8C, fps_limit / 30, void)    # battle script step wait"
    };
    const int line_num = sizeof(lines) / sizeof(lines[0]);

    reset(FT_ENABLED, FPS_60);
    for (int i = 0; i < line_num; i++) {
        CHECK(parse(lines[i]) == IO_OK, "patch line %d installs", i);
    }
    CHECK(fake_offset_hook_num() == line_num, "every line took a hook");
    CHECK(fake_import_hook_num(0x7A410B64) == 1, "one frame counter for all of them");
    for (int i = 0; i < line_num; i++) {
        CHECK(g_main.rate_hook[i].divisor == 2, "line %d runs 1 call in 2", i);
    }
    CHECK(g_main.rate_hook[2].ret_value == 0 && g_main.rate_hook[0].ret_value == 1,
                "void and ret=1 are kept apart");

    // The same file with the frame rate left at the game's own 30
    reset(FT_ENABLED, FPS_30);
    for (int i = 0; i < line_num; i++) {
        CHECK(parse(lines[i]) == IO_OK, "patch line %d parses at FPS=30", i);
    }
    CHECK(g_fake_num == 0, "a 30 FPS run hooks nothing at all");
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v"))
            g_verbose = true;
    }

    test_parse_address();
    test_existing_directives();
    test_rate_divide_install();
    test_rate_divide_parse_errors();
    test_rate_divide_calls();
    test_slots();
    test_patch_shaped_lines();

    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
