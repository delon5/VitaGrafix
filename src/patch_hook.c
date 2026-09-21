#include <vitasdk.h>
#include <taihen.h>
#include <stdbool.h>
#include <strings.h>

#include "io.h"
#include "log.h"
#include "config.h"
#include "patch.h"
#include "patch_hook.h"
#include "main.h"

#include "interpreter/interpreter.h"

// A rate divided call runs 1 time in N. Anything above this is a patch typo
// rather than a frame rate ratio, and would stall the game function forever.
#define RATE_DIVISOR_MAX 16

typedef enum {
    // taiHookFunctionImport() by NID, for the named Sce function directives
    HOOK_KIND_IMPORT,
    // taiHookFunctionOffset() into the game, for '>rateDivide()'
    HOOK_KIND_RATE_DIVIDE
} vg_hook_kind_t;

typedef struct {
    vg_hook_kind_t kind;

    // import hooks
    vg_hook_id_t hook_id;
    uint32_t import_nid;
    const void *hook_ptr;

    // rate divided hooks
    uint8_t segment;
    uint32_t offset;
    bool thumb;
    uint32_t divisor;
    uint32_t ret_value;
} vg_hook_request_t;

int vg_hook_sceDisplaySetFrameBuf_withWait(const SceDisplayFrameBuf *pParam, int sync) {
    int ret = TAI_CONTINUE(int, g_main.hook_ref[HOOK_DISPLAY_SET_FRAMEBUF_WITH_WAIT], pParam, sync);
    sceDisplayWaitVblankStartMulti(2);
    return ret;
}

int vg_hook_sceCtrlReadBufferPositive_peekPatched(int port, SceCtrlData *pad_data, int count) {
    return sceCtrlPeekBufferPositive(port, pad_data, count);
}

int vg_hook_sceCtrlReadBufferPositive2_peekPatched(int port, SceCtrlData *pad_data, int count) {
    return sceCtrlPeekBufferPositive2(port, pad_data, count);
}

/**
 * Counts displayed frames. The rate divided hooks decide from this counter and
 * not from a per hook call counter, so that a game function called several
 * times in one frame is either made or skipped for that whole frame.
 */
int vg_hook_sceDisplaySetFrameBuf_rateCounter(const SceDisplayFrameBuf *pParam, int sync) {
    g_main.frame++;
    return TAI_CONTINUE(int, g_main.hook_ref[HOOK_RATE_FRAME_COUNTER], pParam, sync);
}

/**
 * Body shared by every rate divided wrapper.
 *
 * taiHEN hands a hook body no context of its own, so each slot gets a distinct
 * static wrapper below which knows its own index and lands here.
 *
 * NOTE: up to four register arguments are carried through, which covers the
 * per frame update functions this exists for. A target that takes stack
 * arguments, or returns a float or a struct, is out of scope.
 */
static inline int vg_hook_rate_divide_call(uint32_t slot, int a0, int a1, int a2, int a3) {
    const vg_rate_hook_t *rate_hook = &g_main.rate_hook[slot];

    // The call the game makes in frame 0, and in every divisor-th frame after
    // it, is made for real; the ones in between return the patch's substitute
    if (rate_hook->divisor > 1 && (g_main.frame % rate_hook->divisor) != 0)
        return (int)rate_hook->ret_value;

    return TAI_CONTINUE(int, rate_hook->ref, a0, a1, a2, a3);
}

#define VG_RATE_WRAPPER(n) \
    static int vg_hook_rate_divide_##n(int a0, int a1, int a2, int a3) { \
        return vg_hook_rate_divide_call(n, a0, a1, a2, a3); \
    }
#define VG_RATE_WRAPPER_REF(n) (const void *)&vg_hook_rate_divide_##n,

#define VG_RATE_WRAPPER_LIST(X) \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7) \
    X(8)  X(9)  X(10) X(11) X(12) X(13) X(14) X(15) \
    X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23)

VG_RATE_WRAPPER_LIST(VG_RATE_WRAPPER)

