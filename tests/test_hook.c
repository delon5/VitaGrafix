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
    // Disarm first, so a call that is already inside a wrapper cannot follow a
    // chain that is being released
    for (uint32_t i = 0; i < g_main.rate_hook_num; i++) {
        g_main.rate_hook[i].armed = false;
    }
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

// The same, for checks made thousands at a time inside a loop: every one is
// counted, but only the first failure of the run is printed
#define CHECK_QUIET(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        if (!g_fails) { printf("FAIL: " __VA_ARGS__); printf("\n"); } \
        g_fails++; \
    } \
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

// Stands in for what sceKernelGetModuleInfo() reports about the game, which
// is what bounds a hook target
#define TEST_SEG0_SIZE 0x200000
#define TEST_SEG1_SIZE 0x10000

static void set_module_segments() {
    g_main.sce_info.segments[0].memsz = TEST_SEG0_SIZE;
    g_main.sce_info.segments[1].memsz = TEST_SEG1_SIZE;
}

static void reset(vg_feature_state_t fps_enabled, vg_fps_t fps) {
    memset(&g_main, 0, sizeof(g_main));
    for (int i = 0; i < MAX_HOOK_NUM; i++) {
        g_main.hook[i] = -1;
    }
    for (int i = 0; i < MAX_RATE_HOOK_NUM; i++) {
        g_main.rate_hook[i].uid = -1;
    }
    set_module_segments();
    memset(g_fake, 0, sizeof(g_fake));
    g_fake_num = 0;
    g_fake_fail_next = 0;
    g_original_calls = 0;
    g_import_original_calls = 0;
    set_fps(fps_enabled, fps);
}

// A '>' line is only ever reached under an enabled feature block, and which
// block that is decides whether the directive is valid at all
static vg_io_status_code_t parse_under(const char *line, vg_feature_t feature) {
    return vg_hook_parse_patch(line, feature).code;
}

static vg_io_status_code_t parse(const char *line) {
    return parse_under(line, FEATURE_FPS);
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

// A '>' directive under any other feature block used to parse, report IO_OK
// and install nothing at all, for the life of the patch file
static void test_feature_gating() {
    static const char *lines[] = {
        ">rateDivide(0:0x143CB8, 2, ret=1)",
        ">sceDisplaySetFrameBuf_withWait()",
        ">sceCtrlReadBufferPositive_peekPatched()",
        ">nonsense()"
    };
    static const vg_feature_t features[] = {FEATURE_FB, FEATURE_IB, FEATURE_MSAA};

    for (size_t f = 0; f < sizeof(features) / sizeof(features[0]); f++) {
        for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
            reset(FT_ENABLED, FPS_60);
            CHECK(parse_under(lines[i], features[f]) == IO_ERROR_HOOK_WRONG_FEATURE,
                        "'%s' under feature %d is refused", lines[i], features[f]);
            CHECK(g_fake_num == 0, "'%s' under feature %d hooks nothing", lines[i], features[f]);
        }
    }

    // ...and the same lines under @FPS are exactly as before
    reset(FT_ENABLED, FPS_60);
    CHECK(parse_under(">rateDivide(0:0x143CB8, 2, ret=1)", FEATURE_FPS) == IO_OK, "@FPS installs it");
    CHECK(fake_offset_hook_num() == 1, "@FPS really hooks");
}

