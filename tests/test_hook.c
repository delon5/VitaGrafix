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

// The thread the plugin sees. The wrapper tells a call the game made from a
// call the hooked function made into itself by thread id, so a test that wants
// to look like another thread moves this.
static int g_thread_id = 0x4001;
int sceKernelGetThreadId(void) { return g_thread_id; }

static int call_game_function(int slot_index, uintptr_t a0);

// Stands in for the hooked game function
static int g_original_calls;
static uintptr_t g_original_last_args[4];
static int g_import_original_calls;

// ------------------------------------------------- the game's input sampler

/*
 * A stand-in for the pad structure the input sampler is handed, with the
 * press/repeat pulse mask at a fixed offset in it, and a stand-in for the
 * sampler itself. Together with the rate divided target below they are enough
 * to run the whole accumulation: a poll is call_sampler(), a frame is a poll
 * plus an update plus present_frame().
 */
#define TEST_MASK_OFFSET 0x98
static uint32_t g_pad[64];

static uint32_t *pad_mask() {
    return (uint32_t *)((uintptr_t)g_pad + TEST_MASK_OFFSET);
}

// What the sampler builds on the next poll, i.e. the pulses of that poll
static uint32_t g_poll_mask;
static int g_sampler_calls;

// What the rate divided target does with the mask it is handed
static bool g_target_reads_mask;
typedef enum {
    TARGET_LEAVES_MASK,     // reads it and writes nothing
    TARGET_SWALLOWS_MASK,   // "I acted on this, nobody else may": a literal 0
    TARGET_DISABLES_MASK    // the engine's -1
} target_mask_write_t;
static target_mask_write_t g_target_write = TARGET_LEAVES_MASK;

#define TARGET_SAW_MAX 64
static uint32_t g_target_saw[TARGET_SAW_MAX];
static int g_target_saw_num;

// The sampler the '>inputUnion()' hook sits on: it rebuilds the mask from
// scratch on every poll, which is what makes every bit of it one poll wide
static int fake_sampler_original(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a1; (void)a2; (void)a3;
    g_sampler_calls++;
    *(uint32_t *)(a0 + TEST_MASK_OFFSET) = g_poll_mask;
    return 0;
}

// A game function that calls itself, which is what defeated the divisor and
// truncated the recursion before the wrapper tracked depth. While
// g_recurse_left is positive the stand-in re-enters the wrapper that hooked
// it, optionally as a different thread.
static int g_recurse_slot = -1;
static int g_recurse_left;
static int g_recurse_thread;        // 0: the same thread as the outer call
static int g_inner_calls;
static int g_inner_made;
static int g_inner_last_ret;
static int g_depth;
static int g_max_depth;

