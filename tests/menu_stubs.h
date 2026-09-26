#ifndef TEST_MENU_STUBS_H
#define TEST_MENU_STUBS_H
#include <vitasdk.h>

extern char stub_root[512];
extern int stub_fail_rename_at;
extern int stub_fail_write_at;
extern int stub_fail_open_write;
extern int stub_fail_open_read;
extern int stub_rename_calls;
extern int stub_renames_before_failing;
void stub_map_path(const char *path, char *out, size_t size);

extern SceUInt64 stub_now;

extern int stub_fail_create_thread;
extern int stub_threads_run;
void stub_run_threads();

extern char stub_titleid[16];
extern char stub_self_path[256];
extern uint32_t stub_module_nid;

extern int stub_hooks_installed;
extern int stub_hooks_released;
void stub_set_original(uint32_t nid, void *original);
void *stub_import(uint32_t nid);
bool stub_is_hooked(uint32_t nid);
void stub_reset_hooks();

extern SceCtrlData stub_pad;
extern int stub_vblank_waits;
extern char stub_log[16384];
#endif