static void test_rate_divide_install() {
    // A 30 FPS game unlocked to 60: 1 call in 2
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x143CB8, fps_limit / 30, ret=1)") == IO_OK, "rateDivide parses");
    CHECK(fake_offset_hook_num() == 1, "rateDivide installs one offset hook");
    CHECK(g_main.rate_hook_num == 1, "one slot used");
    CHECK(g_main.rate_hook[0].divisor == 2, "divisor is 2 at FPS=60 (got %u)", g_main.rate_hook[0].divisor);
    CHECK(g_main.rate_hook[0].ret_value == 1, "substitute return is 1");
    CHECK(g_main.rate_hook[0].frame_counted == false, "calls are counted by default");
    CHECK(g_main.rate_hook[0].armed, "the slot is armed once the hook is installed");
    CHECK(g_main.rate_hook[0].segment == 0 && g_main.rate_hook[0].offset == 0x143CB8, "address recorded");
    CHECK(g_fake[0].segidx == 0 && g_fake[0].offset == 0x143CB8, "hooked by segment and offset");
    CHECK(g_fake[0].thumb == 1, "thumb by default");
    CHECK(g_main.rate_hook[0].uid >= 0, "slot holds the hook uid");

    // Counting calls costs nothing outside the hooked function itself
    CHECK(fake_import_hook_num(0x7A410B64) == 0, "a call counted hook installs no frame counter");
    CHECK(g_main.hook[HOOK_RATE_FRAME_COUNTER] < 0, "...and takes no import hook slot");

    // The same game at its own rate: nothing is hooked at all
    reset(FT_ENABLED, FPS_30);
    CHECK(parse(">rateDivide(0:0x143CB8, fps_limit / 30, ret=1)") == IO_OK, "parses at FPS=30");
    CHECK(g_fake_num == 0, "nothing is hooked at the game's native rate");
    CHECK(g_main.rate_hook_num == 0, "no slot used at the native rate");

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

    // The divisor idiom the studies recommend is defined at every setting:
    // 1 at FPS 20 and 30 (inert), 2 at FPS 60
    reset(FT_ENABLED, FPS_20);
    CHECK(parse(">rateDivide(0:0x1000, 1 + (fps_limit / 60), ret=1)") == IO_OK, "'1 + fps/60' at FPS=20");
    CHECK(g_fake_num == 0, "...installs nothing at FPS=20");
    reset(FT_ENABLED, FPS_30);
    CHECK(parse(">rateDivide(0:0x1000, 1 + (fps_limit / 60), ret=1)") == IO_OK, "'1 + fps/60' at FPS=30");
    CHECK(g_fake_num == 0, "...installs nothing at FPS=30");
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 1 + (fps_limit / 60), ret=1)") == IO_OK, "'1 + fps/60' at FPS=60");
    CHECK(g_main.rate_hook[0].divisor == 2, "...and 1 call in 2 at FPS=60");

    // The whole feature is off when the frame rate option is
    reset(FT_DISABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x143CB8, fps_limit / 30, ret=1)") == IO_OK, "parses with FPS off");
    CHECK(g_fake_num == 0, "nothing is hooked while the FPS option is off");

    // ARM target
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(1:0x2000, 2, void, arm)") == IO_OK, "arm form parses");
    CHECK(g_fake[0].thumb == 0 && g_fake[0].segidx == 1, "arm target in segment 1");

    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide( 0:0x2000 , 2 , ret = 1 + 1 , thumb )") == IO_OK, "whitespace is tolerated");
    CHECK(g_main.rate_hook[0].ret_value == 2, "expression as the substitute value");

    // The optional tokens are order independent
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x2000, 2, void, frame, arm, args=2)") == IO_OK, "frame, arm, args");
    CHECK(g_main.rate_hook[0].frame_counted && g_fake[1].thumb == 0, "...all three are taken");
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x2000, 2, void, args=1, call, thumb)") == IO_OK, "args, call, thumb");
    CHECK(!g_main.rate_hook[0].frame_counted && g_fake[0].thumb == 1, "...all three are taken");

    // Failure from taiHEN is reported, not swallowed
    reset(FT_ENABLED, FPS_60);
    g_fake_fail_next = 1;
    CHECK(parse(">rateDivide(0:0x1000, 2, void, frame)") == IO_ERROR_TAI_GENERIC, "frame counter failure is reported");
    reset(FT_ENABLED, FPS_60);
    g_fake_fail_next = 1;
    CHECK(parse(">rateDivide(0:0x1000, 2, void)") == IO_ERROR_TAI_GENERIC, "offset hook failure is reported");
    CHECK(g_main.rate_hook_num == 0, "a failed hook does not take a slot");
    CHECK(!g_main.rate_hook[0].armed, "a failed hook leaves the slot disarmed");
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, void)") == IO_OK, "first hook ok");
    g_fake_fail_next = 1;
    CHECK(parse(">rateDivide(0:0x2000, 2, void)") == IO_ERROR_TAI_GENERIC, "second offset hook failure");
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
        {">rateDivide(0:0x143CB8, 2, void, arm, thumb)", IO_ERROR_PARSE_INVALID_TOKEN, "two instruction sets"},
        {">rateDivide(0:0x143CB8, 2, void, arm, arm)", IO_ERROR_PARSE_INVALID_TOKEN, "instruction set twice"},
        {">rateDivide(0:0x143CB8, 2, void, call, frame)", IO_ERROR_PARSE_INVALID_TOKEN, "two counting modes"},
        {">rateDivide(0:0x143CB8, 2, void, frame, frame)", IO_ERROR_PARSE_INVALID_TOKEN, "counting mode twice"},
        {">rateDivide(0:0x143CB8, 2, void, args=2, args=2)", IO_ERROR_PARSE_INVALID_TOKEN, "arity twice"},
        {">rateDivide(0:0x143CB8, 2, void, args)", IO_ERROR_PARSE_INVALID_TOKEN, "args without a value"},
        {">rateDivide(0:0x143CB8, 2, void, framed)", IO_ERROR_PARSE_INVALID_TOKEN, "junk after the mode"},
        {">rateDivide(0:0x143CB8, 99, void)", IO_ERROR_HOOK_BAD_DIVISOR, "divisor above the limit"},
        {">rateDivide(0:0x143CB8, 0, void)", IO_ERROR_HOOK_BAD_DIVISOR, "divisor of zero"},
        {">rateDivide(0:0x143CB8, 0 - 1, void)", IO_ERROR_HOOK_BAD_DIVISOR, "negative divisor"},
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

    // 'fps_limit / 30' is 0 at FPS=20, which is not a rate: the patch has to
    // write a divisor that is defined at every setting it supports
    reset(FT_ENABLED, FPS_20);
    CHECK(parse(">rateDivide(0:0x143CB8, fps_limit / 30, ret=1)") == IO_ERROR_HOOK_BAD_DIVISOR,
                "a divisor that collapses to 0 is refused");
    CHECK(g_fake_num == 0, "...and hooks nothing");

    // A line that parses must still be accepted after a comment is stripped
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x143CB8, 2, void) # minigame 45") == IO_OK, "trailing comment is allowed");
}