static int fake_import_original(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    g_import_original_calls++;
    return 0;
}
static int fake_original(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    g_original_calls++;

    // The input union tests hand the target the pad, and what it reads out of
    // the mask while it runs is the whole question
    if (g_target_reads_mask && a0 != 0) {
        uint32_t *mask = (uint32_t *)(a0 + TEST_MASK_OFFSET);
        if (g_target_saw_num < TARGET_SAW_MAX)
            g_target_saw[g_target_saw_num++] = *mask;
        if (g_target_write == TARGET_SWALLOWS_MASK)
            *mask = 0;
        else if (g_target_write == TARGET_DISABLES_MASK)
            *mask = 0xFFFFFFFF;
    }

    g_original_last_args[0] = a0;
    g_original_last_args[1] = a1;
    g_original_last_args[2] = a2;
    g_original_last_args[3] = a3;

    if (++g_depth > g_max_depth)
        g_max_depth = g_depth;

    if (g_recurse_slot >= 0 && g_recurse_left > 0) {
        g_recurse_left--;
        g_inner_calls++;
        if (g_recurse_thread != 0) {
            // Another thread walks into the same target while this one is
            // inside it. That is not re-entry and must be counted.
            int outer_thread = g_thread_id;
            g_thread_id = g_recurse_thread;
            g_inner_last_ret = call_game_function(g_recurse_slot, a0);
            g_thread_id = outer_thread;
        } else {
            g_inner_last_ret = call_game_function(g_recurse_slot, a0);
        }
        if (g_inner_last_ret == FAKE_ORIGINAL_RET)
            g_inner_made++;
    }

    g_depth--;
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

    // ...and the sampler hook after the slots that read what it accumulates
    if (g_main.input.uid >= 0) {
        g_main.input.armed = false;
        g_main.input.mask_addr = NULL;
        taiHookRelease(g_main.input.uid, g_main.input.ref);
        g_main.input.uid = -1;
    }
    g_main.input.requested = false;
    g_main.input.divisor = 0;

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

// The sampler hook is an offset hook like any other, so it has to be kept out
// of the rate divided hooks' indexing. Remembered by index rather than looked
// up by uid every time, so that the tests can still reach the hook body after
// module_stop has released it and cleared the uid.
static int g_sampler_fake = -1;

static bool fake_is_sampler(int i) {
    if (g_sampler_fake < 0 && g_main.input.uid >= 0) {
        for (int j = 0; j < g_fake_num; j++) {
            if (!g_fake[j].is_import && g_fake[j].uid == g_main.input.uid)
                g_sampler_fake = j;
        }
    }
    return i == g_sampler_fake;
}

// The sampler's original is not the rate divided target's: point the installed
// sampler hook at the stand-in that rebuilds the mask
static void bind_sampler_original() {
    for (int i = 0; i < g_fake_num; i++) {
        if (fake_is_sampler(i))
            g_fake[i].node.old = (void *)&fake_sampler_original;
    }
}

// One poll of the game's input sampler
static int call_sampler() {
    for (int i = 0; i < g_fake_num; i++) {
        if (fake_is_sampler(i)) {
            return ((int (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t))g_fake[i].func)(
                        (uintptr_t)g_pad, 0, 0, 0);
        }
    }
    CHECK(false, "no input sampler hook installed");
    return 0;
}

// Rate divided hooks only: the sampler hook '>inputUnion()' installs is an
// offset hook too, and it is counted by fake_sampler_hook_num()
static int fake_offset_hook_num() {
    int n = 0;
    for (int i = 0; i < g_fake_num; i++) {
        if (!g_fake[i].is_import && !fake_is_sampler(i))
            n++;
    }
    return n;
}

static int fake_sampler_hook_num() {
    int n = 0;
    for (int i = 0; i < g_fake_num; i++) {
        if (fake_is_sampler(i))
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
#define TEST_SEG2_SIZE 0x8000

// SceKernelSegmentInfo::perms carries the ELF program header flags: a text
// segment reads 0x5 (read+execute), a data segment 0x6 (read+write)
#define TEST_PERM_TEXT 0x5
#define TEST_PERM_DATA 0x6

static void set_module_segments() {
    g_main.sce_info.segments[0].memsz = TEST_SEG0_SIZE;
    g_main.sce_info.segments[0].perms = TEST_PERM_TEXT;
    g_main.sce_info.segments[1].memsz = TEST_SEG1_SIZE;
    g_main.sce_info.segments[1].perms = TEST_PERM_TEXT;
    g_main.sce_info.segments[2].memsz = TEST_SEG2_SIZE;
    g_main.sce_info.segments[2].perms = TEST_PERM_DATA;
}

static void reset(vg_feature_state_t fps_enabled, vg_fps_t fps) {
    memset(&g_main, 0, sizeof(g_main));
    for (int i = 0; i < MAX_HOOK_NUM; i++) {
        g_main.hook[i] = -1;
    }
    g_main.input.uid = -1;
    for (int i = 0; i < MAX_RATE_HOOK_NUM; i++) {
        g_main.rate_hook[i].uid = -1;
    }
    set_module_segments();
    memset(g_fake, 0, sizeof(g_fake));
    g_fake_num = 0;
    g_fake_fail_next = 0;
    g_original_calls = 0;
    g_import_original_calls = 0;
    g_recurse_slot = -1;
    g_recurse_left = 0;
    g_recurse_thread = 0;
    g_inner_calls = 0;
    g_inner_made = 0;
    g_inner_last_ret = 0;
    g_depth = 0;
    g_max_depth = 0;
    g_thread_id = 0x4001;
    g_sampler_fake = -1;
    memset(g_pad, 0, sizeof(g_pad));
    g_poll_mask = 0;
    g_sampler_calls = 0;
    g_target_reads_mask = false;
    g_target_write = TARGET_LEAVES_MASK;
    g_target_saw_num = 0;
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
static int call_game_function(int slot_index, uintptr_t a0) {
    int seen = -1;
    for (int i = 0; i < g_fake_num; i++) {
        if (g_fake[i].is_import || fake_is_sampler(i))
            continue;
        if (++seen == slot_index)
            return ((int (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t))g_fake[i].func)(
                        a0, 0, 0, 0);
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
        {">rateDivide(3:0x1000, 2, void)", IO_ERROR_HOOK_BAD_TARGET, "segment the module does not have"},
        {">rateDivide(2:0x1000, 2, void)", IO_ERROR_HOOK_BAD_TARGET, "offset into a data segment"},
        {">rateDivide(2:0x1000, 2, void, arm)", IO_ERROR_HOOK_BAD_TARGET, "data segment, arm as well"},
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

    // A segment whose execute bit is clear holds data. An offset into one is a
    // misread address, not a function entry, and arming a hook over it writes
    // a branch into the game's data.
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(2:0x0, 2, void)") == IO_ERROR_HOOK_BAD_TARGET,
                "the start of a data segment is refused too");
    CHECK(fake_offset_hook_num() == 0, "nothing is hooked over data");
    reset(FT_ENABLED, FPS_60);
    g_main.sce_info.segments[2].perms = TEST_PERM_TEXT;
    CHECK(parse(">rateDivide(2:0x1000, 2, void)") == IO_OK,
                "the same offset in an executable segment installs");

    // Without module info the target cannot be bound checked at all, and this
    // check is the only thing between a stale offset and a branch written over
    // whatever it lands on - so the directive is refused rather than installed
    // unchecked. sceKernelGetModuleInfo() failing on hardware lands here.
    reset(FT_ENABLED, FPS_60);
    memset(&g_main.sce_info, 0, sizeof(g_main.sce_info));
    CHECK(parse(">rateDivide(0:0x143CB8, 2, void)") == IO_ERROR_HOOK_NO_MODULE_INFO,
                "no module info: the directive is refused");
    CHECK(fake_offset_hook_num() == 0, "no module info: nothing is hooked");
    CHECK(g_main.rate_hook_num == 0, "no module info: no slot is taken");
    reset(FT_ENABLED, FPS_60);
    memset(&g_main.sce_info, 0, sizeof(g_main.sce_info));
    CHECK(parse(">rateDivide(0:0x143CB9, 2, void)") == IO_ERROR_HOOK_BAD_TARGET,
                "no module info: alignment is still checked first");
    // A directive that installs nothing at this configuration is not parsed
    // against a segment table it never reaches
    reset(FT_ENABLED, FPS_30);
    memset(&g_main.sce_info, 0, sizeof(g_main.sce_info));
    CHECK(parse(">rateDivide(0:0x143CB8, 1 + (fps_limit / 60), void)") == IO_OK,
                "no module info: an inert directive is still not an error");
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
    // 'thumb' is the field where being wrong means taiHEN decodes the branch
    // it writes the wrong way, so it is the one that must not be resolved in
    // favour of whichever line came first
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=1, arm)") == IO_ERROR_HOOK_CONFLICT,
                "arm against thumb is an error");
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=1, args=2)") == IO_ERROR_HOOK_CONFLICT,
                "a different argument count is an error");
    CHECK(g_main.rate_hook[0].divisor == 2 && g_main.rate_hook[0].ret_value == 1,
                "the installed hook is left alone");
    CHECK(g_main.rate_hook[0].thumb, "...and keeps the instruction set it was installed with");
    CHECK(fake_offset_hook_num() == 1, "no conflicting line hooked anything");

    // The same the other way round, so that neither order is the lucky one
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=1, arm)") == IO_OK, "arm line installs");
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=1, arm)") == IO_OK, "an identical arm repeat is fine");
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=1)") == IO_ERROR_HOOK_CONFLICT,
                "thumb against arm is an error");
    CHECK(!g_main.rate_hook[0].thumb, "the arm hook is left alone");
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=1, arm, args=4)") == IO_OK,
                "spelling out the default argument count is not a disagreement");

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

    // A game that stops presenting parks the frame counter, and a parked
    // counter is not a clock. Whichever parity it is parked on, the slot falls
    // back to counting its own calls after RATE_STALL_CALLS of them, so the
    // game keeps the rate it asked for instead of stalling or running free.

    // Parked on a skipped frame: without the fallback the hook would skip
    // every call for as long as the stall lasts, and a game waiting on that
    // function would never make progress
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 3, void, frame)") == IO_OK, "install for the skip stall test");
    present_frame();                                  // park on a skipped frame
    CHECK(g_main.frame % 3 != 0, "parked on a skipped frame");
    for (uint32_t i = 0; i < RATE_STALL_CALLS + 8; i++) {
        call_game_function(0, 0);
    }
    CHECK(g_original_calls > 0, "a stalled frame counted hook does not skip forever");
    CHECK(g_original_calls < (int)(RATE_STALL_CALLS + 8),
                "...and does not simply run everything either (%d)", g_original_calls);
    int before = g_original_calls;
    int interval = 0, irregular = 0;
    for (uint32_t i = 0; i < 300; i++) {
        bool made = call_game_function(0, 0) == FAKE_ORIGINAL_RET;
        interval++;
        if (made) {
            if (i > 0 && interval != 3)
                irregular++;
            CHECK_QUIET(interval == 3 || i == 0, "call %u is made after %d, not 3", i, interval);
            interval = 0;
        } else {
            CHECK_QUIET(interval <= 3, "call %u has been skipped for %d in a row", i, interval);
        }
    }
    CHECK(irregular == 0, "the fallback keeps an exact 1 in 3 interval (%d irregular)", irregular);
    CHECK(g_original_calls - before == 100,
                "a stalled hook parked on a skipped frame still runs 1 call in 3 (got %d)",
                g_original_calls - before);

    // Parked on a made frame: the opposite failure, and the one the stall
    // guard used not to cover at all - every call went through, i.e. no rate
    // division at all, silently, for as long as the game was not presenting
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 3, void, frame)") == IO_OK, "install for the run stall test");
    CHECK(g_main.frame % 3 == 0, "parked on a frame that runs");
    for (uint32_t i = 0; i < RATE_STALL_CALLS + 8; i++) {
        call_game_function(0, 0);
    }
    CHECK(g_original_calls < (int)(RATE_STALL_CALLS + 8),
                "a stalled frame counted hook does not run forever (%d)", g_original_calls);
    before = g_original_calls;
    interval = 0;
    irregular = 0;
    for (uint32_t i = 0; i < 300; i++) {
        bool made = call_game_function(0, 0) == FAKE_ORIGINAL_RET;
        interval++;
        if (made) {
            if (i > 0 && interval != 3)
                irregular++;
            CHECK_QUIET(interval == 3 || i == 0, "call %u is made after %d, not 3", i, interval);
            interval = 0;
        } else {
            CHECK_QUIET(interval <= 3, "call %u has been skipped for %d in a row", i, interval);
        }
    }
    CHECK(irregular == 0, "the fallback keeps an exact 1 in 3 interval here too (%d irregular)",
                irregular);
    CHECK(g_original_calls - before == 100,
                "a stalled hook parked on a made frame runs 1 call in 3 too (got %d)",
                g_original_calls - before);

    // ...and both go back to frame parity as soon as a frame is presented
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, void, frame)") == IO_OK, "install for the recovery test");
    for (uint32_t i = 0; i < RATE_STALL_CALLS + 8; i++) {
        call_game_function(0, 0);
    }
    present_frame();
    before = g_original_calls;
    CHECK(call_game_function(0, 0) == 0, "an odd frame skips again");
    CHECK(g_original_calls == before, "...without making the call");
    present_frame();
    CHECK(call_game_function(0, 0) == FAKE_ORIGINAL_RET, "an even frame runs again");
    CHECK(call_game_function(0, 0) == FAKE_ORIGINAL_RET, "...and so does its second call");
    CHECK(g_original_calls == before + 2, "both calls of that frame were made");
}

