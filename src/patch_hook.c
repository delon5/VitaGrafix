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

// RATE_DIVISOR_MAX lives in main.h, next to the slot pool size, because
// src/io.c renders it into the error string for a divisor out of range.

// RATE_COUNT_MODULUS, RATE_STALL_CALLS and RATE_REENTRY_MAX live in main.h as
// well, with the slot fields they belong to.

// Arguments carried through the wrapper: r0-r3, and nothing else (see the ABI
// note on vg_hook_rate_divide_call)
#define RATE_ARG_MAX 4

// The wrappers over game functions take their arguments as uintptr_t rather
// than int. On the Vita the two are the same 32 bit register and the ABI is
// identical; the difference is that a first argument which is a pointer - the
// pad structure the input sampler is handed - survives on a 64 bit host, so
// tests/test_hook.c can drive these bodies with a real object.
typedef uintptr_t vg_hook_arg_t;

// taiHEN relocates the first instructions of a hooked function into its own
// stub, so a target needs at least this much of its segment left after it
#define RATE_TARGET_MIN_BYTES 16

// Execute bit of SceKernelSegmentInfo::perms, which carries the ELF program
// header flags (PF_X): a Vita text segment reads 0x5 (read+execute), a data
// segment 0x6 (read+write)
#define RATE_SEGMENT_PERM_EXEC 0x1

// vg_hook_kind_t and vg_hook_request_t live in patch_hook.h: the host side
// checker in tests/test_patchlist.c reports what a directive resolved to, and
// it has to read that out of the very request the plugin would act on.

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
 * Counts displayed frames, for the rate divided hooks that asked to be counted
 * in frames rather than in their own calls. Installed with the first such hook
 * and not before.
 *
 * The increment is atomic because whichever thread presents is not necessarily
 * the thread that runs the hooked function, and folds back at
 * RATE_COUNT_MODULUS so that no divisor sees a short interval at the wrap.
 * Exactly one caller ever observes the fold value, so the counter is reduced
 * once; another thread reading a value a little above the modulus in the
 * meantime gets the same remainder it would have got below it.
 */
int vg_hook_sceDisplaySetFrameBuf_rateCounter(const SceDisplayFrameBuf *pParam, int sync) {
    if (__atomic_add_fetch(&g_main.frame, 1, __ATOMIC_ACQ_REL) == RATE_COUNT_MODULUS)
        __atomic_fetch_sub(&g_main.frame, RATE_COUNT_MODULUS, __ATOMIC_ACQ_REL);

    return TAI_CONTINUE(int, g_main.hook_ref[HOOK_RATE_FRAME_COUNTER], pParam, sync);
}

// MAX_INPUT_UNION_RING has to be a power of two, or the write index folding
// back at 2^32 would land in the wrong slot once
typedef char vg_hook_input_ring_must_be_pow2[
        (MAX_INPUT_UNION_RING & (MAX_INPUT_UNION_RING - 1)) == 0 ? 1 : -1];

// ...and it has to hold every poll a 'union' slot skipped
typedef char vg_hook_input_ring_must_hold_a_window[
        MAX_INPUT_UNION_RING >= INPUT_UNION_DIVISOR_MAX - 1 ? 1 : -1];

/**
 * The game's input sampler, hooked at ENTRY by '>inputUnion()'.
 *
 * It does two things and writes nothing the game can see:
 *
 *  - publishes the address of the pulse mask, computed from the pad structure
 *    the sampler is handed as its first argument, so that a rate divided
 *    wrapper can reach the field without a per title global address;
 *  - reads the mask BEFORE the sampler rebuilds it, which is the residual of
 *    the poll that has just ended, and pushes it into the ring.
 *
 * Reading at entry rather than at exit is what makes a consumer's swallow work.
 * A consumer that acts on a press zeroes the mask so that nobody else sees it;
 * at entry that swallow has already happened, so the residual is 0 and
 * contributes nothing to any later union - exactly as it does natively, where
 * a swallow denies the word to every later consumer including the rate divided
 * one. Accumulating the mask as just built would instead hand the minigame a
 * press another consumer had already acted on, and it would be acted on twice.
 *
 * It also makes the design independent of where in the frame loop the sampler
 * sits: the titles this was written for differ (one updates before it samples,
 * the others sample first), and with entry reads each poll reaches exactly one
 * call that a 'union' slot makes.
 */
static int vg_hook_input_union_sampler(vg_hook_arg_t a0, vg_hook_arg_t a1,
            vg_hook_arg_t a2, vg_hook_arg_t a3) {
    vg_input_hook_t *input = &g_main.input;

    // module_stop clears this before releasing the hook. Unlike a rate divided
    // target there is nothing useful to substitute for the sampler - it is the
    // whole game's input - so the cost of the unload window is one unsampled
    // poll, against following a chain that is being torn down.
    if (!__atomic_load_n(&input->armed, __ATOMIC_ACQUIRE))
        return 0;

    // A sampler called with no pad is not something the titles studied do, but
    // the hook dereferences this argument on every poll, so it is checked
    if (a0 != 0) {
        uint32_t *mask = (uint32_t *)(a0 + input->mask_offset);
        __atomic_store_n(&input->mask_addr, mask, __ATOMIC_RELEASE);

        uint32_t residual = *mask;
        // -1 is the engine's "input disabled" value, not a set of pulses: the
        // bit helpers every consumer goes through return 0 for it. OR'd into a
        // union it would read as every bit set and suppress a whole live poll.
        if (residual == INPUT_MASK_DISABLED)
            residual = 0;

        uint32_t head = __atomic_load_n(&input->head, __ATOMIC_ACQUIRE);
        __atomic_store_n(&input->ring[head % MAX_INPUT_UNION_RING], residual, __ATOMIC_RELEASE);
        __atomic_store_n(&input->head, head + 1, __ATOMIC_RELEASE);
    }

    return TAI_CONTINUE(int, input->ref, a0, a1, a2, a3);
}

