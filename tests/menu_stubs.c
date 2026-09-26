// Host stand-ins for the Vita side of the menu: files under a scratch root,
// a controllable clock, deferred threads and a taiHEN-like hook chain
#include <vitasdk.h>
#include <taihen.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "menu_stubs.h"

// Files

char stub_root[512] = "menu_fs";
int stub_fail_rename_at = 0; // fail the Nth rename from now (1 = the next one)
int stub_fail_write_at = 0;
int stub_fail_open_write = 0;
int stub_fail_open_read = 0;
int stub_rename_calls = 0;
int stub_renames_before_failing = -1; // >= 0: that many succeed, then all fail

void stub_map_path(const char *path, char *out, size_t size) {
    const char *colon = strchr(path, ':');
    snprintf(out, size, "%s/%s", stub_root, colon ? colon + 1 : path);
}

SceUID sceIoOpen(const char *file, int flags, int mode) {
    char path[1024];
    stub_map_path(file, path, sizeof(path));

    int host_flags = (flags & SCE_O_RDWR) == SCE_O_RDWR ? O_RDWR : (flags & SCE_O_WRONLY) ? O_WRONLY : O_RDONLY;
    if (flags & SCE_O_CREAT) host_flags |= O_CREAT;
    if (flags & SCE_O_TRUNC) host_flags |= O_TRUNC;
    if (flags & SCE_O_APPEND) host_flags |= O_APPEND;
    if ((flags & SCE_O_WRONLY) && stub_fail_open_write)
        return (SceUID)0x80010005;
    if (!(flags & SCE_O_WRONLY) && stub_fail_open_read)
        return (SceUID)0x80010005; // there, but unreadable

    int fd = open(path, host_flags, 0644);
    return fd < 0 ? (SceUID)0x80010002 : fd;
}

int sceIoClose(SceUID fd) {
    return close(fd) ? (int)0x80010009 : 0;
}

int sceIoRead(SceUID fd, void *data, SceSize size) {
    ssize_t n = read(fd, data, size);
    return n < 0 ? (int)0x80010005 : (int)n;
}

int sceIoWrite(SceUID fd, const void *data, SceSize size) {
    if (stub_fail_write_at && --stub_fail_write_at == 0)
        return (int)0x8001001C; // no space
    ssize_t n = write(fd, data, size);
    return n < 0 ? (int)0x80010005 : (int)n;
}

long long sceIoLseek(SceUID fd, long long offset, int whence) {
    return lseek(fd, offset, whence == SCE_SEEK_SET ? SEEK_SET : whence == SCE_SEEK_END ? SEEK_END : SEEK_CUR);
}

int sceIoMkdir(const char *dir, int mode) {
    char path[1024];
    stub_map_path(dir, path, sizeof(path));
    return mkdir(path, 0755) ? (int)0x80010011 : 0;
}

int sceIoRemove(const char *file) {
    char path[1024];
    stub_map_path(file, path, sizeof(path));
    return unlink(path) ? (int)0x80010002 : 0;
}

// Like the Vita: renaming onto an existing file fails
int sceIoRename(const char *oldname, const char *newname) {
    char from[1024], to[1024];
    struct stat st;
    stub_rename_calls++;
    if (stub_fail_rename_at && --stub_fail_rename_at == 0)
        return (int)0x80010005;
    if (stub_renames_before_failing >= 0 && stub_renames_before_failing-- == 0) {
        stub_renames_before_failing = 0; // every later one fails too
        return (int)0x80010005;
    }

    stub_map_path(oldname, from, sizeof(from));
    stub_map_path(newname, to, sizeof(to));
    if (stat(to, &st) == 0)
        return (int)0x80010011;
    return rename(from, to) ? (int)0x80010002 : 0;
}

int sceIoGetstat(const char *file, SceIoStat *stat_out) {
    char path[1024];
    struct stat st;
    stub_map_path(file, path, sizeof(path));
    if (stat(path, &st))
        return (int)0x80010002;
    memset(stat_out, 0, sizeof(*stat_out));
    stat_out->st_size = st.st_size;
    return 0;
}