/*
 * A hooked function that calls itself. Every entry into the wrapper used to
 * consume a tick of the divisor, so the effective divisor collapsed to d-1
 * (to 1 at divisor 2, i.e. the directive became a silent no-op), and the inner
 * call was skipped every single time, truncating the recursion and handing
 * every level below the top the fabricated return value.
 *
 * The rule: only the outermost call of a nest is counted and decided. An inner
 * call exists only because the outermost call was made, so it is always made.
 */
static void test_rate_divide_recursion() {
    for (uint32_t divisor = 2; divisor <= 4; divisor++) {
        for (int levels = 1; levels <= 3; levels++) {
            reset(FT_ENABLED, FPS_60);

            char line[64];
            snprintf(line, sizeof(line), ">rateDivide(0:0x1000, %u, ret=0x77)", divisor);
            CHECK(parse(line) == IO_OK, "install a recursive target, divisor %u", divisor);

            const int outer = 60;
            int outer_made = 0;
            for (int i = 0; i < outer; i++) {
                g_recurse_slot = 0;
                g_recurse_left = levels;
                if (call_game_function(0, i) == FAKE_ORIGINAL_RET)
                    outer_made++;
            }

            CHECK(outer_made == outer / (int)divisor,
                        "divisor %u with %d levels of recursion makes %d of %d outer calls (got %d)",
                        divisor, levels, outer / (int)divisor, outer, outer_made);
            CHECK(g_original_calls == outer_made * (1 + levels),
                        "divisor %u, %d levels: every made outer call carries its %d inner calls"
                        " (got %d, want %d)",
                        divisor, levels, levels, g_original_calls, outer_made * (1 + levels));
            CHECK(g_inner_calls == outer_made * levels,
                        "divisor %u, %d levels: inner calls only happen under a made outer call",
                        divisor, levels);
            CHECK(g_inner_last_ret == FAKE_ORIGINAL_RET,
                        "divisor %u, %d levels: an inner call reaches the game, not the substitute",
                        divisor, levels);
            CHECK(g_max_depth == levels + 1,
                        "divisor %u: the recursion is not truncated (depth %d, want %d)",
                        divisor, g_max_depth, levels + 1);
        }
    }

    // The outermost call being skipped means the original never runs, so there
    // is no inner call to decide: a skipped call's substitute is unchanged
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=0x77)") == IO_OK, "install divisor 2");
    g_recurse_slot = 0;
    g_recurse_left = 4;
    CHECK(call_game_function(0, 0) == FAKE_ORIGINAL_RET, "the first call is made");
    g_recurse_left = 4;
    CHECK(call_game_function(0, 0) == 0x77, "the second call is skipped");
    CHECK(g_inner_calls == 4, "a skipped outer call makes no inner call at all");

    // Frame counted mode takes the same view: the frame decides the outermost
    // call, and the recursion under a made call is not re-decided
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=0x77, frame)") == IO_OK, "install frame counted");
    for (int i = 0; i < 8; i++) {
        g_recurse_slot = 0;
        g_recurse_left = 2;
        call_game_function(0, i);
        present_frame();
    }
    CHECK(g_original_calls == 4 * 3, "frame counted: 4 made outer calls, 2 inner each (got %d)",
                g_original_calls);
    CHECK(g_max_depth == 3, "frame counted: the recursion is not truncated");

    // Two threads calling the hooked function share the slot's counter: the
    // divisor is a property of the function, not of a thread
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=0x77)") == IO_OK, "install divisor 2");
    for (int i = 0; i < 40; i++) {
        g_thread_id = (i % 2) ? 0x4001 : 0x4002;
        call_game_function(0, i);
    }
    CHECK(g_original_calls == 20, "two threads share one slot's counter (got %d)", g_original_calls);
    for (uint32_t i = 0; i < RATE_REENTRY_MAX; i++) {
        CHECK_QUIET(g_main.rate_hook[0].entry[i].thread == 0,
                    "every re-entry record is released when its call returns");
    }

    // A second thread walking into the same target while the first is inside
    // it is NOT re-entry - it is another call from the game, and it takes part
    // in the count like any other. Here the two streams interleave exactly, so
    // 40 outer calls and the 40 calls the other thread makes inside them are
    // 80 counted calls of which 40 are made; what matters is that the inner
    // call was decided by the divisor rather than waved through.
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=0x77)") == IO_OK, "install divisor 2");
    for (int i = 0; i < 40; i++) {
        g_recurse_slot = 0;
        g_recurse_left = 1;
        g_recurse_thread = 0x4002;
        call_game_function(0, i);
    }
    CHECK(g_inner_calls == 40, "the other thread called in every time");
    CHECK(g_original_calls == 40,
                "80 counted calls across two threads make 40 (got %d)", g_original_calls);
    CHECK(g_inner_made == 0 && g_inner_last_ret == 0x77,
                "a call on another thread is counted, not waved through as re-entry");

    // More threads inside one target than there are re-entry records: such a
    // call is treated as the outermost call it is, never as a free pass
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x1000, 2, ret=0x77)") == IO_OK, "install divisor 2");
    g_recurse_slot = 0;
    for (uint32_t i = 0; i < RATE_REENTRY_MAX; i++) {
        g_main.rate_hook[0].entry[i].thread = 0x5000 + i;
        g_main.rate_hook[0].entry[i].depth = 1;
    }
    int no_record_made = 0;
    for (int i = 0; i < 20; i++) {
        if (call_game_function(0, i) == FAKE_ORIGINAL_RET)
            no_record_made++;
    }
    CHECK(no_record_made == 10, "a call with no free re-entry record is still counted (got %d)",
                no_record_made);
    for (uint32_t i = 0; i < RATE_REENTRY_MAX; i++) {
        CHECK_QUIET(g_main.rate_hook[0].entry[i].thread == (int32_t)(0x5000 + i),
                    "...and does not release a record it never claimed");
    }
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


