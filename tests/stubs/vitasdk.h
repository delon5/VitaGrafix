/*
 * Minimal host stand-in for <vitasdk.h>, enough to compile the plugin sources
 * that the host test suites link (src/io.c, src/patch_hook.c).
 */
#ifndef STUB_VITASDK_H
#define STUB_VITASDK_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <strings.h>

typedef int SceUID;
typedef uint32_t SceUInt32;
typedef uint32_t SceSize;

// Layout trimmed to what the plugin reads, field names and meaning as in
// <psp2/kernel/modulemgr.h>
typedef struct SceKernelSegmentInfo {
    SceSize size;
    SceUInt32 perms;
    void *vaddr;
    SceSize memsz;
    SceSize filesz;
    SceUInt32 res;
} SceKernelSegmentInfo;

typedef struct {
    SceSize size;
    char path[256];
    SceKernelSegmentInfo segments[4];
} SceKernelModuleInfo;

typedef struct SceDisplayFrameBuf {
    SceSize size;
    void *base;
    unsigned int pitch;
    unsigned int pixelformat;
    unsigned int width;
    unsigned int height;
} SceDisplayFrameBuf;

typedef struct SceCtrlData {
    uint64_t timeStamp;
    unsigned int buttons;
    unsigned char lx, ly, rx, ry;
} SceCtrlData;

#define SCE_O_RDONLY 1
#define SCE_O_WRONLY 2
#define SCE_O_CREAT  0x200
#define SCE_O_TRUNC  0x400
#define SCE_O_APPEND 0x100
#define SCE_SEEK_SET 0
#define SCE_SEEK_CUR 1
#define SCE_SEEK_END 2

SceUID sceIoOpen(const char *file, int flags, int mode);
int sceIoClose(SceUID fd);
int sceIoRead(SceUID fd, void *data, SceSize size);
int sceIoWrite(SceUID fd, const void *data, SceSize size);
long long sceIoLseek(SceUID fd, long long offset, int whence);
int sceIoMkdir(const char *dir, int mode);

int sceDisplayWaitVblankStartMulti(unsigned int vcount);
int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad_data, int count);

#endif
