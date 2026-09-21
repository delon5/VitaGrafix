#ifndef _MAIN_H_
#define _MAIN_H_

#define SECOND             1000000
#define OSD_SHOW_DURATION  5 * SECOND

#define OSD_MSG_CONFIG_OPEN_FAILED   "Failed to open config file."
#define OSD_MSG_CONFIG_ERROR         "An error occured while reading config file."
#define OSD_MSG_PATCH_OPEN_FAILED    "Failed to open patch file."
#define OSD_MSG_PATCH_ERROR          "An error occured while reading patch file."
#define OSD_MSG_IOPLUS_HINT          "Do you have ioPlus installed?"
#define OSD_MSG_GAME_WRONG_VERSION   "Your game version is not supported :("

#define VG_VERSION         "v6.0.0-dev"
#define VG_DIR             "ux0:data/VitaGrafix/"

#define STRING_BUFFER_SIZE 1024

#define MAX_INJECT_NUM 1024
#define MAX_HOOK_NUM   4

// Function hooks installed by '>rateDivide()' directives. One slot (and one
// static wrapper, see patch_hook.c) per directive instance.
#define MAX_RATE_HOOK_NUM 24

// A rate divided call runs 1 time in N. Anything above this is a patch typo
// rather than a frame rate ratio, and would stall the game function forever.
#define RATE_DIVISOR_MAX 16

// Every counter a rate divided hook reads is folded back here instead of
// wrapping at 2^32: this is lcm(1..RATE_DIVISOR_MAX), so every supported
// divisor divides it and folding back never changes a remainder. Without it
// the one interval across the wrap is short for any divisor that is not a
// power of two.
#define RATE_COUNT_MODULUS 720720u

// Frame counted hooks only. A displayed frame counter stops moving whenever
// the game stops presenting - a loading screen, a blocking wait, or the step
// wait loops this feature exists to slow down - and a frozen counter is then
// not a rate at all: parked on a skipped frame the hook would skip every call
// forever (hanging a game that is waiting for that function to make progress),
// and parked on a made frame it would make every call, which is no division at
// all. After this many consecutive calls with the frame counter unmoved the
// slot falls back to counting its own calls until the display moves again, so
// both parities keep the requested rate. It is far above any plausible number
// of calls a target takes within one displayed frame.
#define RATE_STALL_CALLS 256

// Threads that can be inside one rate divided hook's target at the same time
// and still have their nesting tracked separately. A slot needs one record per
// such thread to tell a call the game made from a call the hooked function
// made into itself; a call that finds no free record is treated as an
// outermost call, which is what it almost certainly is.
#define RATE_REENTRY_MAX 4

#define TITLEID_ANY  "XXXXxxxxx"

int isspace(int c);
int isdigit(int c);
int isalpha(int c);
int tolower(int c);

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

typedef enum {
    MODULE_TITLE_MISMATCH,
    MODULE_SELF_MISMATCH,
    MODULE_NID_MISMATCH,
    MODULE_MATCH
} vg_module_match_t;

// One thread's presence inside a rate divided hook's target. The wrapper needs
// this to tell a call that came from the game (which the divisor decides) from
// a call the hooked function made into itself (which must always be made, or
// the recursion is truncated and every deeper level is handed a fabricated
// return value).
typedef struct {
    // Thread currently inside the target through this record, 0 when free.
    // Claimed and released with an atomic compare-exchange
    volatile int32_t thread;

    // How deep that thread is inside the target: 1 for the outermost call.
    // Written only by the thread that owns the record
    volatile uint32_t depth;
} vg_rate_entry_t;

// A game function that is called only 1 time in 'divisor'
typedef struct {
    SceUID uid;
    tai_hook_ref_t ref;

    // Cleared before the hook is released, so a call that is still on its way
    // in takes the skipped path instead of following a released chain
    volatile bool armed;

    uint8_t segment;
    uint32_t offset;

    // Instruction set of the target, as the directive declared it: taiHEN
    // decodes the branch it writes the wrong way if this is wrong, so two
    // directives about one address that disagree about it are a patch bug
    bool thumb;

    // Arguments the directive declared the target takes. The wrapper carries
    // r0-r3 whatever this says - it is the patch author's declaration, checked
    // against what the wrapper can carry at parse time and kept here only so
    // that two directives about one address that disagree are refused
    uint32_t arg_num;

    // 0 = slot unused, 1 = every call (never installed), n = 1 call in n
    uint32_t divisor;

    // false: the slot counts its own calls (the default, and what the per
    // title studies ask for). true: the decision is taken from the displayed
    // frame counter below, so that every call made in one frame agrees.
    bool frame_counted;

    // Outermost calls this slot has taken, the parity source in call counted
    // mode and the fallback when a frame counted slot's display has stalled
    volatile uint32_t count;

    // Frame counted mode only: the displayed frame the last decision was taken
    // in, and how many consecutive calls have been taken with that frame
    // unmoved. Past RATE_STALL_CALLS the display counts as stalled and the
    // slot decides on 'count' instead, so neither parity of a frozen display
    // costs the game its rate or its progress
    volatile uint32_t stall_frame;
    volatile uint32_t stall_calls;

    // Threads currently inside the target, and how deep
    vg_rate_entry_t entry[RATE_REENTRY_MAX];

    // value handed back to the game for a call that was not made
    uint32_t ret_value;
} vg_rate_hook_t;

typedef struct {
    // OSD hook
    bool osd_done;
    SceUID osd_hook;
    tai_hook_ref_t osd_hook_ref;
    SceUInt32 osd_timer;

    // title info
    char titleid[16];
    tai_module_info_t tai_info;
    SceKernelModuleInfo sce_info;

    vg_module_match_t patch_match;

    // eboot patches
    uint32_t inject_num;
    SceUID inject[MAX_INJECT_NUM];

    // eboot hooks
    SceUID hook[MAX_HOOK_NUM];
    tai_hook_ref_t hook_ref[MAX_HOOK_NUM];

    // eboot rate divided function hooks
    uint32_t rate_hook_num;
    vg_rate_hook_t rate_hook[MAX_RATE_HOOK_NUM];

    // Displayed frame counter, the parity source for the rate divided hooks
    // that asked for 'frame' counting. Folded back at RATE_COUNT_MODULUS, so
    // the parity of every supported divisor survives the wrap.
    volatile uint32_t frame;

} vg_main_t;

extern vg_main_t g_main;

const char *vg_main_get_self_filename();
vg_module_match_t vg_main_match_current_module(const char titleid[], const char self[], uint32_t nid, bool require_exact_module);

#endif