static const void * const _RATE_WRAPPERS[] = {
    VG_RATE_WRAPPER_LIST(VG_RATE_WRAPPER_REF)
};

// One wrapper per slot, or a slot would have no body to hook with
typedef char vg_hook_rate_wrapper_count_must_match[
        sizeof(_RATE_WRAPPERS) / sizeof(_RATE_WRAPPERS[0]) == MAX_RATE_HOOK_NUM ? 1 : -1];

static vg_io_status_t vg_hook_function_import(vg_hook_id_t hook_id, uint32_t nid, const void *func) {
    // Each hook has exactly one slot. Hooking the same import twice would
    // overwrite the first hook's uid/ref (leaking it, and making its
    // TAI_CONTINUE re-enter the hook forever), so treat a repeat as a no-op.
    if (g_main.hook[hook_id] >= 0) {
        vg_log_printf("[HOOK] Function import nid=0x%X is already hooked, skipping\n", nid);
        __ret_status(IO_OK, 0, 0);
    }

    vg_log_printf("[HOOK] Hooking function import nid=0x%X to 0x%X\n", nid, func);

    g_main.hook[hook_id] = taiHookFunctionImport(&g_main.hook_ref[hook_id], TAI_MAIN_MODULE, TAI_ANY_LIBRARY, nid, func);
    if (g_main.hook[hook_id] < 0) {
        __ret_status(IO_ERROR_TAI_GENERIC, 0, 0);
    }

    __ret_status(IO_OK, 0, 0);
}

/**
 * Installs the game function hook that a '>rateDivide()' directive asked for.
 */
static vg_io_status_t vg_hook_function_offset_rate_divide(const vg_hook_request_t *request) {
    // Hooking one site twice would overwrite the first hook's uid/ref, and its
    // TAI_CONTINUE would then re-enter the wrapper forever (see above)
    for (uint32_t i = 0; i < g_main.rate_hook_num; i++) {
        if (g_main.rate_hook[i].segment == request->segment
                && g_main.rate_hook[i].offset == request->offset) {
            vg_log_printf("[HOOK] seg%03d : %08X is already rate divided, skipping\n",
                        request->segment, request->offset);
            __ret_status(IO_OK, 0, 0);
        }
    }

    if (g_main.rate_hook_num >= MAX_RATE_HOOK_NUM) {
        vg_log_printf("[HOOK] Too many rate divided hooks, limit: " TOSTRING(MAX_RATE_HOOK_NUM) "\n");
        __ret_status(IO_ERROR_TOO_MANY_PATCHES, 0, 0);
    }

    // The parity source has to be running before the first divided call
    if (g_main.hook[HOOK_RATE_FRAME_COUNTER] < 0) {
        vg_io_status_t ret = vg_hook_function_import(HOOK_RATE_FRAME_COUNTER, 0x7A410B64,
                    &vg_hook_sceDisplaySetFrameBuf_rateCounter);
        if (ret.code != IO_OK)
            return ret;
    }

    uint32_t slot = g_main.rate_hook_num;
    vg_rate_hook_t *rate_hook = &g_main.rate_hook[slot];

    // Configure the slot before arming the hook, never after: the wrapper can
    // be entered as soon as taiHookFunctionOffset() returns
    rate_hook->segment = request->segment;
    rate_hook->offset = request->offset;
    rate_hook->divisor = request->divisor;
    rate_hook->ret_value = request->ret_value;

    vg_log_printf("[HOOK] Hooking seg%03d : %08X (%s) to 0x%X, 1 call in %u frames, skipped call returns 0x%X\n",
                request->segment, request->offset, request->thumb ? "thumb" : "arm",
                _RATE_WRAPPERS[slot], request->divisor, request->ret_value);

    rate_hook->uid = taiHookFunctionOffset(&rate_hook->ref, g_main.tai_info.modid,
                request->segment, request->offset, request->thumb ? 1 : 0, _RATE_WRAPPERS[slot]);
    if (rate_hook->uid < 0) {
        rate_hook->divisor = 0;
        __ret_status(IO_ERROR_TAI_GENERIC, 0, 0);
    }

    g_main.rate_hook_num++;
    __ret_status(IO_OK, 0, 0);
}