// The wrapper carries r0-r3 in and r0 out. A target that needs anything else
// has to be refused: skipping such a call fabricates only half of its result.
static void test_rate_divide_abi() {
    struct { const char *line; vg_io_status_code_t code; const char *what; } cases[] = {
        {">rateDivide(0:0x1000, 2, ret64=1)", IO_ERROR_HOOK_UNSUPPORTED_ABI, "64 bit return"},
        {">rateDivide(0:0x1000, 2, retfloat)", IO_ERROR_HOOK_UNSUPPORTED_ABI, "float return"},
        {">rateDivide(0:0x1000, 2, retstruct)", IO_ERROR_HOOK_UNSUPPORTED_ABI, "struct return"},
        {">rateDivide(0:0x1000, 2, ret=1.5)", IO_ERROR_PARSE_INVALID_TOKEN, "float substitute value"},
        {">rateDivide(0:0x1000, 2, void, args=5)", IO_ERROR_HOOK_UNSUPPORTED_ABI, "five arguments"},
        {">rateDivide(0:0x1000, 2, void, args=16)", IO_ERROR_HOOK_UNSUPPORTED_ABI, "sixteen arguments"},
        {">rateDivide(0:0x1000, 2, void, args=0 - 1)", IO_ERROR_HOOK_UNSUPPORTED_ABI, "negative arity"},
        {">rateDivide(0:0x1000, 2, void, args=4)", IO_OK, "four arguments"},
        {">rateDivide(0:0x1000, 2, void, args=0)", IO_OK, "no arguments"},
        {">rateDivide(0:0x1000, 2, ret=0 - 1)", IO_OK, "-1 as the substitute value"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset(FT_ENABLED, FPS_60);
        vg_io_status_code_t code = parse(cases[i].line);
        CHECK(code == cases[i].code, "%s: expected %d, got %d (%s)",
                    cases[i].what, cases[i].code, code, cases[i].line);
        CHECK(fake_offset_hook_num() == (cases[i].code == IO_OK ? 1 : 0),
                    "%s: %s hooked", cases[i].what, cases[i].code == IO_OK ? "is" : "is not");
    }

    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=0 - 1)") == IO_OK, "install -1 substitute");
    call_game_function(0, 0);
    CHECK(call_game_function(0, 0) == -1, "a skipped call can return -1");
}