/**
 * Makes one call of a 'union' slot with the skipped polls' pulses folded into
 * the mask, and takes them out again when the call returns.
 *
 * The union exists in the word only for the interval [entry, exit] of this one
 * call. That is what keeps it invisible: everything the hooked function calls
 * sees it, which is right because those callees run at the divided rate, and
 * nothing else in the frame runs during the interval. Writing the union into
 * the mask from the sampler instead would leave it there for the whole frame,
 * where every full rate consumer would read it - and a press from a skipped
 * poll, which those consumers already acted on when it was fresh, would be
 * acted on a second time.
 *
 * The exit test is 'is the word still what we put there'. Untouched means no
 * consumer inside the call wrote it, so the true one poll value goes back.
 * Anything else means the game replaced the word - a swallow (a literal 0) or
 * the -1 disable sentinel - and that write is the game's, so it stays.
 */
static int vg_hook_rate_union_call(vg_rate_hook_t *rate_hook, uint32_t divisor,
            vg_hook_arg_t a0, vg_hook_arg_t a1, vg_hook_arg_t a2, vg_hook_arg_t a3) {
    vg_input_hook_t *input = &g_main.input;
    uint32_t *mask = __atomic_load_n(&input->mask_addr, __ATOMIC_ACQUIRE);

    // The sampler has not run yet, so there is no pad address and no poll to
    // accumulate. Make the call exactly as a slot without 'union' would.
    if (mask == NULL)
        return TAI_CONTINUE(int, rate_hook->ref, a0, a1, a2, a3);

    uint32_t saved = *mask;

    // 'saved | ring' rather than the ring alone, for two properties: a saved
    // value of -1 absorbs the union, so a poll on which the engine has
    // disabled input stays disabled; and a saved value of 0 - something has
    // already swallowed this poll - still lets through the stale bits that
    // nobody has handled.
    uint32_t combined = saved;
    uint32_t head = __atomic_load_n(&input->head, __ATOMIC_ACQUIRE);
    for (uint32_t k = 1; k < divisor; k++) {
        combined |= __atomic_load_n(&input->ring[(head - k) % MAX_INPUT_UNION_RING],
                    __ATOMIC_ACQUIRE);
    }

    *mask = combined;
    int ret = TAI_CONTINUE(int, rate_hook->ref, a0, a1, a2, a3);
    if (*mask == combined)
        *mask = saved;

    return ret;
}

/**
 * Claims this thread's re-entry record for a slot, and returns how deep inside
 * the hooked function the current call is: 1 for a call that arrived from the
 * game, 2 or more for a call the hooked function made into itself.
 *
 * *claimed is the record to hand back to vg_hook_rate_leave(), or NULL when no
 * record was free - that means RATE_REENTRY_MAX other threads are inside this
 * one target at this instant, which a self-calling game function does not do
 * on its own, so such a call is reported at depth 1 and decided as the
 * outermost call it almost certainly is.
 */