/*
 * ------------------------------------------------------------ input union
 *
 * The problem: the games these directives were written for rebuild a
 * press/repeat pulse mask from zero on every poll of their input sampler, and
 * the sampler runs once per RENDERED frame. Every bit of that word is one poll
 * wide. A minigame update that is called 1 time in 2 therefore never sees the
 * presses that landed on a poll it skipped - they are overwritten unread, and
 * that is lost input rather than late input.
 *
 * '>inputUnion()' names the sampler and the mask; 'union' on a rate divided
 * slot makes that slot see the polls it skipped, for the duration of each call
 * it makes and not one instruction longer.
 */

#define TEST_SAMPLER_OFFSET 0x1100
#define TEST_UNION_TARGET   0x2000

// Sets up the pair the rest of these tests drive: the sampler declared, one
// accumulating slot over it, and a target that reads the mask it is handed
static void install_union(const char *rate_line) {
    reset(FT_ENABLED, FPS_60);
    char line[128];
    snprintf(line, sizeof(line), ">inputUnion(0:0x%X, 0x%X)",
                TEST_SAMPLER_OFFSET, TEST_MASK_OFFSET);
    CHECK(parse(line) == IO_OK, "the sampler is declared");
    CHECK(parse(rate_line) == IO_OK, "'%s' installs", rate_line);
    bind_sampler_original();
    g_target_reads_mask = true;
}

// One displayed frame in the order PCSG00246 and PCSG00042 run it: sample,
// then dispatch the updates, then present
static void frame_sample_first(int slot, uint32_t poll_mask) {
    g_poll_mask = poll_mask;
    call_sampler();
    call_game_function(slot, (uintptr_t)g_pad);
    present_frame();
}

// ...and in the order PCSG00490 runs it: update (on the mask the previous
// frame's poll built), then sample, then present
static void frame_update_first(int slot, uint32_t poll_mask) {
    call_game_function(slot, (uintptr_t)g_pad);
    g_poll_mask = poll_mask;
    call_sampler();
    present_frame();
}