// The hook target is an address a branch is written to, so a stale or
// mistyped one corrupts whatever it lands on. taiHEN checks the segment index
// and nothing else.
static void test_rate_divide_target_bounds() {
    struct { const char *line; vg_io_status_code_t code; const char *what; } cases[] = {
        {">rateDivide(200:0x1000, 2, void)", IO_ERROR_HOOK_BAD_TARGET, "segment past the module"},
        {">rateDivide(2:0x1000, 2, void)", IO_ERROR_HOOK_BAD_TARGET, "segment the module does not have"},
        {">rateDivide(0:0xFFFFFFFF, 2, void)", IO_ERROR_HOOK_BAD_TARGET, "offset past the module"},
        {">rateDivide(0:0x200000, 2, void)", IO_ERROR_HOOK_BAD_TARGET, "offset at the end of the segment"},
        {">rateDivide(0:0x1FFFF8, 2, void)", IO_ERROR_HOOK_BAD_TARGET, "no room for the target's prologue"},
        {">rateDivide(1:0x10000, 2, void)", IO_ERROR_HOOK_BAD_TARGET, "past the end of segment 1"},
        {">rateDivide(0:0x1001, 2, void)", IO_ERROR_HOOK_BAD_TARGET, "odd thumb offset"},
        {">rateDivide(0:0x1002, 2, void, arm)", IO_ERROR_HOOK_BAD_TARGET, "unaligned arm offset"},
        {">rateDivide(0:0x1002, 2, void)", IO_OK, "even thumb offset"},
        {">rateDivide(0:0x1004, 2, void, arm)", IO_OK, "aligned arm offset"},
        {">rateDivide(1:0xFFF0, 2, void)", IO_OK, "the last function in segment 1"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset(FT_ENABLED, FPS_60);
        vg_io_status_code_t code = parse(cases[i].line);
        CHECK(code == cases[i].code, "%s: expected %d, got %d (%s)",
                    cases[i].what, cases[i].code, code, cases[i].line);
        CHECK(fake_offset_hook_num() == (cases[i].code == IO_OK ? 1 : 0),
                    "%s: %s hooked", cases[i].what, cases[i].code == IO_OK ? "is" : "is not");
        CHECK(g_main.rate_hook_num == (uint32_t)(cases[i].code == IO_OK ? 1 : 0),
                    "%s: a refused target takes no slot", cases[i].what);
    }

    // Without module info nothing can be bound checked; say so and carry on
    // rather than refusing every directive
    reset(FT_ENABLED, FPS_60);
    memset(&g_main.sce_info, 0, sizeof(g_main.sce_info));
    CHECK(parse(">rateDivide(0:0x143CB8, 2, void)") == IO_OK, "no module info: the hook still installs");
    CHECK(fake_offset_hook_num() == 1, "no module info: hooked anyway");
    reset(FT_ENABLED, FPS_60);
    memset(&g_main.sce_info, 0, sizeof(g_main.sce_info));
    CHECK(parse(">rateDivide(0:0x143CB9, 2, void)") == IO_ERROR_HOOK_BAD_TARGET,
                "no module info: alignment is still checked");
}

// Two lines about one address that disagree are a patch bug, not something to
// resolve by taking whichever came first
static void test_rate_divide_conflicts() {
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=1)") == IO_OK, "first line installs");
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=1)") == IO_OK, "an identical repeat is not an error");
    CHECK(fake_offset_hook_num() == 1, "...and does not hook twice");
    CHECK(parse(">rateDivide(0:0x1000, 3, ret=1)") == IO_ERROR_HOOK_CONFLICT, "a different divisor is an error");
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=99)") == IO_ERROR_HOOK_CONFLICT, "a different return is an error");
    CHECK(parse(">rateDivide(0:0x1000, 2, void)") == IO_ERROR_HOOK_CONFLICT, "void against ret=1 is an error");
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=1, frame)") == IO_ERROR_HOOK_CONFLICT, "a different mode is an error");
    CHECK(g_main.rate_hook[0].divisor == 2 && g_main.rate_hook[0].ret_value == 1,
                "the installed hook is left alone");
    CHECK(fake_offset_hook_num() == 1, "no conflicting line hooked anything");

    // An identical repeat of a line that is inert is still not an error
    reset(FT_ENABLED, FPS_30);
    CHECK(parse(">rateDivide(0:0x1000, fps_limit / 30, ret=1)") == IO_OK, "inert line");
    CHECK(parse(">rateDivide(0:0x1000, fps_limit / 30, ret=99)") == IO_OK, "inert lines are not compared");
}