static uint32_t vg_hook_rate_enter(vg_rate_hook_t *rate_hook, int32_t thread,
            vg_rate_entry_t **claimed) {
    // Already inside on this thread: one more level of recursion. Only the
    // owning thread writes 'depth', so it needs no atomic of its own.
    for (uint32_t i = 0; i < RATE_REENTRY_MAX; i++) {
        vg_rate_entry_t *entry = &rate_hook->entry[i];
        if (__atomic_load_n(&entry->thread, __ATOMIC_ACQUIRE) == thread) {
            *claimed = entry;
            return ++entry->depth;
        }
    }

    // Not inside on this thread: take a free record. The compare-exchange is
    // what keeps two threads from claiming the same one.
    for (uint32_t i = 0; i < RATE_REENTRY_MAX; i++) {
        vg_rate_entry_t *entry = &rate_hook->entry[i];
        int32_t unused = 0;
        if (__atomic_compare_exchange_n(&entry->thread, &unused, thread, false,
                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            entry->depth = 1;
            *claimed = entry;
            return 1;
        }
    }

    *claimed = NULL;
    return 1;
}

static void vg_hook_rate_leave(vg_rate_entry_t *entry) {
    if (entry == NULL)
        return;

    if (--entry->depth == 0)
        __atomic_store_n(&entry->thread, 0, __ATOMIC_RELEASE);
}

/**
 * Decides whether this outermost call is one of the 1 in <divisor> that are
 * made. Called once per outermost call, and it is the only thing that moves
 * the slot's counter.
 */
static bool vg_hook_rate_shall_run(vg_rate_hook_t *rate_hook, uint32_t divisor) {
    // Counted in both modes: in call counted mode this is the parity source,
    // and in frame counted mode it is what the slot falls back on when the
    // display stalls. Folds back at RATE_COUNT_MODULUS, which every accepted
    // divisor divides, so no interval is short at the wrap and a value read a
    // little above the modulus gives the same remainder as its folded self.
    uint32_t count = __atomic_add_fetch(&rate_hook->count, 1, __ATOMIC_ACQ_REL);
    if (count == RATE_COUNT_MODULUS)
        __atomic_fetch_sub(&rate_hook->count, RATE_COUNT_MODULUS, __ATOMIC_ACQ_REL);

    if (!rate_hook->frame_counted) {
        // Call counting, the default: this slot makes the first call of every
        // group of <divisor>. Nothing else can move the counter, so it cannot
        // drift against the function it is attached to and it cannot freeze.
        return (count - 1) % divisor == 0;
    }

    // Frame counting: every call made in one displayed frame takes the same
    // decision, which is what a function called several times per frame (on
    // behalf of several objects) needs to keep those objects in step.
    uint32_t frame = __atomic_load_n(&g_main.frame, __ATOMIC_ACQUIRE);
    if (frame != __atomic_load_n(&rate_hook->stall_frame, __ATOMIC_ACQUIRE)) {
        // The display has moved since the last decision: this is a new frame
        __atomic_store_n(&rate_hook->stall_frame, frame, __ATOMIC_RELEASE);
        __atomic_store_n(&rate_hook->stall_calls, 0, __ATOMIC_RELEASE);
        return frame % divisor == 0;
    }

    uint32_t stalled = __atomic_load_n(&rate_hook->stall_calls, __ATOMIC_ACQUIRE);
    if (stalled < RATE_STALL_CALLS) {
        // The ordinary case: a handful of calls within one displayed frame,
        // all taking the decision that frame took. The counter stops at
        // RATE_STALL_CALLS rather than running on, so it cannot wrap.
        __atomic_add_fetch(&rate_hook->stall_calls, 1, __ATOMIC_ACQ_REL);
        return frame % divisor == 0;
    }

    // Nothing has been presented for RATE_STALL_CALLS calls, so the frame
    // counter is not a clock any more. Fall back to this slot's own calls:
    // parked on a skipped frame that stops the hook skipping forever (which
    // would hang a game waiting on this function), and parked on a made frame
    // it stops the hook making every call (which would be no division at all).
    // Either way the requested rate is what the game gets while it lasts.
    return (count - 1) % divisor == 0;
}

/**
 * Body shared by every rate divided wrapper.
 *
 * taiHEN hands a hook body no context of its own, so each slot gets a distinct
 * static wrapper below which knows its own index and lands here.
 *
 * Re-entry: only the outermost call of a nest is counted and decided. A call
 * the hooked function made into itself is always made, because it only exists
 * at all when the outermost call was made - counting it would consume a tick
 * that belongs to no game step (which is what collapsed the effective divisor
 * to d-1, and to 1 at divisor 2), and skipping it would truncate the recursion
 * and hand every deeper level the fabricated return value.
 *
 * ABI: r0-r3 in, r0 out, and that is all the wrapper can carry. A target that
 * takes stack arguments, or returns a float, a struct or a 64 bit value, must
 * not be pointed at - a skipped call would hand the game back a fabricated r0,
 * leave r1 untouched and drop whatever the caller put on the stack. Nothing
 * here can tell such a target from any other: what the directive offers is the
 * 'args=<n>', 'ret64', 'retfloat' and 'retstruct' tokens, with which a patch
 * author who has worked the shape out declares it and gets a refusal instead
 * of a corrupted call. See vg_hook_parse_rate_divide().
 *
 * NOTE: a skipped call deliberately returns without TAI_CONTINUE, which
 * taihen.h calls out as the shape that breaks the chain for any other hook on
 * the same address. That is the whole point of the directive - the call must
 * not happen - and it is only ever pointed at a game internal offset, which no
 * other plugin has reason to hook. Nothing else in VitaGrafix hooks by offset.
 */
static inline int vg_hook_rate_divide_call(uint32_t slot, vg_hook_arg_t a0, vg_hook_arg_t a1,
            vg_hook_arg_t a2, vg_hook_arg_t a3) {
    vg_rate_hook_t *rate_hook = &g_main.rate_hook[slot];

    // module_stop clears this before releasing the hook: a call that is
    // already on its way in must not follow a chain that is being torn down
    if (!__atomic_load_n(&rate_hook->armed, __ATOMIC_ACQUIRE))
        return (int)rate_hook->ret_value;

    uint32_t divisor = rate_hook->divisor;
    if (divisor <= 1)
        return TAI_CONTINUE(int, rate_hook->ref, a0, a1, a2, a3);

    // 0 marks a free re-entry record, so it cannot also mean a thread
    int32_t thread = sceKernelGetThreadId();
    if (thread == 0)
        thread = -1;

    vg_rate_entry_t *entry = NULL;
    uint32_t depth = vg_hook_rate_enter(rate_hook, thread, &entry);

    int ret;
    if (depth > 1) {
        // A call the hooked function made into itself is already inside the
        // outermost call's substitution interval, so the union it should see
        // is in the word already and there is nothing to do here
        ret = TAI_CONTINUE(int, rate_hook->ref, a0, a1, a2, a3);
    } else if (vg_hook_rate_shall_run(rate_hook, divisor)) {
        ret = rate_hook->union_input
                    ? vg_hook_rate_union_call(rate_hook, divisor, a0, a1, a2, a3)
                    : TAI_CONTINUE(int, rate_hook->ref, a0, a1, a2, a3);
    } else {
        // A skipped call touches nothing at all: the mask keeps its true one
        // poll value for whatever else reads it this frame
        ret = (int)rate_hook->ret_value;
    }

    vg_hook_rate_leave(entry);
    return ret;
}

#define VG_RATE_WRAPPER(n) \
    static int vg_hook_rate_divide_##n(vg_hook_arg_t a0, vg_hook_arg_t a1, \
                vg_hook_arg_t a2, vg_hook_arg_t a3) { \
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
 * Refuses a target address that is not an instruction boundary inside the
 * module. A hook writes a branch into code, so a stale or mistyped offset does
 * not fail cleanly the way a mistyped patch line does - it corrupts whatever
 * it lands on. taiHEN only checks the segment index.
 */
static vg_io_status_t vg_hook_check_rate_target(const vg_hook_request_t *request) {
    // Thumb instructions are 2 byte aligned, ARM ones 4 byte aligned; anything
    // else is a mid instruction offset
    if (request->offset & (request->thumb ? 1 : 3)) {
        vg_log_printf("[HOOK] seg%03d : %08X is not a %s instruction boundary\n",
                    request->segment, request->offset, request->thumb ? "thumb" : "arm");
        __ret_status(IO_ERROR_HOOK_BAD_TARGET, 0, 0);
    }

    const SceKernelSegmentInfo *segments = g_main.sce_info.segments;
    uint32_t segment_num = sizeof(g_main.sce_info.segments) / sizeof(g_main.sce_info.segments[0]);

    // No module info at all: sceKernelGetModuleInfo() failed, or a host tool
    // was not told the segment sizes. This check is the only thing between a
    // stale offset and a branch written over whatever it lands on, so it is
    // refused rather than waved through - a hook installed without it is
    // exactly the case the check exists for, and a log line nobody reads is
    // not a substitute. src/main.c logs the failed call for the same reason.
    bool known = false;
    for (uint32_t i = 0; i < segment_num; i++) {
        known = known || segments[i].memsz > 0;
    }
    if (!known) {
        vg_log_printf("[HOOK] no module segment info, refusing to hook seg%03d : %08X unchecked\n",
                    request->segment, request->offset);
        __ret_status(IO_ERROR_HOOK_NO_MODULE_INFO, 0, 0);
    }

    if (request->segment >= segment_num || segments[request->segment].memsz == 0) {
        vg_log_printf("[HOOK] seg%03d does not exist in this module\n", request->segment);
        __ret_status(IO_ERROR_HOOK_BAD_TARGET, 0, 0);
    }
    // A hook target is code. A segment without the execute bit holds data, so
    // an offset into one is a misread address rather than a function entry.
    if (!(segments[request->segment].perms & RATE_SEGMENT_PERM_EXEC)) {
        vg_log_printf("[HOOK] seg%03d is not executable (perms 0x%X), nothing there is a function\n",
                    request->segment, segments[request->segment].perms);
        __ret_status(IO_ERROR_HOOK_BAD_TARGET, 0, 0);
    }
    if (request->offset > segments[request->segment].memsz
            || segments[request->segment].memsz - request->offset < RATE_TARGET_MIN_BYTES) {
        vg_log_printf("[HOOK] seg%03d : %08X is past the end of the segment (size %u)\n",
                    request->segment, request->offset, segments[request->segment].memsz);
        __ret_status(IO_ERROR_HOOK_BAD_TARGET, 0, 0);
    }

    __ret_status(IO_OK, 0, 0);
}

/**
 * Records what a '>inputUnion()' directive declared about the game's input
 * sampler. Nothing is hooked here: the sampler hook is installed by the first
 * 'union' rate divided slot that needs it, so a patch file that declares the
 * sampler and then resolves every slot to divisor 1 - the game running at its
 * own rate - hooks nothing at all.
 */
static vg_io_status_t vg_hook_record_input_union(const vg_hook_request_t *request) {
    vg_input_hook_t *input = &g_main.input;
    vg_io_status_t ret;

    // The line's own validity first, so that a sampler offset which is not an
    // instruction inside the module says that, rather than being reported as a
    // disagreement with the line that got there first
    ret = vg_hook_check_rate_target(request);
    if (ret.code != IO_OK)
        return ret;

    if (input->requested) {
        // Two lines about one sampler that disagree are a patch bug: there is
        // one pad structure and one mask field per title, so there is no
        // reading under which both are right
        if (input->segment != request->segment || input->offset != request->offset
                || input->thumb != request->thumb || input->mask_offset != request->mask_offset) {
            vg_log_printf("[HOOK] The input sampler is already declared as seg%03d : %08X"
                        " (%s) mask pad+0x%X, not seg%03d : %08X (%s) mask pad+0x%X\n",
                        input->segment, input->offset, input->thumb ? "thumb" : "arm",
                        input->mask_offset, request->segment, request->offset,
                        request->thumb ? "thumb" : "arm", request->mask_offset);
            __ret_status(IO_ERROR_HOOK_CONFLICT, 0, 0);
        }

        vg_log_printf("[HOOK] The input sampler is already declared, skipping\n");
        __ret_status(IO_OK, 0, 0);
    }

    input->segment = request->segment;
    input->offset = request->offset;
    input->thumb = request->thumb;
    input->mask_offset = request->mask_offset;
    input->requested = true;

    vg_log_printf("[HOOK] Input sampler is seg%03d : %08X (%s), pulse mask at pad+0x%X\n",
                request->segment, request->offset, request->thumb ? "thumb" : "arm",
                request->mask_offset);
    __ret_status(IO_OK, 0, 0);
}

/**
 * Installs the sampler hook, for the first 'union' rate divided slot that asks
 * for it. Repeats are a no-op: one sampler, one accumulator.
 */
static vg_io_status_t vg_hook_install_input_union(void) {
    vg_input_hook_t *input = &g_main.input;

    if (input->uid >= 0)
        __ret_status(IO_OK, 0, 0);

    // Configure before arming, as the rate divided slots do: the hook body can
    // be entered as soon as taiHookFunctionOffset() returns, and until 'armed'
    // is set it reads nothing and publishes nothing.
    input->mask_addr = NULL;
    input->head = 0;
    for (uint32_t i = 0; i < MAX_INPUT_UNION_RING; i++) {
        input->ring[i] = 0;
    }

    vg_log_printf("[HOOK] Hooking input sampler seg%03d : %08X (%s) to 0x%X,"
                " accumulating pad+0x%X\n",
                input->segment, input->offset, input->thumb ? "thumb" : "arm",
                &vg_hook_input_union_sampler, input->mask_offset);

    input->uid = taiHookFunctionOffset(&input->ref, g_main.tai_info.modid,
                input->segment, input->offset, input->thumb ? 1 : 0,
                &vg_hook_input_union_sampler);
    if (input->uid < 0) {
        __ret_status(IO_ERROR_TAI_GENERIC, 0, 0);
    }

    __atomic_store_n(&input->armed, true, __ATOMIC_RELEASE);
    __ret_status(IO_OK, 0, 0);
}

/**
 * Installs the game function hook that a '>rateDivide()' directive asked for.
 */
static vg_io_status_t vg_hook_function_offset_rate_divide(const vg_hook_request_t *request) {
    vg_io_status_t ret;

    // Hooking one site twice would overwrite the first hook's uid/ref, and its
    // TAI_CONTINUE would then re-enter the wrapper forever (see above)
    for (uint32_t i = 0; i < g_main.rate_hook_num; i++) {
        if (g_main.rate_hook[i].segment != request->segment
                || g_main.rate_hook[i].offset != request->offset)
            continue;

        // Two lines about one address that disagree about how it is to be
        // called are a patch bug: there is no basis for picking one of them.
        // Every argument the directive carries is compared, not just the ones
        // that change what the wrapper does - 'thumb' decides how taiHEN
        // decodes the branch it writes, and 'args' is the author's statement
        // about the target, which two lines cannot sensibly disagree about.
        if (g_main.rate_hook[i].divisor != request->divisor
                || g_main.rate_hook[i].ret_value != request->ret_value
                || g_main.rate_hook[i].frame_counted != request->frame_counted
                || g_main.rate_hook[i].thumb != request->thumb
                || g_main.rate_hook[i].union_input != request->union_input
                || g_main.rate_hook[i].arg_num != request->arg_num) {
            vg_log_printf("[HOOK] seg%03d : %08X is already rate divided with other arguments"
                        " (divisor %u/%u, return 0x%X/0x%X, %s/%s, %s/%s, args %u/%u,"
                        " %s/%s)\n",
                        request->segment, request->offset,
                        g_main.rate_hook[i].divisor, request->divisor,
                        g_main.rate_hook[i].ret_value, request->ret_value,
                        g_main.rate_hook[i].frame_counted ? "frame" : "call",
                        request->frame_counted ? "frame" : "call",
                        g_main.rate_hook[i].thumb ? "thumb" : "arm",
                        request->thumb ? "thumb" : "arm",
                        g_main.rate_hook[i].arg_num, request->arg_num,
                        g_main.rate_hook[i].union_input ? "union" : "no union",
                        request->union_input ? "union" : "no union");
            __ret_status(IO_ERROR_HOOK_CONFLICT, 0, 0);
        }

        vg_log_printf("[HOOK] seg%03d : %08X is already rate divided, skipping\n",
                    request->segment, request->offset);
        __ret_status(IO_OK, 0, 0);
    }

    if (g_main.rate_hook_num >= MAX_RATE_HOOK_NUM) {
        vg_log_printf("[HOOK] Too many rate divided hooks, limit: " TOSTRING(MAX_RATE_HOOK_NUM) "\n");
        __ret_status(IO_ERROR_TOO_MANY_HOOKS, 0, 0);
    }

    ret = vg_hook_check_rate_target(request);
    if (ret.code != IO_OK)
        return ret;

    // A 'union' slot needs the sampler named and hooked before it can take a
    // call, and every 'union' slot has to divide the same way: they decide on
    // the shared frame counter, so two different divisors mean two different
    // sets of live polls reading one accumulator, and the minigame halves the
    // 'frame' token exists to keep in step would not be in step at all.
    if (request->union_input) {
        if (!g_main.input.requested) {
            vg_log_printf("[HOOK] seg%03d : %08X asked for 'union' with no '>inputUnion()'"
                        " line before it\n", request->segment, request->offset);
            __ret_status(IO_ERROR_HOOK_UNION_NO_SAMPLER, 0, 0);
        }
        if (g_main.input.divisor != 0 && g_main.input.divisor != request->divisor) {
            vg_log_printf("[HOOK] seg%03d : %08X divides by %u, but the accumulated input is"
                        " already shared at a divisor of %u\n", request->segment,
                        request->offset, request->divisor, g_main.input.divisor);
            __ret_status(IO_ERROR_HOOK_UNION_DIVISOR_CONFLICT, 0, 0);
        }

        ret = vg_hook_install_input_union();
        if (ret.code != IO_OK)
            return ret;
    }

    // A frame counted slot needs the displayed frame counter running before it
    // takes its first decision; a call counted one needs nothing at all
    if (request->frame_counted && g_main.hook[HOOK_RATE_FRAME_COUNTER] < 0) {
        ret = vg_hook_function_import(HOOK_RATE_FRAME_COUNTER, 0x7A410B64,
                    &vg_hook_sceDisplaySetFrameBuf_rateCounter);
        if (ret.code != IO_OK)
            return ret;
    }

    uint32_t slot = g_main.rate_hook_num;
    vg_rate_hook_t *rate_hook = &g_main.rate_hook[slot];

    // Configure the slot before arming the hook, never after: the wrapper can
    // be entered as soon as taiHookFunctionOffset() returns. 'armed' is set
    // last, so a call that arrives before the ref is written is skipped
    // (returning the patch's substitute) instead of following an empty chain.
    rate_hook->segment = request->segment;
    rate_hook->offset = request->offset;
    rate_hook->thumb = request->thumb;
    rate_hook->arg_num = request->arg_num;
    rate_hook->divisor = request->divisor;
    rate_hook->frame_counted = request->frame_counted;
    rate_hook->ret_value = request->ret_value;
    rate_hook->union_input = request->union_input;
    rate_hook->count = 0;
    rate_hook->stall_frame = __atomic_load_n(&g_main.frame, __ATOMIC_ACQUIRE);
    rate_hook->stall_calls = 0;
    for (uint32_t i = 0; i < RATE_REENTRY_MAX; i++) {
        rate_hook->entry[i].thread = 0;
        rate_hook->entry[i].depth = 0;
    }

    vg_log_printf("[HOOK] Hooking seg%03d : %08X (%s) to 0x%X, 1 call in %u %s,"
                " skipped call returns 0x%X%s\n",
                request->segment, request->offset, request->thumb ? "thumb" : "arm",
                _RATE_WRAPPERS[slot], request->divisor,
                request->frame_counted ? "displayed frames" : "calls", request->ret_value,
                request->union_input ? ", sees the polls it skipped" : "");

    rate_hook->uid = taiHookFunctionOffset(&rate_hook->ref, g_main.tai_info.modid,
                request->segment, request->offset, request->thumb ? 1 : 0, _RATE_WRAPPERS[slot]);
    if (rate_hook->uid < 0) {
        rate_hook->divisor = 0;
        __ret_status(IO_ERROR_TAI_GENERIC, 0, 0);
    }

    __atomic_store_n(&rate_hook->armed, true, __ATOMIC_RELEASE);
    // Only now that a slot really shares the accumulator does its divisor
    // become the one every other 'union' slot is held to
    if (request->union_input)
        g_main.input.divisor = request->divisor;
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
 * Parses
 *   '>rateDivide(<seg>:<offset>, <divisor>, void|ret=<value>
 *                [, thumb|arm][, call|frame][, args=<n>][, union])'
 *
 * The game function at <seg>:<offset> is then called 1 time in <divisor>
 * instead of every time, which keeps logic that is written as one step per
 * rendered frame at its original speed while the display runs unlocked.
 * <divisor> is an interpreter expression, so it can be written against the
 * configured frame rate ('1 + (fps_limit / 60)' for a game whose own rate is
 * 30) and collapses to 1 - no hook at all - at the game's native rate. It must
 * evaluate to at least 1 at every frame rate setting, which is why that form
 * is preferred over 'fps_limit / 30': that one is 0 at FPS=20.
 *
 * What is counted:
 *  - 'call' (the default) counts this hook's own invocations. Nothing else can
 *    move the counter, so it cannot drift against the function it is attached
 *    to and it cannot freeze while the game is not presenting.
 *  - 'frame' counts displayed frames instead, so that every call made in one
 *    frame is made or skipped together. That is only what a target called
 *    several times per frame on behalf of several objects needs, to keep those
 *    objects in step with each other; it costs an import hook on
 *    sceDisplaySetFrameBuf, it is decided on whichever thread presents, and it
 *    stops being a rate at all while nothing is presented (see the stall
 *    handling in vg_hook_rate_divide_call).
 *
 * The skipped call never reaches the game, so its return value is made up:
 * 'void' for a function whose result the game discards (0 is returned), or
 * 'ret=<value>' for one whose result is read.
 *
 * 'union' folds the pulses of the polls this slot skipped into the game's
 * input mask, for the duration of each call the slot makes. Without it a
 * target that reads a press/repeat mask rebuilt once per rendered frame simply
 * never sees the presses that landed on a skipped poll. It needs an
 * '>inputUnion()' line before it to say where the sampler and the mask are,
 * it needs 'frame' counting so that every slot sharing the accumulator agrees
 * on which polls are live, and it refuses a divisor above
 * INPUT_UNION_DIVISOR_MAX - past that a window is wider than the sampler's
 * auto-repeat step and the OR would swallow repeat ticks.
 *
 * 'args=<n>', 'ret64', 'retfloat' and 'retstruct' are DECLARATIONS BY THE
 * PATCH AUTHOR, not checks on the target. Nothing here reads the target: an
 * ARM prologue does not say how many arguments a function takes or what it
 * returns, so there is nothing to derive. A line that omits 'args=' is not
 * checked against anything. What these tokens buy is that an author who has
 * worked out that the target takes five arguments, or returns a float, can
 * write it down and have the line refused at parse time instead of shipping a
 * hook that fabricates r0 and drops the rest. The real guard against pointing
 * the directive at such a target is the author reading it first.
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
    // 'one call in 0' is not a rate. It is what 'fps_limit / 30' evaluates to
    // at FPS=20, so it is a plausible thing for a patch to write by accident,
    // and it has no reading that is safe to guess at - refuse it and let the
    // author write a divisor that is defined at every frame rate setting.
    if (request->divisor < 1 || request->divisor > RATE_DIVISOR_MAX) {
        vg_log_printf("[HOOK] Rate divisor %u is out of range (1.." TOSTRING(RATE_DIVISOR_MAX) ")\n",
                    request->divisor);
        __ret_status(IO_ERROR_HOOK_BAD_DIVISOR, 0, pos_divisor);
    }

    // Return handling. A skipped call fabricates whatever the game reads back,
    // so the patch has to say what that is - there is no safe default
    while (isspace(line[pos])) { pos++; }
    if (line[pos] != ',')
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
    pos++;

    while (isspace(line[pos])) { pos++; }
    // A return the wrapper cannot fabricate in r0 alone. The target is not
    // inspected - naming it is how a patch author who knows says so and gets a
    // refusal instead of a corrupted call.
    if (!strncasecmp(&line[pos], "ret64", 5)
            || !strncasecmp(&line[pos], "retfloat", 8)
            || !strncasecmp(&line[pos], "retstruct", 9)) {
        vg_log_printf("[HOOK] Only an int return can be substituted for a skipped call\n");
        __ret_status(IO_ERROR_HOOK_UNSUPPORTED_ABI, 0, pos);
    }
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

    // Optional trailing tokens, in any order and at most one of each kind:
    // the instruction set (Thumb unless the patch says otherwise), what is
    // counted (the slot's own calls unless the patch says otherwise), and the
    // number of arguments the patch declares the target takes. The default of
    // RATE_ARG_MAX is 'the author did not say', not 'checked and found to be
    // four'.
    request->thumb = true;
    request->frame_counted = false;
    request->arg_num = RATE_ARG_MAX;
    request->union_input = false;
    bool isa_given = false, counted_given = false, args_given = false, union_given = false;
    int pos_union = pos;

    while (true) {
        while (isspace(line[pos])) { pos++; }
        if (line[pos] != ',')
            break;
        pos++;
        while (isspace(line[pos])) { pos++; }

        int pos_token = pos;
        if (!strncasecmp(&line[pos], "thumb", 5)) {
            request->thumb = true;
            pos += 5;
            if (isa_given)
                __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos_token);
            isa_given = true;
        } else if (!strncasecmp(&line[pos], "arm", 3)) {
            request->thumb = false;
            pos += 3;
            if (isa_given)
                __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos_token);
            isa_given = true;
        } else if (!strncasecmp(&line[pos], "call", 4)) {
            request->frame_counted = false;
            pos += 4;
            if (counted_given)
                __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos_token);
            counted_given = true;
        } else if (!strncasecmp(&line[pos], "frame", 5)) {
            request->frame_counted = true;
            pos += 5;
            if (counted_given)
                __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos_token);
            counted_given = true;
        } else if (!strncasecmp(&line[pos], "union", 5)) {
            request->union_input = true;
            pos += 5;
            if (union_given)
                __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos_token);
            union_given = true;
            pos_union = pos_token;
        } else if (!strncasecmp(&line[pos], "args", 4)) {
            pos += 4;
            while (isspace(line[pos])) { pos++; }
            if (line[pos] != '=')
                __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
            pos++;

            int pos_args = pos;
            ret = vg_hook_parse_argument(line, &pos, &request->arg_num);
            if (ret.code != IO_OK)
                return ret;
            if (args_given)
                __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos_token);
            args_given = true;

            // Everything past r3 is on the stack, and the wrapper carries
            // registers only - a skipped call would drop those arguments and
            // a made one is at the mercy of how the wrapper was compiled. This
            // refuses what the patch declared; it does not discover it.
            if (request->arg_num > RATE_ARG_MAX) {
                vg_log_printf("[HOOK] A target taking %u arguments cannot be rate divided,"
                            " at most " TOSTRING(RATE_ARG_MAX) " fit in registers\n", request->arg_num);
                __ret_status(IO_ERROR_HOOK_UNSUPPORTED_ABI, 0, pos_args);
            }
        } else {
            __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
        }
    }

    if (line[pos] != ')')
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
    pos++;
    if (!vg_io_is_line_end(line, pos))
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);

    // What 'union' needs that the line itself can answer. A divisor of 1 is a
    // line that installs nothing at this configuration, so it is not held to
    // the accumulator's range - it never reaches an accumulator.
    if (request->union_input) {
        if (!request->frame_counted) {
            vg_log_printf("[HOOK] Accumulated input needs 'frame' counting: call counted slots"
                        " sharing one accumulator do not agree on which polls are live\n");
            __ret_status(IO_ERROR_HOOK_UNION_NEEDS_FRAME, 0, pos_union);
        }
        if (request->divisor > INPUT_UNION_DIVISOR_MAX) {
            vg_log_printf("[HOOK] A divisor of %u cannot accumulate input: a window of %u polls"
                        " is wider than the sampler's auto-repeat step, so two repeat ticks of"
                        " one button fall in it and the union collapses them into one\n",
                        request->divisor, request->divisor);
            __ret_status(IO_ERROR_HOOK_UNION_BAD_DIVISOR, 0, pos_union);
        }
    }

    request->kind = HOOK_KIND_RATE_DIVIDE;
    __ret_status(IO_OK, 0, 0);
}

