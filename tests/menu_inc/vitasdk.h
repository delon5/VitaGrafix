// Host stand-in for the parts of the VitaSDK that test_menu needs
#ifndef TEST_MENU_VITASDK_H
#define TEST_MENU_VITASDK_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

typedef int SceUID;
typedef unsigned int SceSize;
typedef unsigned int SceUInt;
typedef uint32_t SceUInt32;
typedef int32_t SceInt32;
typedef uint64_t SceUInt64;
typedef int64_t SceOff;

#define SCE_KERNEL_START_SUCCESS 0
#define SCE_KERNEL_STOP_SUCCESS  0

typedef struct SceCtrlData {
    uint64_t timeStamp;
    unsigned int buttons;
    unsigned char lx, ly, rx, ry;
    uint8_t up, right, down, left;
    uint8_t lt, rt, l1, r1;
    uint8_t triangle, circle, cross, square;
    uint8_t reserved[4];
} SceCtrlData;
_Static_assert(sizeof(SceCtrlData) == 0x20, "SceCtrlData");

enum {
    SCE_CTRL_SELECT = 0x1, SCE_CTRL_L3 = 0x2, SCE_CTRL_R3 = 0x4, SCE_CTRL_START = 0x8,
    SCE_CTRL_UP = 0x10, SCE_CTRL_RIGHT = 0x20, SCE_CTRL_DOWN = 0x40, SCE_CTRL_LEFT = 0x80,
    SCE_CTRL_LTRIGGER = 0x100, SCE_CTRL_RTRIGGER = 0x200, SCE_CTRL_L1 = 0x400, SCE_CTRL_R1 = 0x800,
    SCE_CTRL_TRIANGLE = 0x1000, SCE_CTRL_CIRCLE = 0x2000, SCE_CTRL_CROSS = 0x4000, SCE_CTRL_SQUARE = 0x8000,
    SCE_CTRL_INTERCEPTED = 0x10000, SCE_CTRL_HEADPHONE = 0x80000, SCE_CTRL_VOLUP = 0x100000,
};

typedef struct SceTouchReport {
    uint8_t id, force;
    uint16_t x, y;
    int8_t reserved[8];
    uint16_t info;
} SceTouchReport;

typedef struct SceTouchData {
    SceUInt64 timeStamp;
    SceUInt32 status;
    SceUInt32 reportNum;
    SceTouchReport report[8];
} SceTouchData;

typedef struct SceDisplayFrameBuf {
    SceSize size;
    void *base;
    unsigned int pitch;
    unsigned int pixelformat;
    unsigned int width;
    unsigned int height;
} SceDisplayFrameBuf;

typedef struct { SceSize size; char name[28]; char path[256]; } SceKernelModuleInfo;
typedef struct { int st_mode; unsigned int st_attr; SceOff st_size; } SceIoStat;

#define SCE_O_RDONLY 0x0001
#define SCE_O_WRONLY 0x0002
#define SCE_O_RDWR   0x0003
#define SCE_O_APPEND 0x0100
#define SCE_O_CREAT  0x0200
#define SCE_O_TRUNC  0x0400
#define SCE_SEEK_SET 0
#define SCE_SEEK_CUR 1
#define SCE_SEEK_END 2

SceUID sceIoOpen(const char *file, int flags, int mode);
int sceIoClose(SceUID fd);
int sceIoRead(SceUID fd, void *data, SceSize size);
int sceIoWrite(SceUID fd, const void *data, SceSize size);
long long sceIoLseek(SceUID fd, long long offset, int whence);
int sceIoMkdir(const char *dir, int mode);
int sceIoRemove(const char *file);
int sceIoRename(const char *oldname, const char *newname);
int sceIoGetstat(const char *file, SceIoStat *stat);

SceUInt32 sceKernelGetProcessTimeLow(void);
SceUInt64 sceKernelGetProcessTimeWide(void);

typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int priority, SceSize stack,
        SceUInt attr, int cpu, const void *option);
int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp);
int sceKernelDeleteThread(SceUID thid);
int sceKernelExitDeleteThread(int status);
int sceKernelDelayThread(SceUInt delay);
int sceKernelGetModuleInfo(SceUID modid, SceKernelModuleInfo *info);

int sceAppMgrAppParamGetString(int pid, int param, char *string, SceSize length);

int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad_data, int count);
int sceCtrlPeekBufferPositive2(int port, SceCtrlData *pad_data, int count);

int sceDisplayWaitVblankStart(void);
int sceDisplayWaitVblankStartMulti(unsigned int vcount);
#endif
