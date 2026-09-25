#include <vitasdk.h>
#include <taihen.h>
#include <string.h>
#include <strings.h>

#include "io.h"
#include "log.h"
#include "config.h"
#include "patch.h"
#include "patch_hook.h"
#include "patch_gxp.h"
#include "patch_gxp_core.h"
#include "main.h"

#include "interpreter/interpreter.h"

#define GXP_RULE_LOG_MAX    16   // distinct programs reported for value rules
#define GXP_ERR_PER_PROGRAM 4    // error lines per program named by gxp lines
#define GXP_ERR_MISC        16   // other error and survey lines per session

// Written while the patch file is parsed (module_start), read-only once the hook is installed
static vg_gxp_table_t g_gxp;
static uint16_t g_gxp_ordinal;

// Guarded by g_gxp_lock (the hook runs on whatever thread registers the program)
static SceKernelLwMutexWork g_gxp_lock;
static bool g_gxp_lock_ok;
static uint16_t g_gxp_patched[VG_GXP_MAX_PATCHES];   // per program slot
static uint8_t g_gxp_errlines[VG_GXP_MAX_PATCHES];   // per program slot
static uint8_t g_gxp_misc_lines;
static bool g_gxp_suppressed;
static uint32_t g_gxp_rule_logged[GXP_RULE_LOG_MAX];
static uint8_t g_gxp_rule_logged_count;

void vg_gxp_reset() {
    vg_gxp_table_reset(&g_gxp);
    g_gxp_ordinal = 0;
}

static void log_bytes(const uint8_t *b, uint8_t size, uint32_t gaps) {
    for (uint8_t i = 0; i < size; i++) {
        if (gaps & (1u << i))
            vg_log_printf(" ??");
        else
            vg_log_printf(" %02X", b[i]);
    }
}

vg_io_status_t vg_gxp_parse_patch(const char line[]) {
    vg_gxp_line_t l;
    uint32_t pos = 0;
    intp_status_t ist = {0};

    vg_gxp_parse_t pr = vg_gxp_parse_line(line, &l, &pos, &ist);
    switch (pr) {
        case VG_GXP_PARSE_OK:
        case VG_GXP_PARSE_UNCHANGED:
            break;
        case VG_GXP_PARSE_EVAL: {
            char buf[256];
            intp_format_error(line, ist, buf, sizeof(buf));
            vg_log_printf("%s\n", buf);
            __ret_status(IO_ERROR_INTERPRETER_ERROR, 0, pos);
        }
        default:
            vg_log_printf("[PATCH] Invalid shader patch (%s) at pos %u\n",
                          pr == VG_GXP_PARSE_SIZE ? "both sides need the same size; gxplit: whole words"
                          : pr == VG_GXP_PARSE_GAPS ? "?? gaps differ, or nothing but gaps" : "syntax", pos);
            __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
    }

    g_gxp_ordinal++;
    if (l.approximated)
        vg_log_printf("[PATCH] WARNING: immediate has no exact encoding and was rounded in: %s\n", line);

    if (l.kind == VG_GXP_LINE_RULE) {
        l.rule.ordinal = g_gxp_ordinal;
        if (pr == VG_GXP_PARSE_UNCHANGED) {
            vg_log_printf("[PATCH] Skipped gxplit rule %u, value is stock\n", g_gxp_ordinal);
            __ret_status(IO_OK, 0, 0);
        }
    } else {
        l.patch.ordinal = g_gxp_ordinal;
    }

    switch (vg_gxp_table_add(&g_gxp, &l)) {
        case VG_GXP_ADD_OK:
            break;
        case VG_GXP_ADD_FULL:
            __ret_status(IO_ERROR_TOO_MANY_GXP_PATCHES, 0, 0);
        case VG_GXP_ADD_OVERLAP:
            vg_log_printf("[PATCH] Shader patch overlaps an earlier line for the same program\n");
            __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, 0);
        case VG_GXP_ADD_CHAIN:
            vg_log_printf("[PATCH] gxplit rule writes words another rule looks for (or the reverse)\n");
            __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, 0);
    }

    if (l.kind == VG_GXP_LINE_RULE) {
        vg_log_printf("[PATCH] Queued gxplit rule %u:", l.rule.ordinal);
        for (uint8_t w = 0; w < l.rule.words; w++)
            vg_log_printf(" %08X", l.rule.stock[w]);
        vg_log_printf(" ->");
        for (uint8_t w = 0; w < l.rule.words; w++)
            vg_log_printf(" %08X", l.rule.data[w]);
        vg_log_printf("\n");
    } else if (pr == VG_GXP_PARSE_UNCHANGED) {
        vg_log_printf("[PATCH] Skipped gxp %08X:+0x%X, value is stock\n", l.patch.hash, l.patch.offset);
    } else {
        vg_log_printf("[PATCH] Queued gxp %08X:+0x%X =", l.patch.hash, l.patch.offset);
        log_bytes(l.patch.data, l.patch.size, l.patch.gap_mask);
        vg_log_printf(" (%u bytes)\n", l.patch.size);
    }
    __ret_status(IO_OK, 0, 0);
}