// The default: each hook counts its own calls. Nothing else can move that
// counter, so it cannot drift against the function it is attached to and it
// cannot freeze while the game is not presenting.
static void test_rate_divide_calls() {
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x143CB8, fps_limit / 30, ret=1)") == IO_OK, "install for call test");

    // The first call of every pair is made, the second is not
    CHECK(call_game_function(0, 0x11) == FAKE_ORIGINAL_RET, "the first call reaches the game");
    CHECK(g_original_calls == 1, "one call so far");
    CHECK(g_original_last_args[0] == 0x11, "arguments are carried through");
    CHECK(call_game_function(0, 0x22) == 1, "the second call returns the substitute");
    CHECK(g_original_calls == 1, "...and is not made");
    CHECK(call_game_function(0, 0x33) == FAKE_ORIGINAL_RET, "the third call reaches the game");
    CHECK(g_original_calls == 2, "two calls in six");
    CHECK(g_original_last_args[0] == 0x33, "arguments of the call that was made");

    // ...whatever the display is doing. This is the case a frame counter gets
    // wrong: 1000 calls with nothing presented used to make 0 or 1000 of them.
    for (int i = 0; i < 1000; i++) {
        call_game_function(0, 0);
    }
    CHECK(g_original_calls == 2 + 500, "1 call in 2 with nothing presented (got %d)", g_original_calls - 2);
    CHECK(fake_import_hook_num(0x7A410B64) == 0, "and no frame counter was needed");

    // 'void' hands back 0
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x12465E, 2, void)") == IO_OK, "install void target");
    CHECK(call_game_function(0, 0) == FAKE_ORIGINAL_RET, "the first void call is made");
    CHECK(call_game_function(0, 0) == 0, "a skipped void call returns 0");
    CHECK(g_original_calls == 1, "the skipped void call is not made");

    // 1 call in 3
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 3, ret=1)") == IO_OK, "install 1 in 3");
    for (int i = 0; i < 9; i++) {
        call_game_function(0, 0);
    }
    CHECK(g_original_calls == 3, "1 call in 3 over 9 calls (got %d)", g_original_calls);

    // 1 call in 16, the largest divisor the directive takes
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 16, ret=1)") == IO_OK, "install 1 in 16");
    for (int i = 0; i < 160; i++) {
        call_game_function(0, 0);
    }
    CHECK(g_original_calls == 10, "1 call in 16 over 160 calls (got %d)", g_original_calls);

    // Two hooks count independently: one called twice as often as the other
    // still runs 1 call in 2 of its own
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=1)") == IO_OK, "install first");
    CHECK(parse(">rateDivide(0:0x2000, 2, ret=1)") == IO_OK, "install second");
    CHECK(fake_offset_hook_num() == 2, "two offset hooks");
    for (int i = 0; i < 8; i++) {
        call_game_function(0, 0);
        if (i % 2 == 0)
            call_game_function(1, 0);
    }
    CHECK(g_original_calls == 4 + 2, "each slot divides its own calls (got %d)", g_original_calls);
    CHECK(g_main.rate_hook[0].count == 8 && g_main.rate_hook[1].count == 4, "per slot counts");
}