/**
 * Parses '>inputUnion(<seg>:<sampler_offset>, <mask_offset>[, thumb|arm])'.
 *
 * Names the game's input sampler and says where in the pad structure the
 * press/repeat pulse mask sits. The sampler takes that structure as its first
 * argument, so the hook reaches the field from its own r0 and no per title
 * global address is needed - only the offset.
 *
 * On its own the line hooks nothing and changes nothing. It is what a
 * '>rateDivide(..., union)' slot reads to find the mask, and the sampler hook
 * is installed with the first such slot.
 */
static vg_io_status_t vg_hook_parse_input_union(const char line[], int pos,
            vg_hook_request_t *request) {
    vg_io_status_t ret;

    while (isspace(line[pos])) { pos++; }
    if (line[pos] != '(')
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
    pos++;

    // The sampler
    while (isspace(line[pos])) { pos++; }
    ret = vg_io_parse_address(line, &pos, &request->segment, &request->offset);
    if (ret.code != IO_OK)
        return ret;

    while (isspace(line[pos])) { pos++; }
    if (line[pos] != ',')
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
    pos++;

    // Where the mask sits inside the pad
    int pos_mask = pos;
    while (isspace(line[pos_mask])) { pos_mask++; }
    ret = vg_hook_parse_argument(line, &pos, &request->mask_offset);
    if (ret.code != IO_OK)
        return ret;
    // The hook dereferences pad + this on every poll. A misread offset is not
    // something the plugin can recognise, but one that is not a word boundary
    // or is far past the end of any pad structure is not a field at all.
    if ((request->mask_offset & 3) != 0 || request->mask_offset > INPUT_MASK_OFFSET_MAX) {
        vg_log_printf("[HOOK] pad+0x%X is not a word inside the pad structure (0..0x%X,"
                    " word aligned)\n", request->mask_offset, INPUT_MASK_OFFSET_MAX);
        __ret_status(IO_ERROR_HOOK_BAD_MASK_OFFSET, 0, pos_mask);
    }

    // Instruction set of the sampler, Thumb unless the line says otherwise
    request->thumb = true;
    bool isa_given = false;

    while (true) {
        while (isspace(line[pos])) { pos++; }
        if (line[pos] != ',')
            break;
        pos++;
        while (isspace(line[pos])) { pos++; }

        int pos_token = pos;
        if (!strncasecmp(&line[pos], "thumb", 5)) {
            request->thumb = true;
            pos += 5;
        } else if (!strncasecmp(&line[pos], "arm", 3)) {
            request->thumb = false;
            pos += 3;
        } else {
            __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
        }

        if (isa_given)
            __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos_token);
        isa_given = true;
    }

    if (line[pos] != ')')
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
    pos++;
    if (!vg_io_is_line_end(line, pos))
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);

    request->kind = HOOK_KIND_INPUT_UNION;
    __ret_status(IO_OK, 0, 0);
}