/*
 * Runtime (the hook)
 */
static void lock() {
    if (g_gxp_lock_ok)
        sceKernelLockLwMutex(&g_gxp_lock, 1, NULL);
}

static void unlock() {
    if (g_gxp_lock_ok)
        sceKernelUnlockLwMutex(&g_gxp_lock, 1);
}

// [prog, prog + size) lies in one mapped block; *writable: that block can be written
static bool vg_gxp_mapped(const void *prog, uint32_t size, bool *writable, int *rc, uint32_t *access) {
    SceKernelMemBlockInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    *rc = sceKernelGetMemBlockInfoByRange((void *)prog, size, &info);
    *access = info.access;
    *writable = false;
    if (*rc < 0)
        return false;
    uint32_t base = (uint32_t)info.mappedBase, p = (uint32_t)prog;
    if (p < base || size > info.mappedSize || p - base > info.mappedSize - size)
        return false;
    *writable = (info.access & SCE_KERNEL_MEMORY_ACCESS_W) != 0;
    return true;
}

// An error or survey line, capped per program and per session; never flushed on its own (lock held)
static bool vg_gxp_may_log(const vg_gxp_result_t *r) {
    bool named = r->count > 0;
    uint8_t slot = named ? g_gxp.e[r->first].prog : 0;
    if (named && g_gxp_errlines[slot] < GXP_ERR_PER_PROGRAM) {
        g_gxp_errlines[slot]++;
        return true;
    }
    if (!named && g_gxp_misc_lines < GXP_ERR_MISC) {
        g_gxp_misc_lines++;
        return true;
    }
    if (!g_gxp_suppressed) {
        g_gxp_suppressed = true;
        vg_log_printf("[GXP] further lines suppressed\n");
    }
    return false;
}

static void vg_gxp_log_problem(const void *prog, const vg_gxp_result_t *r, const char *what) {
    if (!vg_gxp_may_log(r))
        return;
    switch (r->status) {
        case GXP_STOCK_MISMATCH:
            vg_log_printf("[GXP] %08X at 0x%08X (%u bytes): %s, byte +0x%X is not the stock value; nothing written\n",
                          r->hash, (uint32_t)prog, r->size, what, r->bad_offset);
            break;
        case GXP_OUT_OF_RANGE:
            vg_log_printf("[GXP] %08X at 0x%08X (%u bytes): %s, offset 0x%X is past the end; nothing written\n",
                          r->hash, (uint32_t)prog, r->size, what, r->bad_offset);
            break;
        case GXP_RULE_CONFLICT:
            vg_log_printf("[GXP] %08X at 0x%08X (%u bytes): two value rules claim the literal at +0x%X; nothing written\n",
                          r->hash, (uint32_t)prog, r->size, r->bad_offset);
            break;
        case GXP_RULE_TOO_MANY:
            vg_log_printf("[GXP] %08X at 0x%08X (%u bytes): more than %u value rule sites; nothing written\n",
                          r->hash, (uint32_t)prog, r->size, VG_GXP_MAX_RULE_SITES);
            break;
        case GXP_SURVEY:
            vg_log_printf("[GXP] Survey: %08X (%u bytes) holds stock literal %08X at +0x%X, no rule matched\n",
                          r->hash, r->size, r->survey_word, r->survey_offset);
            break;
        default:
            vg_log_printf("[GXP] %08X at 0x%08X (%u bytes): %s\n", r->hash, (uint32_t)prog, r->size, what);
            break;
    }
}

static void vg_gxp_log_patched(const void *prog, const vg_gxp_result_t *r) {
    if (r->status == GXP_MATCHED) {
        uint8_t slot = g_gxp.e[r->first].prog;
        uint16_t n = g_gxp_patched[slot] < 0xFFFF ? ++g_gxp_patched[slot] : g_gxp_patched[slot];
        if (n == 1) {
            vg_log_printf("[GXP] Patched %08X at 0x%08X (%u bytes): %u sites, %u bytes\n",
                          r->hash, (uint32_t)prog, r->size, r->sites, r->bytes);
            vg_log_flush();
        } else if (n == 2) {
            vg_log_printf("[GXP] Patched %08X again at 0x%08X\n", r->hash, (uint32_t)prog);
        }
        return;
    }

    // Value rule: a program the patch file does not list yet
    for (uint8_t i = 0; i < g_gxp_rule_logged_count; i++) {
        if (g_gxp_rule_logged[i] == r->hash)
            return;
    }
    if (g_gxp_rule_logged_count >= GXP_RULE_LOG_MAX)
        return;
    g_gxp_rule_logged[g_gxp_rule_logged_count++] = r->hash;
    vg_log_printf("[GXP] Rule %u patched %08X at 0x%08X (%u bytes):", g_gxp.r[r->site[0].rule].ordinal,
                  r->hash, (uint32_t)prog, r->size);
    for (uint8_t s = 0; s < r->nsite; s++)
        vg_log_printf(" +0x%X", r->site[s].offset + 4);
    vg_log_printf(" (not in the hash list, please report)\n");
    vg_log_flush();
}