// Frame counting is the other model, and it is only for a target that is
// called several times in one frame on behalf of several objects (PCSG00246's
// battle script step wait): those calls have to agree with each other.
static void test_rate_divide_frame_counted() {
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x44A8C, fps_limit / 30, void, frame)") == IO_OK, "frame form parses");
    CHECK(g_main.rate_hook[0].frame_counted, "the slot is frame counted");
    CHECK(fake_import_hook_num(0x7A410B64) == 1, "a frame counted hook installs the frame counter");
    CHECK(g_main.hook[HOOK_RATE_FRAME_COUNTER] >= 0, "...in its own slot");

    // Every call made in one frame agrees, which is the whole point
    CHECK(call_game_function(0, 0) == FAKE_ORIGINAL_RET, "frame 0 call reaches the game");
    CHECK(call_game_function(0, 0) == FAKE_ORIGINAL_RET, "every call in frame 0 runs");
    CHECK(call_game_function(0, 0) == FAKE_ORIGINAL_RET, "...all of them");
    CHECK(g_original_calls == 3, "three calls in frame 0");

    present_frame();
    CHECK(g_main.frame == 1, "the frame counter advances");
    CHECK(g_import_original_calls == 1, "the frame counter chains to the original");
    CHECK(call_game_function(0, 0) == 0, "frame 1 returns the substitute");
    CHECK(call_game_function(0, 0) == 0, "every call in frame 1 is skipped");
    CHECK(g_original_calls == 3, "frame 1 makes no call");

    present_frame();
    CHECK(call_game_function(0, 0) == FAKE_ORIGINAL_RET, "frame 2 calls the game again");
    CHECK(g_original_calls == 4, "half of the frames call the original");

    // Two frame counted hooks share one frame counter and skip together
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=1, frame)") == IO_OK, "install first");
    CHECK(parse(">rateDivide(0:0x2000, 2, ret=1, frame)") == IO_OK, "install second");
    CHECK(fake_import_hook_num(0x7A410B64) == 1, "the frame counter is installed once");
    call_game_function(0, 0);
    call_game_function(1, 0);
    CHECK(g_original_calls == 2, "both run in frame 0");
    present_frame();
    call_game_function(0, 0);
    call_game_function(1, 0);
    CHECK(g_original_calls == 2, "both skip frame 1");

    // A game that stops presenting parks the counter. Frame counting has no
    // answer to that, so the stall guard makes the call rather than let the
    // game wait for a frame that is not coming.
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, void, frame)") == IO_OK, "install for the stall test");
    present_frame();                                  // park on a skipped frame
    CHECK(g_main.frame % 2 == 1, "parked on a skipped frame");
    for (uint32_t i = 0; i < RATE_STALL_SKIPS - 1; i++) {
        CHECK_QUIET(call_game_function(0, 0) == 0, "call %u is skipped", i);
    }
    CHECK(g_original_calls == 0, "the first RATE_STALL_SKIPS-1 calls are skipped");
    CHECK(call_game_function(0, 0) == FAKE_ORIGINAL_RET, "the stalled hook lets a call through");
    CHECK(g_original_calls == 1, "...exactly one");
    for (uint32_t i = 0; i < RATE_STALL_SKIPS; i++) {
        CHECK_QUIET(call_game_function(0, 0) == (i + 1 < RATE_STALL_SKIPS ? 0 : FAKE_ORIGINAL_RET),
                    "the stall count starts again");
    }
    CHECK(g_original_calls == 2, "a stalled frame counted hook runs 1 call in RATE_STALL_SKIPS");

    // ...and it goes back to frame parity as soon as a frame is presented
    present_frame();
    CHECK(call_game_function(0, 0) == FAKE_ORIGINAL_RET, "an even frame runs again");
    CHECK(g_original_calls == 3, "...once");
    present_frame();
    CHECK(call_game_function(0, 0) == 0, "and an odd frame skips again");
    CHECK(g_original_calls == 3, "...without making the call");
}

