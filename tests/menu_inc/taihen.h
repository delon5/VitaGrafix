// Host stand-in for taiHEN: hooks become a table the test fills with "original" functions
#ifndef TEST_MENU_TAIHEN_H
#define TEST_MENU_TAIHEN_H
#include <stdint.h>

typedef uintptr_t tai_hook_ref_t;
typedef struct { SceSize size; SceUID modid; uint32_t module_nid; } tai_module_info_t;

#define TAI_MAIN_MODULE  ((const char *)0)
#define TAI_ANY_LIBRARY  0xFFFFFFFF

// The next function in the chain, called without a prototype like taiHEN's macro does
extern void *stub_tai_next[64];
#define TAI_CONTINUE(type, ref, ...) (((type (*)())stub_tai_next[(ref)])(__VA_ARGS__))

SceUID taiHookFunctionImport(tai_hook_ref_t *ref, const char *module, uint32_t library_nid,
        uint32_t function_nid, const void *hook);
int taiHookRelease(SceUID uid, tai_hook_ref_t ref);
int taiInjectRelease(SceUID uid);
int taiGetModuleInfo(const char *module, tai_module_info_t *info);
#endif