// Time

SceUInt64 stub_now = 1000000;

SceUInt32 sceKernelGetProcessTimeLow(void) {
    return (SceUInt32)stub_now;
}

SceUInt64 sceKernelGetProcessTimeWide(void) {
    return stub_now;
}

// Threads: started threads run when the test says so (or on a delay)

static struct {
    SceKernelThreadEntry entry;
    bool started;
    bool done;
} g_threads[16];
static int g_thread_count = 0;
int stub_fail_create_thread = 0;
int stub_threads_run = 0;

SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int priority, SceSize stack,
        SceUInt attr, int cpu, const void *option) {
    if (stub_fail_create_thread)
        return (SceUID)0x80020001;

    // Reuse the slot of a thread that is done
    int slot = 0;
    while (slot < g_thread_count && !g_threads[slot].done)
        slot++;
    if (slot >= 16)
        return (SceUID)0x80020001;
    if (slot == g_thread_count)
        g_thread_count++;

    g_threads[slot].entry = entry;
    g_threads[slot].started = false;
    g_threads[slot].done = false;
    return 0x4000 + slot;
}

int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp) {
    g_threads[thid - 0x4000].started = true;
    return 0;
}

int sceKernelDeleteThread(SceUID thid) {
    g_threads[thid - 0x4000].done = true;
    return 0;
}

int sceKernelExitDeleteThread(int status) {
    return 0;
}

void stub_run_threads() {
    for (int i = 0; i < g_thread_count; i++) {
        if (g_threads[i].started && !g_threads[i].done) {
            g_threads[i].done = true;
            g_threads[i].entry(0, NULL);
            stub_threads_run++;
        }
    }
}

int sceKernelDelayThread(SceUInt delay) {
    stub_now += delay;
    stub_run_threads();
    return 0;
}

// Module info

char stub_titleid[16] = "PCSA00549";
char stub_self_path[256] = "ux0:/patch/PCSA00549/eboot.bin";
uint32_t stub_module_nid = 0x91ABDB4F;

int sceAppMgrAppParamGetString(int pid, int param, char *string, SceSize length) {
    snprintf(string, length, "%s", stub_titleid);
    return 0;
}

int taiGetModuleInfo(const char *module, tai_module_info_t *info) {
    info->modid = 1;
    info->module_nid = stub_module_nid;
    return 0;
}

int sceKernelGetModuleInfo(SceUID modid, SceKernelModuleInfo *info) {
    snprintf(info->path, sizeof(info->path), "%s", stub_self_path);
    return 0;
}

// Hooks. Each import has an original and a chain of hooks; like taiHEN, the first
// hook stays at the head and later ones go right after it.

void *stub_tai_next[64];

static struct {
    uint32_t nid;
    const void *hook;
    bool released;
} g_hooks[64];
static int g_hook_count = 0;

static struct {
    uint32_t nid;
    void *original;
} g_originals[32];
static int g_original_count = 0;

int stub_hooks_installed = 0;
int stub_hooks_released = 0;

void stub_set_original(uint32_t nid, void *original) {
    g_originals[g_original_count].nid = nid;
    g_originals[g_original_count].original = original;
    g_original_count++;
}

static void *stub_original(uint32_t nid) {
    for (int i = 0; i < g_original_count; i++) {
        if (g_originals[i].nid == nid)
            return g_originals[i].original;
    }
    return NULL;
}

// The live chain for an import, head first
static int stub_chain(uint32_t nid, int chain[]) {
    int count = 0;
    for (int i = 0; i < g_hook_count; i++) {
        if (g_hooks[i].nid == nid && !g_hooks[i].released)
            chain[count++] = i;
    }
    // taiHEN order: first installed, then the newest down to the second
    if (count > 2) {
        for (int i = 1, j = count - 1; i < j; i++, j--) {
            int t = chain[i]; chain[i] = chain[j]; chain[j] = t;
        }
    }
    return count;
}

