#ifndef _PATCH_HOOKS_H_
#define _PATCH_HOOKS_H_

#include "io.h"
#include "config.h"

typedef enum {
    HOOK_DISPLAY_SET_FRAMEBUF_WITH_WAIT,
    HOOK_CTRL_READ_BUFFER_POSITIVE,
    HOOK_CTRL_READ_BUFFER_POSITIVE2,

    // Counts displayed frames for the '>rateDivide()' hooks that asked to be
    // counted in frames. Installed only when the first such hook is.
    HOOK_RATE_FRAME_COUNTER
} vg_hook_id_t;

typedef enum {
    // taiHookFunctionImport() by NID, for the named Sce function directives
    HOOK_KIND_IMPORT,
    // taiHookFunctionOffset() into the game, for '>rateDivide()'
    HOOK_KIND_RATE_DIVIDE
} vg_hook_kind_t;

/**
 * One '>' directive, as parsed. Everything the directive resolved to at the
 * current configuration is here, which is also what the host side checker
 * (tests/test_patchlist.c) reports, so that a patch file is validated by the
 * same parser that will run it on hardware.
 */
typedef struct {
    vg_hook_kind_t kind;

    // Directive name as it is written, for logs and for the checker
    const char *name;

    // import hooks
    vg_hook_id_t hook_id;
    uint32_t import_nid;
    const void *hook_ptr;

    // rate divided hooks
    uint8_t segment;
    uint32_t offset;
    bool thumb;
    uint32_t divisor;
    bool frame_counted;
    uint32_t ret_value;
    uint32_t arg_num;
} vg_hook_request_t;

int sceCtrlPeekBufferPositive2(int port, SceCtrlData *pad_data, int count);

vg_io_status_t vg_hook_parse_request(const char line[], vg_feature_t feature,
            vg_hook_request_t *request, uint8_t *shall_hook);
vg_io_status_t vg_hook_apply_request(const vg_hook_request_t *request, uint8_t shall_hook);
vg_io_status_t vg_hook_parse_patch(const char line[], vg_feature_t feature);

#endif
