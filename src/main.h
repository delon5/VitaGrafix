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
// wait loops this feature exists to slow down - and a hook parked on a skipped
// frame would then skip every call forever, which hangs a game that is waiting
// for that function to make progress. After this many consecutive skips the
// next call is made and the count starts again, so a stall costs speed rather
// than the game. It is far above any plausible number of calls in one frame.
#define RATE_STALL_SKIPS 256

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

// A game function that is called only 1 time in 'divisor'
typedef struct {
    SceUID uid;
    tai_hook_ref_t ref;

    // Cleared before the hook is released, so a call that is still on its way
    // in takes the skipped path instead of following a released chain
    volatile bool armed;

    uint8_t segment;
    uint32_t offset;

    // 0 = slot unused, 1 = every call (never installed), n = 1 call in n
    uint32_t divisor;

    // false: the slot counts its own calls (the default, and what the per
    // title studies ask for). true: the decision is taken from the displayed
    // frame counter below, so that every call made in one frame agrees.
    bool frame_counted;

    // Calls this slot has taken, the parity source in call counted mode
    volatile uint32_t count;

    // Consecutive skipped calls, kept in frame counted mode only: a frame
    // counter stops moving whenever the game stops presenting, and a hook that
    // skipped this many calls in a row lets the next one through rather than
    // waiting for a frame that may never come
    volatile uint32_t skips;

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