static void test_input_union_parse_errors() {
    struct { const char *line; vg_io_status_code_t code; const char *what; } cases[] = {
        {">inputUnion", IO_ERROR_PARSE_INVALID_TOKEN, "no argument list"},
        {">inputUnion()", IO_ERROR_PARSE_INVALID_TOKEN, "empty argument list"},
        {">inputUnion(0:0x1100)", IO_ERROR_PARSE_INVALID_TOKEN, "no mask offset"},
        {">inputUnion(0:0x1100,)", IO_ERROR_INTERPRETER_ERROR, "empty mask offset"},
        {">inputUnion(0x1100, 0x98)", IO_ERROR_PARSE_INVALID_TOKEN, "sampler without a segment"},
        {">inputUnion(0:0x1100, 0x98", IO_ERROR_PARSE_INVALID_TOKEN, "unclosed argument list"},
        {">inputUnion(0:0x1100, 0x98) x", IO_ERROR_PARSE_INVALID_TOKEN, "junk after the directive"},
        {">inputUnion(0:0x1100, 0x98, neon)", IO_ERROR_PARSE_INVALID_TOKEN, "unknown instruction set"},
        {">inputUnion(0:0x1100, 0x98, arm, arm)", IO_ERROR_PARSE_INVALID_TOKEN, "instruction set twice"},
        {">inputUnion(0:0x1100, 0x98, arm, thumb)", IO_ERROR_PARSE_INVALID_TOKEN, "two instruction sets"},
        {">inputUnion(0:0x1100, 0x98.0)", IO_ERROR_PARSE_INVALID_TOKEN, "float mask offset"},
        {">inputUnion(0:0x1100, 0x99)", IO_ERROR_HOOK_BAD_MASK_OFFSET, "mask off a word boundary"},
        {">inputUnion(0:0x1100, 0x2000)", IO_ERROR_HOOK_BAD_MASK_OFFSET, "mask past any pad"},
        {">inputUnion(0:0x1100, 0 - 4)", IO_ERROR_HOOK_BAD_MASK_OFFSET, "negative mask offset"},
        {">inputUnion(0:0x1101, 0x98)", IO_ERROR_HOOK_BAD_TARGET, "sampler off an instruction"},
        {">inputUnion(0:0x400000, 0x98)", IO_ERROR_HOOK_BAD_TARGET, "sampler past the segment"},
        {">inputUnion(2:0x1100, 0x98)", IO_ERROR_HOOK_BAD_TARGET, "sampler in a data segment"},
        {">inputUnion(0:0x1100, 0x98)", IO_OK, "the plain form"},
        {">inputUnion(0:0x1100, 0x98, arm)", IO_OK, "an arm sampler"},
        {">inputUnion( 0:0x1100 , 0x40 + 0x58 , thumb )", IO_OK, "whitespace and an expression"},
        {">inputUnion(0:0x1100, 0)", IO_OK, "the mask at the start of the pad"},
        {">inputUnion(0:0x1100, 0x1000)", IO_OK, "the last offset a pad may have"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset(FT_ENABLED, FPS_60);
        vg_io_status_code_t code = parse(cases[i].line);
        CHECK(code == cases[i].code, "%s: expected %d, got %d (%s)",
                    cases[i].what, cases[i].code, code, cases[i].line);
        // Whatever the line says, it never hooks anything on its own
        CHECK(g_fake_num == 0, "%s: '>inputUnion()' hooks nothing by itself", cases[i].what);
    }

    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">inputUnion(0:0x1100, 0x40 + 0x58)") == IO_OK, "expression mask offset");
    CHECK(g_main.input.mask_offset == 0x98, "...evaluates to 0x98 (got 0x%X)",
                g_main.input.mask_offset);
    CHECK(g_main.input.requested, "the sampler is recorded");
    CHECK(g_main.input.segment == 0 && g_main.input.offset == 0x1100, "...with its address");
    CHECK(g_main.input.thumb, "...thumb by default");
    CHECK(g_main.input.uid < 0, "...and is not hooked until a slot needs it");

    // A directive outside @FPS is dead text, like every other one
    reset(FT_ENABLED, FPS_60);
    CHECK(parse_under(">inputUnion(0:0x1100, 0x98)", FEATURE_FB) == IO_ERROR_HOOK_WRONG_FEATURE,
                "'>inputUnion()' under @FB is refused");

    // Two lines about the one pad structure that disagree are a patch bug
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">inputUnion(0:0x1100, 0x98)") == IO_OK, "first declaration");
    CHECK(parse(">inputUnion(0:0x1100, 0x98)") == IO_OK, "an identical repeat is not an error");
    CHECK(parse(">inputUnion(0:0x1100, 0xC0)") == IO_ERROR_HOOK_CONFLICT, "another mask offset");
    CHECK(parse(">inputUnion(0:0x1200, 0x98)") == IO_ERROR_HOOK_CONFLICT, "another sampler");
    CHECK(parse(">inputUnion(0:0x1100, 0x98, arm)") == IO_ERROR_HOOK_CONFLICT, "another instruction set");
    CHECK(g_main.input.offset == 0x1100 && g_main.input.mask_offset == 0x98 && g_main.input.thumb,
                "the first declaration is left alone");

    // The whole feature is off when the frame rate option is
    reset(FT_DISABLED, FPS_60);
    CHECK(parse(">inputUnion(0:0x1100, 0x98)") == IO_OK, "parses with FPS off");
    CHECK(!g_main.input.requested, "...and records nothing while the FPS option is off");
}

// 'union' on a rate divided line: what it needs, and what it refuses
static void test_input_union_slot_refusals() {
    // The sampler has to be named first: without it there is nothing to read
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x2000, 2, ret=1, frame, union)") == IO_ERROR_HOOK_UNION_NO_SAMPLER,
                "'union' with no '>inputUnion()' line is refused");
    CHECK(g_fake_num == 0, "...and hooks nothing");
    CHECK(g_main.rate_hook_num == 0, "...and takes no slot");

    // 'frame' counting is mandatory: call counted slots sharing one
    // accumulator do not agree on which polls are live
    struct { const char *line; vg_io_status_code_t code; const char *what; } cases[] = {
        {">rateDivide(0:0x2000, 2, void, union)", IO_ERROR_HOOK_UNION_NEEDS_FRAME, "no counting mode"},
        {">rateDivide(0:0x2000, 2, void, call, union)", IO_ERROR_HOOK_UNION_NEEDS_FRAME, "call counted"},
        {">rateDivide(0:0x2000, 4, void, frame, union)", IO_ERROR_HOOK_UNION_BAD_DIVISOR, "divisor 4"},
        {">rateDivide(0:0x2000, 16, void, frame, union)", IO_ERROR_HOOK_UNION_BAD_DIVISOR, "divisor 16"},
        {">rateDivide(0:0x2000, 2, void, frame, union, union)", IO_ERROR_PARSE_INVALID_TOKEN, "'union' twice"},
        {">rateDivide(0:0x2000, 2, void, frame, unions)", IO_ERROR_PARSE_INVALID_TOKEN, "junk after 'union'"},
        {">rateDivide(0:0x2000, 2, void, frame, union)", IO_OK, "divisor 2"},
        {">rateDivide(0:0x2000, 3, void, frame, union)", IO_OK, "divisor 3"},
        {">rateDivide(0:0x2000, 2, ret=1, union, frame, args=2)", IO_OK, "order independent"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset(FT_ENABLED, FPS_60);
        CHECK(parse(">inputUnion(0:0x1100, 0x98)") == IO_OK, "declare the sampler");
        vg_io_status_code_t code = parse(cases[i].line);
        CHECK(code == cases[i].code, "%s: expected %d, got %d (%s)",
                    cases[i].what, cases[i].code, code, cases[i].line);
        CHECK(fake_offset_hook_num() == (cases[i].code == IO_OK ? 1 : 0),
                    "%s: %s hooked", cases[i].what, cases[i].code == IO_OK ? "is" : "is not");
        // The sampler hook is installed with the first slot that needs it, and
        // only then - a refused slot leaves it uninstalled
        CHECK(fake_sampler_hook_num() == (cases[i].code == IO_OK ? 1 : 0),
                    "%s: the sampler is %s hooked", cases[i].what,
                    cases[i].code == IO_OK ? "" : "not");
    }

    // A divisor of 1 is a line that installs nothing here, so the accumulator's
    // range does not apply to it - it never reaches an accumulator
    reset(FT_ENABLED, FPS_30);
    CHECK(parse(">inputUnion(0:0x1100, 0x98)") == IO_OK, "declare the sampler at FPS=30");
    CHECK(parse(">rateDivide(0:0x2000, 1 + (fps_limit / 60), ret=1, frame, union)") == IO_OK,
                "an accumulating line is inert at the game's own rate");
    CHECK(g_fake_num == 0, "...and nothing at all is hooked, sampler included");
    CHECK(g_main.input.uid < 0, "...the sampler is declared but not hooked");

    // Every accumulating slot has to divide the same way: they decide on the
    // shared frame counter, so two divisors are two sets of live polls
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">inputUnion(0:0x1100, 0x98)") == IO_OK, "declare the sampler");
    CHECK(parse(">rateDivide(0:0x2000, 2, void, frame, union)") == IO_OK, "first slot");
    CHECK(parse(">rateDivide(0:0x2010, 2, void, frame, union)") == IO_OK, "second slot agrees");
    CHECK(parse(">rateDivide(0:0x2020, 3, void, frame, union)")
                == IO_ERROR_HOOK_UNION_DIVISOR_CONFLICT, "a third that divides differently");
    CHECK(g_main.rate_hook_num == 2, "...takes no slot");
    // A slot that does not accumulate is free to divide however it likes
    CHECK(parse(">rateDivide(0:0x2030, 3, void, frame)") == IO_OK,
                "a slot without 'union' is not held to the accumulator's divisor");
    CHECK(fake_sampler_hook_num() == 1, "one sampler hook for all of them");
    CHECK(fake_import_hook_num(0x7A410B64) == 1, "one frame counter as well");

    // 'union' is part of a line's identity, like every other argument
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">inputUnion(0:0x1100, 0x98)") == IO_OK, "declare the sampler");
    CHECK(parse(">rateDivide(0:0x2000, 2, ret=1, frame, union)") == IO_OK, "first line");
    CHECK(parse(">rateDivide(0:0x2000, 2, ret=1, frame, union)") == IO_OK, "identical repeat");
    CHECK(parse(">rateDivide(0:0x2000, 2, ret=1, frame)") == IO_ERROR_HOOK_CONFLICT,
                "no 'union' against 'union' is a disagreement");
    CHECK(fake_offset_hook_num() == 1, "...and hooks nothing");
    CHECK(g_main.rate_hook[0].union_input, "the accumulating slot is left alone");
}