// Neither counter may produce a short interval where it wraps
static void test_rate_divide_counter_wrap() {
    // The frame counter folds back on a multiple of every supported divisor
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 3, void, frame)") == IO_OK, "install frame counted 1 in 3");
    g_main.frame = RATE_COUNT_MODULUS - 3;
    for (int i = 0; i < 9; i++) {
        call_game_function(0, 0);
        present_frame();
    }
    CHECK(g_main.frame < RATE_COUNT_MODULUS, "the frame counter folded back (%u)", g_main.frame);
    CHECK(g_original_calls == 3, "1 call in 3 across the fold (got %d)", g_original_calls);

    // ...and so does each slot's own call counter. Divisor 3 does not divide
    // 2^32, so a plain uint32 wrap would run two calls back to back here.
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 3, void)") == IO_OK, "install call counted 1 in 3");
    g_main.rate_hook[0].count = RATE_COUNT_MODULUS - 3;
    for (int i = 0; i < 9; i++) {
        call_game_function(0, 0);
    }
    CHECK(g_main.rate_hook[0].count < RATE_COUNT_MODULUS, "the call counter folded back (%u)",
                g_main.rate_hook[0].count);
    CHECK(g_original_calls == 3, "1 call in 3 across the fold (got %d)", g_original_calls);

    // Every divisor the directive accepts divides the fold value
    for (uint32_t divisor = 1; divisor <= RATE_DIVISOR_MAX; divisor++) {
        CHECK(RATE_COUNT_MODULUS % divisor == 0, "divisor %u divides the fold value", divisor);
    }
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
    CHECK(parse(">rateDivide(1:0x2000, 2, ret=1)") == IO_OK, "another segment installs");
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
    CHECK(parse(line) == IO_ERROR_TOO_MANY_HOOKS, "one past the pool is refused");
    // ...with the limit that was actually reached, not MAX_INJECT_NUM
    CHECK(strstr(vg_io_status_code_to_string(IO_ERROR_TOO_MANY_HOOKS), TOSTRING(MAX_RATE_HOOK_NUM)) != NULL,
                "the message names the hook limit: %s", vg_io_status_code_to_string(IO_ERROR_TOO_MANY_HOOKS));
    CHECK(strcmp(vg_io_status_code_to_string(IO_ERROR_TOO_MANY_HOOKS),
                vg_io_status_code_to_string(IO_ERROR_TOO_MANY_PATCHES)) != 0,
                "...and is not the patch limit message");

    // Every slot has its own wrapper, so no two slots share a hook body
    for (int i = 0; i < g_fake_num; i++) {
        for (int j = i + 1; j < g_fake_num; j++) {
            CHECK_QUIET(g_fake[i].func != g_fake[j].func, "hook bodies %d and %d are distinct", i, j);
        }
    }

    // ...and every slot's own wrapper drives its own slot
    for (int i = 0; i < MAX_RATE_HOOK_NUM; i++) {
        g_original_calls = 0;
        CHECK(call_game_function(i, 0) == FAKE_ORIGINAL_RET, "slot %d runs its first call", i);
        CHECK(g_original_calls == 1, "slot %d calls the original once", i);
        CHECK(call_game_function(i, 0) == 0, "slot %d skips its second call", i);
        CHECK(g_original_calls == 1, "slot %d makes no call while skipping", i);
        CHECK(g_main.rate_hook[i].count == 2, "slot %d counted its own two calls", i);
    }

    // module_stop releases every hook it took
    release_all_hooks();
    for (int i = 0; i < g_fake_num; i++) {
        CHECK_QUIET(g_fake[i].released, "hook %d released", i);
    }
    CHECK(g_main.rate_hook_num == 0, "the pool is empty again");
}

