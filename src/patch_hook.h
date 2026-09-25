#ifndef _PATCH_HOOKS_H_
#define _PATCH_HOOKS_H_

typedef enum {
    HOOK_DISPLAY_SET_FRAMEBUF_WITH_WAIT,
    HOOK_CTRL_READ_BUFFER_POSITIVE,
    HOOK_CTRL_READ_BUFFER_POSITIVE2,
    HOOK_GXM_REGISTER_PROGRAM
} vg_hook_id_t;

int sceCtrlPeekBufferPositive2(int port, SceCtrlData *pad_data, int count);

vg_io_status_t vg_hook_function_import(vg_hook_id_t hook_id, uint32_t nid, const void *func);
vg_io_status_t vg_hook_parse_patch(const char line[]);

#endif