/*
 * The whole point: a press that lands on a poll the slot skipped still reaches
 * it, exactly once, one poll late - which is what the game does natively at
 * its own rate.
 */
static void test_input_union_accumulation() {
    // A press on a live poll, one on a skipped poll, one on the next skipped
    // poll. Without accumulation the second and third are lost outright.
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    frame_sample_first(0, 0x10);        // frame 0, live
    frame_sample_first(0, 0x40);        // frame 1, skipped
    frame_sample_first(0, 0);           // frame 2, live: sees the skipped press
    frame_sample_first(0, 0x8);         // frame 3, skipped
    frame_sample_first(0, 0);           // frame 4, live: sees that one too
    frame_sample_first(0, 0);           // frame 5, skipped

    CHECK(g_sampler_calls == 6, "the sampler still runs every frame (got %d)", g_sampler_calls);
    CHECK(g_original_calls == 3, "the target still runs 1 frame in 2 (got %d)", g_original_calls);
    CHECK(g_target_saw_num == 3, "three calls read the mask (got %d)", g_target_saw_num);
    CHECK(g_target_saw[0] == 0x10, "the live press arrives on its own frame (0x%X)", g_target_saw[0]);
    CHECK(g_target_saw[1] == 0x40, "the skipped press arrives one frame late (0x%X)", g_target_saw[1]);
    CHECK(g_target_saw[2] == 0x8, "...and so does the next one (0x%X)", g_target_saw[2]);

    // Nothing is delivered twice: three presses, three bits, and no bit is in
    // two of the words the target saw
    CHECK((g_target_saw[0] & g_target_saw[1]) == 0 && (g_target_saw[1] & g_target_saw[2]) == 0
                && (g_target_saw[0] & g_target_saw[2]) == 0, "no press is presented twice");

    // The same run without 'union' is the bug the directive exists to fix
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">rateDivide(0:0x2000, 2, ret=1, frame)") == IO_OK, "install without 'union'");
    g_target_reads_mask = true;
    for (int frame = 0; frame < 6; frame++) {
        static const uint32_t masks[6] = {0x10, 0x40, 0, 0x8, 0, 0};
        *pad_mask() = masks[frame];
        call_game_function(0, (uintptr_t)g_pad);
        present_frame();
    }
    CHECK(g_target_saw_num == 3, "the same three calls are made");
    CHECK(g_target_saw[0] == 0x10 && g_target_saw[1] == 0 && g_target_saw[2] == 0,
                "without accumulation both skipped presses are lost (0x%X, 0x%X, 0x%X)",
                g_target_saw[0], g_target_saw[1], g_target_saw[2]);

    // Invisibility: outside the call the word holds the true value of the poll
    // it belongs to, so every full rate consumer sees what it would natively
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    frame_sample_first(0, 0x10);
    CHECK(*pad_mask() == 0x10, "after a live call the word is that poll's own value (0x%X)",
                *pad_mask());
    frame_sample_first(0, 0x40);
    CHECK(*pad_mask() == 0x40, "a skipped call touches the word at all (0x%X)", *pad_mask());
    frame_sample_first(0, 0);
    CHECK(g_target_saw[1] == 0x40, "the live call saw the union");
    CHECK(*pad_mask() == 0, "...and left the word at the true value of its own poll (0x%X)",
                *pad_mask());

    // Divisor 3: two skipped polls, both accumulated, into one call
    install_union(">rateDivide(0:0x2000, 3, ret=1, frame, union)");
    frame_sample_first(0, 0x1);         // frame 0, live
    frame_sample_first(0, 0x2);         // frame 1, skipped
    frame_sample_first(0, 0x4);         // frame 2, skipped
    frame_sample_first(0, 0x8);         // frame 3, live
    CHECK(g_original_calls == 2, "1 frame in 3 (got %d)", g_original_calls);
    CHECK(g_target_saw[0] == 0x1, "the first live poll (0x%X)", g_target_saw[0]);
    CHECK(g_target_saw[1] == 0xE, "its own poll and both skipped ones (0x%X)", g_target_saw[1]);
    CHECK(*pad_mask() == 0x8, "the word is left at the live poll's own value (0x%X)", *pad_mask());

    // The other frame loop ordering - the update runs on the mask the previous
    // frame's poll built - reaches the same place without being told about it
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    frame_update_first(0, 0x10);        // frame 0, live: the sampler has not run yet
    frame_update_first(0, 0);           // frame 1, skipped
    frame_update_first(0, 0x40);        // frame 2, live: sees the frame 0 press
    frame_update_first(0, 0);           // frame 3, skipped
    frame_update_first(0, 0);           // frame 4, live: sees the frame 2 press
    CHECK(g_original_calls == 3, "1 frame in 2 in this ordering too (got %d)", g_original_calls);
    CHECK(g_target_saw[0] == 0, "nothing had been sampled before the first call (0x%X)",
                g_target_saw[0]);
    CHECK(g_target_saw[1] == 0x10, "the press of the poll this call skipped (0x%X)",
                g_target_saw[1]);
    CHECK(g_target_saw[2] == 0x40, "...and the next one (0x%X)", g_target_saw[2]);
}