// Unload: a call that is already on its way into a wrapper must not follow a
// chain that is being released
static void test_release() {
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=7)") == IO_OK, "install for the release test");
    CHECK(parse(">rateDivide(0:0x2000, 2, void, frame)") == IO_OK, "install a frame counted one too");
    CHECK(g_main.rate_hook[0].armed && g_main.rate_hook[1].armed, "both slots are armed");

    release_all_hooks();
    for (int i = 0; i < g_fake_num; i++) {
        CHECK(g_fake[i].released, "hook %d is released", i);
    }
    CHECK(!g_main.rate_hook[0].armed && !g_main.rate_hook[1].armed, "both slots are disarmed");

    // The fake hooks are still in place, so a wrapper that ignored 'armed'
    // would reach fake_original through a released ref
    g_original_calls = 0;
    CHECK(call_game_function(0, 0) == 7, "a call after release returns the substitute");
    CHECK(call_game_function(1, 0) == 0, "...for a frame counted slot too");
    CHECK(g_original_calls == 0, "a call after release follows no released chain");

    // The divisor reset in module_stop is not what keeps them out: a slot that
    // was released and had its divisor cleared must still not continue
    g_main.rate_hook[0].divisor = 0;
    CHECK(call_game_function(0, 0) == 7, "a cleared divisor does not force the call through");
    CHECK(g_original_calls == 0, "...and still makes no call");
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
// frame functions have their result discarded, and the battle script step
// wait is the one target that is called several times in one frame)
static void test_patch_shaped_lines() {
    static const char *lines[] = {
        ">rateDivide(0:0x143CB8, fps_limit / 30, ret=1)         # minigame 45 update",
        ">rateDivide(0:0x12276C, fps_limit / 30, ret=1)         # minigame 3 update",
        ">rateDivide(0:0x12465E, fps_limit / 30, void)          # minigame 3, result discarded",
        ">rateDivide(0:0x1434B0, fps_limit / 30, ret=1)         # minigame 44 update",
        ">rateDivide(0:0x44A8C, fps_limit / 30, void, frame)    # battle script step wait"
    };
    const int line_num = sizeof(lines) / sizeof(lines[0]);

    reset(FT_ENABLED, FPS_60);
    for (int i = 0; i < line_num; i++) {
        CHECK(parse(lines[i]) == IO_OK, "patch line %d installs", i);
    }
    CHECK(fake_offset_hook_num() == line_num, "every line took a hook");
    CHECK(fake_import_hook_num(0x7A410B64) == 1, "one frame counter, for the one line that asked");
    for (int i = 0; i < line_num; i++) {
        CHECK(g_main.rate_hook[i].divisor == 2, "line %d runs 1 call in 2", i);
    }
    CHECK(g_main.rate_hook[2].ret_value == 0 && g_main.rate_hook[0].ret_value == 1,
                "void and ret=1 are kept apart");
    CHECK(!g_main.rate_hook[0].frame_counted && g_main.rate_hook[4].frame_counted,
                "call counting by default, frame counting where the line says so");

    // The minigame updates are called once per frame; the step wait is called
    // several times in one frame, for several objects, and those calls agree
    for (int frame = 0; frame < 4; frame++) {
        call_game_function(0, 0);
        for (int object = 0; object < 3; object++) {
            call_game_function(4, 0);
        }
        present_frame();
    }
    CHECK(g_original_calls == 2 + 6, "half rate for both shapes (got %d)", g_original_calls);

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
    test_feature_gating();
    test_rate_divide_install();
    test_rate_divide_parse_errors();
    test_rate_divide_abi();
    test_rate_divide_target_bounds();
    test_rate_divide_conflicts();
    test_rate_divide_calls();
    test_rate_divide_frame_counted();
    test_rate_divide_counter_wrap();
    test_slots();
    test_release();
    test_patch_shaped_lines();

    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