static void vg_gxp_apply(void *prog, vg_gxp_result_t *r) {
    bool writable;
    int rc;
    uint32_t access;
    bool mapped = vg_gxp_mapped(prog, r->size, &writable, &rc, &access);

    lock();
    if (!mapped || !writable) {
        if (vg_gxp_may_log(r))
            vg_log_printf("[GXP] %08X at 0x%08X (%u bytes): not writable (rc 0x%08X, access 0x%X); nothing written\n",
                          r->hash, (uint32_t)prog, r->size, rc, access);
    } else {
        // Another thread may have registered the same buffer since the match
        vg_gxp_status_t st = vg_gxp_recheck(&g_gxp, prog, r);
        if (st == r->status) {
            vg_gxp_commit(&g_gxp, prog, r);
            vg_gxp_log_patched(prog, r);
        } else if (st == GXP_STOCK_MISMATCH) {
            vg_gxp_status_t matched = r->status;
            r->status = GXP_STOCK_MISMATCH;
            vg_gxp_log_problem(prog, r, matched == GXP_MATCHED ? "changed while patching" : "rule site changed");
        }
        // GXP_ALREADY_PATCHED: the other registration wrote the same bytes
    }
    unlock();
}

static int vg_gxp_hook_register(SceGxmShaderPatcher *shaderPatcher, const SceGxmProgram *programHeader,
                                SceGxmShaderPatcherId *programId) {
    uint32_t size;
    if (programHeader && vg_gxp_program_size(programHeader, &size)) {
        bool writable;
        int rc;
        uint32_t access;
        // Hash only a program that lies in mapped memory
        if (vg_gxp_mapped(programHeader, size, &writable, &rc, &access)) {
            vg_gxp_result_t r;
            vg_gxp_status_t st = vg_gxp_match(&g_gxp, programHeader, &r);
            if (st == GXP_MATCHED || st == GXP_RULE_MATCHED) {
                vg_gxp_apply((void *)programHeader, &r);
            } else if (st != GXP_NO_MATCH && st != GXP_NOT_PROGRAM) {
                lock();
                vg_gxp_log_problem(programHeader, &r, "the program differs from the one the patch was made for");
                unlock();
            }
        }
    }
    return TAI_CONTINUE(int, g_main.hook_ref[HOOK_GXM_REGISTER_PROGRAM], shaderPatcher, programHeader, programId);
}

vg_io_status_t vg_gxp_install() {
    if (!g_gxp.active && !g_gxp.rule_count) {
        if (g_gxp.count)
            vg_log_printf("[PATCH] Shader patches: all %u lines are stock at this resolution\n", g_gxp.count);
        __ret_status(IO_OK, 0, 0);
    }

    vg_gxp_table_seal(&g_gxp);
    memset(g_gxp_patched, 0, sizeof(g_gxp_patched));
    memset(g_gxp_errlines, 0, sizeof(g_gxp_errlines));
    g_gxp_misc_lines = 0;
    g_gxp_suppressed = false;
    g_gxp_rule_logged_count = 0;

    g_gxp_lock_ok = sceKernelCreateLwMutex(&g_gxp_lock, "VitaGrafixGxp", 0, 0, NULL) >= 0;
    if (!g_gxp_lock_ok) {
        vg_log_printf("[PATCH] Shader patches not installed: could not create a lock\n");
        __ret_status(IO_ERROR_TAI_GENERIC, 0, 0);
    }

    vg_log_printf("[PATCH] Queued %u shader patches for %u programs and %u value rules\n",
                  g_gxp.active, g_gxp.programs, g_gxp.rule_count);
    return vg_hook_function_import(HOOK_GXM_REGISTER_PROGRAM, 0x2B528462, vg_gxp_hook_register);
}

void vg_gxp_uninstall() {
    if (g_gxp_lock_ok) {
        sceKernelDeleteLwMutex(&g_gxp_lock);
        g_gxp_lock_ok = false;
    }
}