/**
 * Evaluates one directive argument with the patch interpreter, so that a
 * divisor can be written in terms of the configuration (e.g. 'fps_limit / 30').
 */
static vg_io_status_t vg_hook_parse_argument(const char line[], int *pos, uint32_t *value) {
    intp_value_t parsed = {0};
    uint32_t intp_pos = *pos;

    intp_status_t intp_ret = intp_evaluate_arg(line, &intp_pos, &parsed);
    if (intp_ret.code != INTP_STATUS_OK) {
        char buf[256];
        intp_format_error(line, intp_ret, buf, 256);
        vg_log_printf("%s\n", buf);

        __ret_status(IO_ERROR_INTERPRETER_ERROR, 0, intp_ret.pos);
    }

    // Raw byte strings and floats have no meaning as a divisor or as a value
    // returned in r0
    if (parsed.type != DATA_TYPE_SIGNED && parsed.type != DATA_TYPE_UNSIGNED)
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, *pos);

    *value = parsed.data.uint32;
    *pos = intp_pos;
    __ret_status(IO_OK, 0, 0);
}

/**
 * Parses '>rateDivide(<seg>:<offset>, <divisor>, void|ret=<value>[, thumb|arm])'
 *
 * The game function at <seg>:<offset> is then called once every <divisor>
 * displayed frames instead of on every one, which keeps logic that is written
 * as one step per rendered frame at its original speed while the display runs
 * unlocked. <divisor> is an interpreter expression, so it can be written
 * against the configured frame rate ('fps_limit / 30' for a game whose own rate
 * is 30) and collapses to 1 - no hook at all - at the game's native rate.
 *
 * The skipped call never reaches the game, so its return value is made up:
 * 'void' for a function whose result the game discards (0 is returned), or
 * 'ret=<value>' for one whose result is read.
 */
static vg_io_status_t vg_hook_parse_rate_divide(const char line[], int pos, vg_hook_request_t *request) {
    vg_io_status_t ret;
    bool return_specified = false;

    while (isspace(line[pos])) { pos++; }
    if (line[pos] != '(')
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
    pos++;

    // Target address
    while (isspace(line[pos])) { pos++; }
    ret = vg_io_parse_address(line, &pos, &request->segment, &request->offset);
    if (ret.code != IO_OK)
        return ret;

    while (isspace(line[pos])) { pos++; }
    if (line[pos] != ',')
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
    pos++;

    // Divisor
    int pos_divisor = pos;
    while (isspace(line[pos_divisor])) { pos_divisor++; }
    ret = vg_hook_parse_argument(line, &pos, &request->divisor);
    if (ret.code != IO_OK)
        return ret;
    if (request->divisor > RATE_DIVISOR_MAX)
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos_divisor);

    // Return handling. A skipped call fabricates whatever the game reads back,
    // so the patch has to say what that is - there is no safe default
    while (isspace(line[pos])) { pos++; }
    if (line[pos] != ',')
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
    pos++;

    while (isspace(line[pos])) { pos++; }
    if (!strncasecmp(&line[pos], "void", 4)) {
        pos += 4;
        request->ret_value = 0;
        return_specified = true;
    } else if (!strncasecmp(&line[pos], "ret", 3)) {
        pos += 3;
        while (isspace(line[pos])) { pos++; }
        if (line[pos] != '=')
            __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
        pos++;

        ret = vg_hook_parse_argument(line, &pos, &request->ret_value);
        if (ret.code != IO_OK)
            return ret;
        return_specified = true;
    }
    if (!return_specified)
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);

    // Instruction set of the target, Thumb unless the patch says otherwise
    request->thumb = true;
    while (isspace(line[pos])) { pos++; }
    if (line[pos] == ',') {
        pos++;
        while (isspace(line[pos])) { pos++; }
        if (!strncasecmp(&line[pos], "thumb", 5)) {
            request->thumb = true;
            pos += 5;
        } else if (!strncasecmp(&line[pos], "arm", 3)) {
            request->thumb = false;
            pos += 3;
        } else {
            __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
        }
        while (isspace(line[pos])) { pos++; }
    }

    if (line[pos] != ')')
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
    pos++;
    if (!vg_io_is_line_end(line, pos))
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);

    request->kind = HOOK_KIND_RATE_DIVIDE;
    __ret_status(IO_OK, 0, 0);
}