/**
 * Parses one '>' directive into a request, and decides from the configuration
 * whether it is to be installed. Nothing is hooked here - vg_hook_parse_patch()
 * applies the result, and the host side patch file checker reports it.
 */
vg_io_status_t vg_hook_parse_request(const char line[], vg_feature_t feature,
            vg_hook_request_t *request, uint8_t *shall_hook) {
    vg_io_status_t ret = {IO_OK, 0, 0};
    const vg_config_t *config = vg_config_get();

    // Every '>' directive hooks something that belongs to the frame rate
    // option, and only the block a line sits under decides whether it is
    // parsed at all. Under @FB, @IB or @MSAA a directive would parse, report
    // IO_OK and then install nothing, for the life of the patch file - so say
    // that it is in the wrong block instead of silently doing nothing.
    if (feature != FEATURE_FPS) {
        vg_log_printf("[HOOK] Hook directives belong under @FPS, not here: %s\n", line);
        __ret_status(IO_ERROR_HOOK_WRONG_FEATURE, 0, 0);
    }

    if (!strncasecmp(&line[1], "sceDisplaySetFrameBuf_withWait", 30)) {
        request->kind = HOOK_KIND_IMPORT;
        request->hook_id = HOOK_DISPLAY_SET_FRAMEBUF_WITH_WAIT;
        request->name = "sceDisplaySetFrameBuf_withWait";
        request->import_nid = 0x7A410B64;
        request->hook_ptr = &vg_hook_sceDisplaySetFrameBuf_withWait;
        *shall_hook = config->fps_enabled == FT_ENABLED && config->fps == FPS_30;
        return ret;
    }
    if (!strncasecmp(&line[1], "sceCtrlReadBufferPositive_peekPatched", 37)) {
        request->kind = HOOK_KIND_IMPORT;
        request->hook_id = HOOK_CTRL_READ_BUFFER_POSITIVE;
        request->name = "sceCtrlReadBufferPositive_peekPatched";
        request->import_nid = 0x67E7AB83;
        request->hook_ptr = &vg_hook_sceCtrlReadBufferPositive_peekPatched;
        *shall_hook = config->fps_enabled == FT_ENABLED && config->fps == FPS_60;
        return ret;
    }
    if (!strncasecmp(&line[1], "sceCtrlReadBufferPositive2_peekPatched", 38)) {
        request->kind = HOOK_KIND_IMPORT;
        request->hook_id = HOOK_CTRL_READ_BUFFER_POSITIVE2;
        request->name = "sceCtrlReadBufferPositive2_peekPatched";
        request->import_nid = 0xC4226A3E;
        request->hook_ptr = &vg_hook_sceCtrlReadBufferPositive2_peekPatched;
        *shall_hook = config->fps_enabled == FT_ENABLED && config->fps == FPS_60;
        return ret;
    }
    if (!strncasecmp(&line[1], "inputUnion", 10)) {
        // 1 for the leading '>', 10 for the directive name
        request->name = "inputUnion";
        ret = vg_hook_parse_input_union(line, 1 + 10, request);
        if (ret.code != IO_OK)
            return ret;

        // Inert unless the frame rate option is on. Whether anything is hooked
        // at all is decided by the 'union' slots that read this: at the game's
        // own rate they resolve to divisor 1, install nothing, and never ask
        // for the sampler hook.
        *shall_hook = config->fps_enabled == FT_ENABLED;
        if (!*shall_hook) {
            vg_log_printf("[HOOK] Not declaring the input sampler seg%03d : %08X"
                        " (fps_enabled=%d)\n", request->segment, request->offset,
                        config->fps_enabled);
        }
        return ret;
    }
    if (!strncasecmp(&line[1], "rateDivide", 10)) {
        // 1 for the leading '>', 10 for the directive name
        request->name = "rateDivide";
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
 * Installs what a parsed directive asked for, if the configuration asked for it
 * to be installed at all.
 */
vg_io_status_t vg_hook_apply_request(const vg_hook_request_t *request, uint8_t shall_hook) {
    if (shall_hook) {
        if (request->kind == HOOK_KIND_RATE_DIVIDE)
            return vg_hook_function_offset_rate_divide(request);
        if (request->kind == HOOK_KIND_INPUT_UNION)
            return vg_hook_record_input_union(request);

        return vg_hook_function_import(request->hook_id, request->import_nid, request->hook_ptr);
    }

    __ret_status(IO_OK, 0, 0);
}

/**
 * Parses and applies a common hook
 */
vg_io_status_t vg_hook_parse_patch(const char line[], vg_feature_t feature) {
    vg_hook_request_t request = {0};
    uint8_t shall_hook = 0;
    vg_io_status_t ret = {IO_OK, 0, 0};

    // Check for common hook
    ret = vg_hook_parse_request(line, feature, &request, &shall_hook);
    if (ret.code != IO_OK)
        return ret;

    // Apply
    return vg_hook_apply_request(&request, shall_hook);
}
