/*
 * Minimal host stand-in for <taihen.h>. The hook chain layout and the
 * TAI_CONTINUE macro match the real header, so a hook body compiled against
 * this one behaves as it does on the Vita; the hook calls themselves are
 * implemented by the test.
 */
#ifndef STUB_TAIHEN_H
#define STUB_TAIHEN_H
#include <stdint.h>

#define TAI_ANY_LIBRARY 0xFFFFFFFF
#define TAI_MAIN_MODULE ((void *)0)

typedef uintptr_t tai_hook_ref_t;

typedef struct { uint32_t size; int modid; uint32_t module_nid; } tai_module_info_t;

struct _tai_hook_user {
    uintptr_t next;
    void *func;
    void *old;
};

SceUID taiHookFunctionImport(tai_hook_ref_t *p_hook, void *module, uint32_t library_nid, uint32_t func_nid, const void *hook_func);
SceUID taiHookFunctionOffset(tai_hook_ref_t *p_hook, SceUID modid, int segidx, uint32_t offset, int thumb, const void *hook_func);
int taiHookRelease(SceUID tai_uid, tai_hook_ref_t hook);

#define TAI_CONTINUE(type, hook, ...) ({ \
  struct _tai_hook_user *cur, *next; \
  cur = (struct _tai_hook_user *)(hook); \
  next = (struct _tai_hook_user *)cur->next; \
  (next == NULL) ? \
    ((type(*)())cur->old)(__VA_ARGS__) \
  : \
    ((type(*)())next->func)(__VA_ARGS__) \
  ; \
})

#endif