static vg_io_status_t vg_hook_parse_common(const char line[], vg_hook_request_t *request, uint8_t *shall_hook) {
    vg_io_status_t ret = {IO_OK, 0, 0};
    const vg_config_t *config = vg_config_get();

    if (!strncasecmp(&line[1], "sceDisplaySetFrameBuf_withWait", 30)) {
        request->kind = HOOK_KIND_IMPORT;
        request->hook_id = HOOK_DISPLAY_SET_FRAMEBUF_WITH_WAIT;
        request->import_nid = 0x7A410B64;
        request->hook_ptr = &vg_hook_sceDisplaySetFrameBuf_withWait;
        *shall_hook = config->fps_enabled == FT_ENABLED && config->fps == FPS_30;
        return ret;
    }
    if (!strncasecmp(&line[1], "sceCtrlReadBufferPositive_peekPatched", 37)) {
        request->kind = HOOK_KIND_IMPORT;
        request->hook_id = HOOK_CTRL_READ_BUFFER_POSITIVE;
        request->import_nid = 0x67E7AB83;
        request->hook_ptr = &vg_hook_sceCtrlReadBufferPositive_peekPatched;
        *shall_hook = config->fps_enabled == FT_ENABLED && config->fps == FPS_60;
        return ret;
    }
    if (!strncasecmp(&line[1], "sceCtrlReadBufferPositive2_peekPatched", 38)) {
        request->kind = HOOK_KIND_IMPORT;
        request->hook_id = HOOK_CTRL_READ_BUFFER_POSITIVE2;
        request->import_nid = 0xC4226A3E;
        request->hook_ptr = &vg_hook_sceCtrlReadBufferPositive2_peekPatched;
        *shall_hook = config->fps_enabled == FT_ENABLED && config->fps == FPS_60;
        return ret;
    }
    if (!strncasecmp(&line[1], "rateDivide", 10)) {
        // 1 for the leading '>', 10 for the directive name
        ret = vg_hook_parse_rate_divide(line, 1 + 10, request);
        if (ret.code != IO_OK)
            return ret;

        // Inert unless the frame rate option is on and is actually running
        // the game faster than it was written for. At the game's own rate the
        // divisor collapses to 1 and nothing is hooked at all.
        *shall_hook = config->fps_enabled == FT_ENABLED && request->divisor > 1;
        if (!*shall_hook) {
            vg_log_printf("[HOOK] Not rate dividing seg%03d : %08X (fps_enabled=%d, divisor=%u)\n",
                        request->segment, request->offset, config->fps_enabled, request->divisor);
        }
        return ret;
    }

    __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, 1);
}

/**
 * Parses and applies a common hook
 */
vg_io_status_t vg_hook_parse_patch(const char line[]) {
    vg_hook_request_t request = {0};
    uint8_t shall_hook = 0;
    vg_io_status_t ret = {IO_OK, 0, 0};

    // Check for common hook
    ret = vg_hook_parse_common(line, &request, &shall_hook);
    if (ret.code != IO_OK)
        return ret;

    // Apply
    if (shall_hook) {
        if (request.kind == HOOK_KIND_RATE_DIVIDE)
            return vg_hook_function_offset_rate_divide(&request);

        return vg_hook_function_import(request.hook_id, request.import_nid, request.hook_ptr);
    }

    return ret;
}