static void stub_relink(uint32_t nid) {
    int chain[64];
    int count = stub_chain(nid, chain);
    for (int i = 0; i < count; i++) {
        stub_tai_next[chain[i]] = i + 1 < count ? (void *)g_hooks[chain[i + 1]].hook : stub_original(nid);
    }
}

void *stub_import(uint32_t nid) {
    int chain[64];
    return stub_chain(nid, chain) ? (void *)g_hooks[chain[0]].hook : stub_original(nid);
}

bool stub_is_hooked(uint32_t nid) {
    int chain[64];
    return stub_chain(nid, chain) > 0;
}

SceUID taiHookFunctionImport(tai_hook_ref_t *ref, const char *module, uint32_t library_nid,
        uint32_t function_nid, const void *hook) {
    if (stub_original(function_nid) == NULL || g_hook_count >= 64)
        return (SceUID)0x90010002; // the game does not import it

    int slot = g_hook_count++;
    g_hooks[slot].nid = function_nid;
    g_hooks[slot].hook = hook;
    g_hooks[slot].released = false;
    *ref = slot;
    stub_relink(function_nid);
    stub_hooks_installed++;
    return 0x100 + slot;
}

int taiHookRelease(SceUID uid, tai_hook_ref_t ref) {
    int slot = uid - 0x100;
    if (slot < 0 || slot >= g_hook_count || g_hooks[slot].released || (int)ref != slot)
        return -1;
    g_hooks[slot].released = true;
    stub_relink(g_hooks[slot].nid);
    stub_hooks_released++;
    return 0;
}

int taiInjectRelease(SceUID uid) {
    return 0;
}

void stub_reset_hooks() {
    g_hook_count = 0;
    g_original_count = 0;
    stub_hooks_installed = 0;
    stub_hooks_released = 0;
    memset(stub_tai_next, 0, sizeof(stub_tai_next));
}

// Controller, as the plugin itself reads it (its own imports are not hooked)

SceCtrlData stub_pad;

int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad_data, int count) {
    for (int i = 0; i < count; i++)
        pad_data[i] = stub_pad;
    return count;
}

int sceCtrlPeekBufferPositive2(int port, SceCtrlData *pad_data, int count) {
    return sceCtrlPeekBufferPositive(port, pad_data, count);
}

// Display

int stub_vblank_waits = 0;

int sceDisplayWaitVblankStart(void) {
    stub_vblank_waits++;
    return 0;
}

int sceDisplayWaitVblankStartMulti(unsigned int vcount) {
    stub_vblank_waits += vcount;
    return 0;
}

// The rest of the plugin

#include "../src/io.h"
#include "../src/config.h"
#include "../src/main.h"

vg_io_status_t stub_patch_status = {IO_OK, 0, 0};
vg_module_match_t stub_patch_match = MODULE_MATCH;
vg_feature_state_t stub_patch_caps[FEATURE_INVALID] = {FT_ENABLED, FT_ENABLED, FT_UNSUPPORTED, FT_ENABLED};
int stub_patch_injects = 1;

vg_io_status_t vg_patch_parse_and_apply() {
    g_main.patch_match = stub_patch_match;
    vg_config_apply_patch_capabilities(stub_patch_caps);
    if (stub_patch_match == MODULE_MATCH && stub_patch_injects) {
        g_main.inject[0] = 0x200;
        g_main.inject_num = 1;
    }
    return stub_patch_status;
}

const vg_io_status_t *vg_patch_get_status() {
    return &stub_patch_status;
}

void vg_gxp_uninstall() {
}

char stub_log[16384];

void vg_log_printf(const char *format, ...) {
    size_t length = strlen(stub_log);
    va_list args;
    va_start(args, format);
    vsnprintf(stub_log + length, sizeof(stub_log) - length, format, args);
    va_end(args);
}

void vg_log_set_enabled(bool enabled) {}
void vg_log_prepare() {}
void vg_log_flush() {}
void vg_log_read(char *dest, int size) {
    snprintf(dest, size, "%s", stub_log);
}