/*
 * The residual is read at sampler ENTRY, which is what makes a consumer's
 * swallow work and what keeps the engine's -1 out of the accumulator.
 */
static void test_input_union_residual() {
    // A full rate consumer that acts on a press zeroes the word so that nobody
    // else sees it. Read at entry, that swallow has already happened, so the
    // press contributes nothing to the next union - exactly as natively.
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    frame_sample_first(0, 0);           // frame 0, live
    g_poll_mask = 0x40;
    call_sampler();                     // frame 1 poll builds a press...
    *pad_mask() = 0;                    // ...and a full rate consumer swallows it
    call_game_function(0, (uintptr_t)g_pad);
    present_frame();
    frame_sample_first(0, 0);           // frame 2, live
    CHECK(g_target_saw_num == 2, "two live calls (got %d)", g_target_saw_num);
    CHECK(g_target_saw[1] == 0, "a swallowed press is not handed to the minigame (0x%X)",
                g_target_saw[1]);

    // -1 is the engine's "input disabled" value, not a set of pulses. In the
    // ring it would read as every bit set and suppress a whole live poll.
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    frame_sample_first(0, 0);
    g_poll_mask = 0xFFFFFFFF;
    call_sampler();
    call_game_function(0, (uintptr_t)g_pad);
    present_frame();
    frame_sample_first(0, 0x10);
    CHECK(g_target_saw[1] == 0x10, "a -1 residual does not reach the union (0x%X)",
                g_target_saw[1]);

    // ...but a live poll that is itself -1 stays -1: the game has disabled
    // input for this frame, and accumulating into it must not undo that
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    frame_sample_first(0, 0);
    frame_sample_first(0, 0x40);        // skipped, a real press
    frame_sample_first(0, 0xFFFFFFFF);  // live, but input is disabled
    CHECK(g_target_saw[1] == 0xFFFFFFFF, "a disabled poll absorbs the union (0x%X)",
                g_target_saw[1]);
    CHECK(*pad_mask() == 0xFFFFFFFF, "...and the word is left disabled (0x%X)", *pad_mask());

    // A poll something has already swallowed still lets the stale bits nobody
    // has handled through
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    frame_sample_first(0, 0);
    frame_sample_first(0, 0x40);        // skipped, a real press
    g_poll_mask = 0x8;
    call_sampler();
    *pad_mask() = 0;                    // a consumer swallows this poll's own press
    call_game_function(0, (uintptr_t)g_pad);
    CHECK(g_target_saw[1] == 0x40, "the stale press is still delivered (0x%X)", g_target_saw[1]);
}

/*
 * The exit test: the union is taken back out unless the game replaced the word
 * during the call, in which case that write is the game's and it stays.
 */
static void test_input_union_exit() {
    // Untouched: the true one poll value goes back
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    g_target_write = TARGET_LEAVES_MASK;
    frame_sample_first(0, 0x10);
    CHECK(g_target_saw[0] == 0x10, "the call saw its own poll");
    CHECK(*pad_mask() == 0x10, "an untouched word is restored (0x%X)", *pad_mask());

    // The target swallows: that is the game saying nobody else may act on it,
    // and it has to survive the wrapper's exit
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    g_target_write = TARGET_SWALLOWS_MASK;
    frame_sample_first(0, 0x10);
    CHECK(g_target_saw[0] == 0x10, "the call saw the press");
    CHECK(*pad_mask() == 0, "a swallow by the target is not undone (0x%X)", *pad_mask());

    // ...including a swallow of a press that came out of the accumulator,
    // which is the one leak this design has and it is bounded to that frame
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    g_target_write = TARGET_SWALLOWS_MASK;
    frame_sample_first(0, 0);
    frame_sample_first(0, 0x40);        // skipped
    frame_sample_first(0, 0x10);        // live: sees 0x50, swallows the lot
    CHECK(g_target_saw[1] == 0x50, "the live call saw both polls (0x%X)", g_target_saw[1]);
    CHECK(*pad_mask() == 0, "the target's swallow stands (0x%X)", *pad_mask());

    // The game writing -1 during the call is a replacement too
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    g_target_write = TARGET_DISABLES_MASK;
    frame_sample_first(0, 0x10);
    CHECK(*pad_mask() == 0xFFFFFFFF, "the game disabling input during the call stands (0x%X)",
                *pad_mask());

    // A call made before the sampler has ever run has no pad address to work
    // from: it is made exactly as a slot without 'union' would make it
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    *pad_mask() = 0x10;
    CHECK(g_main.input.mask_addr == NULL, "the sampler has not published a pad yet");
    CHECK(call_game_function(0, (uintptr_t)g_pad) == FAKE_ORIGINAL_RET, "the call is still made");
    CHECK(g_target_saw[0] == 0x10, "...and reads the word as it stands (0x%X)", g_target_saw[0]);
    CHECK(*pad_mask() == 0x10, "...and nothing is substituted into it (0x%X)", *pad_mask());
    CHECK(g_main.input.head == 0, "...and no poll has been accumulated");

    // A call the hooked function makes into itself is already inside the
    // outermost call's substitution, so it sees the union and the outermost
    // call is still the one that puts the word back
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    frame_sample_first(0, 0);
    g_poll_mask = 0x40;
    call_sampler();
    call_game_function(0, (uintptr_t)g_pad);
    present_frame();
    g_poll_mask = 0x10;
    call_sampler();
    g_recurse_slot = 0;
    g_recurse_left = 1;
    call_game_function(0, (uintptr_t)g_pad);
    CHECK(g_target_saw[1] == 0x50 && g_target_saw[2] == 0x50,
                "a recursive call sees the same union (0x%X, 0x%X)",
                g_target_saw[1], g_target_saw[2]);
    CHECK(*pad_mask() == 0x10, "...and the outermost call still restores the poll (0x%X)",
                *pad_mask());
}

/*
 * Two minigame halves that are rate divided separately have to land on the
 * same polls, or only one of them sees the press. That is what 'frame' buys,
 * and it is why 'union' insists on it.
 */
static void test_input_union_pairing() {
    reset(FT_ENABLED, FPS_60);
    CHECK(parse(">inputUnion(0:0x1100, 0x98)") == IO_OK, "declare the sampler");
    CHECK(parse(">rateDivide(0:0x2000, 2, ret=1, frame, union)") == IO_OK, "first half");
    CHECK(parse(">rateDivide(0:0x2010, 2, void, frame, union)") == IO_OK, "second half");
    bind_sampler_original();
    g_target_reads_mask = true;
    CHECK(fake_sampler_hook_num() == 1, "one sampler hook between them");
    CHECK(fake_offset_hook_num() == 2, "two accumulating slots");
    CHECK(g_main.input.divisor == 2, "the accumulator is shared at divisor 2");

    for (int frame = 0; frame < 6; frame++) {
        static const uint32_t masks[6] = {0, 0x40, 0, 0x8, 0, 0};
        g_poll_mask = masks[frame];
        call_sampler();
        call_game_function(0, (uintptr_t)g_pad);
        call_game_function(1, (uintptr_t)g_pad);
        present_frame();
    }

    CHECK(g_original_calls == 6, "both halves run on the same three frames (got %d)",
                g_original_calls);
    CHECK(g_target_saw_num == 6, "six calls read the mask (got %d)", g_target_saw_num);
    // Frames 0, 2 and 4 are live, and the two halves are called back to back
    CHECK(g_target_saw[0] == 0 && g_target_saw[1] == 0, "frame 0: nothing to see");
    CHECK(g_target_saw[2] == 0x40 && g_target_saw[3] == 0x40,
                "frame 2: both halves see the skipped press (0x%X, 0x%X)",
                g_target_saw[2], g_target_saw[3]);
    CHECK(g_target_saw[4] == 0x8 && g_target_saw[5] == 0x8,
                "frame 4: both halves see the next one (0x%X, 0x%X)",
                g_target_saw[4], g_target_saw[5]);
    CHECK(*pad_mask() == 0, "the word is the last poll's own value again (0x%X)", *pad_mask());
}

// Unload: the sampler hook is released with the slots that read it, and a call
// already on its way in must not follow a chain that is being released
static void test_input_union_release() {
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    CHECK(g_main.input.armed, "the sampler hook is armed once it is installed");
    frame_sample_first(0, 0x10);
    CHECK(g_main.input.mask_addr != NULL, "the sampler published the pad");
    CHECK(g_sampler_calls == 1, "one poll so far");

    release_all_hooks();
    for (int i = 0; i < g_fake_num; i++) {
        CHECK(g_fake[i].released, "hook %d is released", i);
    }
    CHECK(!g_main.input.armed, "the sampler hook is disarmed");
    CHECK(g_main.input.mask_addr == NULL, "...and the published pad is dropped");
    CHECK(g_main.input.uid < 0, "...and the slot is free again");
    CHECK(!g_main.input.requested, "...and the declaration is gone with it");
    CHECK(g_main.input.divisor == 0, "...and so is the divisor it was shared at");

    // The fake hooks are still in place, so a sampler body that ignored 'armed'
    // would reach its original through a released ref
    g_poll_mask = 0x40;
    *pad_mask() = 0x10;
    CHECK(call_sampler() == 0, "a poll after release returns without continuing");
    CHECK(g_sampler_calls == 1, "...and follows no released chain");
    CHECK(*pad_mask() == 0x10, "...and rebuilds nothing");

    // ...and a rate divided slot that was accumulating is disarmed with the
    // rest, so it substitutes nothing into a word it no longer owns
    g_original_calls = 0;
    CHECK(call_game_function(0, (uintptr_t)g_pad) == 1, "a call after release is skipped");
    CHECK(g_original_calls == 0, "...and makes no call");
    CHECK(*pad_mask() == 0x10, "...and leaves the word alone (0x%X)", *pad_mask());

    // A second run of the plugin over the same title starts from nothing
    install_union(">rateDivide(0:0x2000, 2, ret=1, frame, union)");
    CHECK(g_main.input.head == 0, "the accumulator starts empty");
    CHECK(g_main.input.mask_addr == NULL, "...with no pad published");
    frame_sample_first(0, 0x10);
    CHECK(g_target_saw[0] == 0x10, "...and accumulates from there (0x%X)", g_target_saw[0]);
}

// The shape the Trails patches use: the sampler declared once, then the
// minigame updates over it, all dividing the same way and counted in frames
static void test_input_union_patch_shaped_lines() {
    static const char *lines[] = {
        ">inputUnion(0:0xC466C, 0xC0)                                    # input sampler",
        ">rateDivide(0:0xECC38, 1 + (fps_limit / 60), ret=1, frame, union)   # minigame update",
        ">rateDivide(0:0xE0E6E, 1 + (fps_limit / 60), void, frame, union)    # its second half"
    };
    const int line_num = sizeof(lines) / sizeof(lines[0]);

    reset(FT_ENABLED, FPS_60);
    for (int i = 0; i < line_num; i++) {
        CHECK(parse(lines[i]) == IO_OK, "patch line %d installs", i);
    }
    CHECK(fake_sampler_hook_num() == 1, "one sampler hook");
    CHECK(fake_offset_hook_num() == 2, "two accumulating slots");
    CHECK(fake_import_hook_num(0x7A410B64) == 1, "one frame counter");
    CHECK(g_main.input.mask_offset == 0xC0, "the mask is at pad+0xC0");
    CHECK(g_main.rate_hook[0].union_input && g_main.rate_hook[1].union_input,
                "both slots accumulate");

    // The same file at the game's own rate hooks nothing at all
    reset(FT_ENABLED, FPS_30);
    for (int i = 0; i < line_num; i++) {
        CHECK(parse(lines[i]) == IO_OK, "patch line %d parses at FPS=30", i);
    }
    CHECK(g_fake_num == 0, "a 30 FPS run hooks nothing, sampler included");
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
    test_rate_divide_recursion();
    test_rate_divide_calls();
    test_rate_divide_frame_counted();
    test_rate_divide_counter_wrap();
    test_slots();
    test_release();
    test_patch_shaped_lines();
    test_input_union_parse_errors();
    test_input_union_slot_refusals();
    test_input_union_accumulation();
    test_input_union_residual();
    test_input_union_exit();
    test_input_union_pairing();
    test_input_union_release();
    test_input_union_patch_shaped_lines();

    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
