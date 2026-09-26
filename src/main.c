#include <vitasdk.h>
#include <taihen.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#include "io.h"
#include "log.h"
#include "config.h"
#include "menu.h"
#include "patch.h"
#include "patch_gxp.h"
#include "main.h"
#include "osd.h"

vg_main_t g_main = {0};

// string buffer
char g_osd_buffer[STRING_BUFFER_SIZE] = "";

#define DECL_FUNC_HOOK_INTERCEPT_CTRL(index, name, negative) \
    static int name##_patched(int port, SceCtrlData *ctrl, int count) { \
        int ret = TAI_CONTINUE(int, g_main.input_hook_ref[(index)], port, ctrl, count); \
        vg_menu_filter_ctrl(ctrl, ret < count ? ret : count, (negative)); \
        return ret; \
    }

#define DECL_FUNC_HOOK_INTERCEPT_TOUCH(index, name) \
    static int name##_patched(SceUInt32 port, SceTouchData *data, SceUInt32 count) { \
        int ret = TAI_CONTINUE(int, g_main.input_hook_ref[(index)], port, data, count); \
        vg_menu_filter_touch(data, ret < (int)count ? ret : (int)count); \
        return ret; \
    }

DECL_FUNC_HOOK_INTERCEPT_CTRL(0, sceCtrlPeekBufferNegative, true)
DECL_FUNC_HOOK_INTERCEPT_CTRL(1, sceCtrlPeekBufferNegative2, true)
DECL_FUNC_HOOK_INTERCEPT_CTRL(2, sceCtrlPeekBufferPositive, false)
DECL_FUNC_HOOK_INTERCEPT_CTRL(3, sceCtrlPeekBufferPositive2, false)
DECL_FUNC_HOOK_INTERCEPT_CTRL(4, sceCtrlReadBufferNegative, true)
DECL_FUNC_HOOK_INTERCEPT_CTRL(5, sceCtrlReadBufferNegative2, true)
DECL_FUNC_HOOK_INTERCEPT_CTRL(6, sceCtrlReadBufferPositive, false)
DECL_FUNC_HOOK_INTERCEPT_CTRL(7, sceCtrlReadBufferPositive2, false)
DECL_FUNC_HOOK_INTERCEPT_CTRL(8, sceCtrlPeekBufferPositiveExt, false)
DECL_FUNC_HOOK_INTERCEPT_CTRL(9, sceCtrlPeekBufferPositiveExt2, false)
DECL_FUNC_HOOK_INTERCEPT_CTRL(10, sceCtrlReadBufferPositiveExt, false)
DECL_FUNC_HOOK_INTERCEPT_CTRL(11, sceCtrlReadBufferPositiveExt2, false)
DECL_FUNC_HOOK_INTERCEPT_TOUCH(12, sceTouchPeek)
DECL_FUNC_HOOK_INTERCEPT_TOUCH(13, sceTouchRead)

static const struct {
    uint32_t nid;
    const void *func;
} g_input_hooks[INPUT_HOOK_NUM] = {
    {0x104ED1A7, sceCtrlPeekBufferNegative_patched},
    {0x81A89660, sceCtrlPeekBufferNegative2_patched},
    {0xA9C3CED6, sceCtrlPeekBufferPositive_patched},
    {0x15F81E8C, sceCtrlPeekBufferPositive2_patched},
    {0x15F96FB0, sceCtrlReadBufferNegative_patched},
    {0x27A0C5FB, sceCtrlReadBufferNegative2_patched},
    {0x67E7AB83, sceCtrlReadBufferPositive_patched},
    {0xC4226A3E, sceCtrlReadBufferPositive2_patched},
    {0xA59454D3, sceCtrlPeekBufferPositiveExt_patched},
    {0x860BF292, sceCtrlPeekBufferPositiveExt2_patched},
    {0xE2D99296, sceCtrlReadBufferPositiveExt_patched},
    {0xA7178860, sceCtrlReadBufferPositiveExt2_patched},
    {0xFF082DF0, sceTouchPeek_patched},
    {0x169A1D58, sceTouchRead_patched},
};

// The classic overlay's box width at 960x544 for this configuration
static int vg_main_get_osd_width() {
    const vg_config_t config = *vg_config_get();

    int w = 180;      // Fit "960x544" or "60 FPS"

    if (config.fb_enabled == FT_UNSUPPORTED && config.ib_enabled == FT_UNSUPPORTED
            && (config.msaa_enabled == FT_DISABLED || config.msaa_enabled == FT_UNSPECIFIED)) {
        w += 60;      // Fit "MSAA: default"
    } else if (config.fb_enabled == FT_DISABLED || config.fb_enabled == FT_UNSPECIFIED
            || config.ib_enabled == FT_DISABLED || config.ib_enabled == FT_UNSPECIFIED
            || config.fps_enabled == FT_DISABLED || config.fps_enabled == FT_UNSPECIFIED) {
        w += 50;      // Fit "Res: default" or "FPS: default"
    } else if (config.fb_enabled == FT_UNSUPPORTED && config.ib_enabled == FT_UNSUPPORTED
            && config.msaa_enabled == FT_ENABLED) {
        w += 10;      // Fit "MSAA: 4x"
    }

    if (config.ib_enabled == FT_ENABLED) {
        if (config.ib[0].width > 999)
            w += 10;  // Fit "1280x720"
        if (config.ib_count > 1)
            w += 110; // Fit "960x544 >> 720x408"
    }
    if ((config.fb_enabled != FT_UNSUPPORTED || config.ib_enabled != FT_UNSUPPORTED)
            && !((config.fb_enabled == FT_ENABLED || config.ib_enabled == FT_ENABLED)           // "960x544 (4x)"
                && (config.fps_enabled == FT_DISABLED || config.fps_enabled == FT_UNSPECIFIED)) // "FPS: default"
            && config.msaa_enabled == FT_ENABLED) {
        w += 50;      // Fit "960x544 (4x)" or "Res: default (4x)"
    }

    return w;
}

// The overlay box in the top right corner: logo, version and up to two lines of text
// (a single line sits at the bottom). Coordinates are in the 960x544 space the menu uses;
// the spacing follows the font, which at 960x544 gives the classic 70 px high box.
static void vg_main_draw_overlay_box(int base_width, const char *top, const char *bottom) {
    int text_width = top ? osd_get_text_width(top) : 0;
    if (bottom && (int)osd_get_text_width(bottom) > text_width)
        text_width = osd_get_text_width(bottom);

    // Small framebuffers have no smaller font for the version: keep the text clear of it
    int version_width = osd_get_text_width_small(VG_VERSION);
    int text_x = 41 + version_width + 10 > 90 ? 41 + version_width + 10 : 90;

    int line_height = osd_get_text_height();
    int width = base_width;
    if (text_x + text_width + 10 > width)
        width = text_x + text_width + 10;
    int height = 2 * line_height + 30 > 70 ? 2 * line_height + 30 : 70;
    int x = 960 - 20 - width;
    int bottom_y = 20 + height - 14 - line_height;

    // Background
    osd_set_back_color(0, 0, 0, 200);
    osd_draw_rounded_rectangle(x, 20, width, height, 0);

    // Logo and version
    osd_draw_logo(x + 15, 30); // 60x38
    osd_set_back_color(0, 0, 0, 0);
    osd_set_text_color(255, 255, 255, 255);
    osd_draw_string_small(x + 41, 20 + height - 6 - osd_get_text_height_small(), VG_VERSION);

    if (bottom) {
        osd_draw_string(x + text_x, bottom_y, bottom);
        if (top)
            osd_draw_string(x + text_x, bottom_y - line_height, top);
    } else if (top) {
        osd_draw_string(x + text_x, bottom_y, top);
    }
}

// Draws the OSD (or the menu) onto the frame about to be shown.
// Returns true when the menu is open.
static bool vg_main_draw_osd(const SceDisplayFrameBuf *pParam) {
    const vg_config_t config = *vg_config_get();
    const vg_io_status_t config_status = *vg_config_get_status();
    const vg_io_status_t patch_status = *vg_patch_get_status();

    // OSD not shown yet? Start the timer
    if (!g_main.osd_timer) {
        g_main.osd_timer = sceKernelGetProcessTimeWide();
    }

    osd_update_fb(pParam);

    if (g_main.menu_ready) {
        SceCtrlData ctrl;
        bool has_input = sceCtrlPeekBufferPositive(0, &ctrl, 1) > 0;

        // A new notice (saving, saved, failed) shows the OSD again
        if (vg_menu_update(has_input ? &ctrl : NULL)) {
            g_main.osd_timer = sceKernelGetProcessTimeWide();
            g_main.osd_done = false;
        }

        if (vg_menu_is_open()) {
            vg_menu_draw();
            return true;
        }
    }

    // OSD timer finished? Stop drawing. The hook itself stays installed until
    // module_stop: unhooking from inside the call chain is not safe when another
    // plugin has hooked the same import (their chain would point at freed memory)
    if (g_main.osd_done
            || (sceKernelGetProcessTimeWide() - g_main.osd_timer > OSD_SHOW_DURATION
            && config_status.code == IO_OK // Show indefinitely on i/o error
            && patch_status.code == IO_OK)) {
        g_main.osd_done = true;
        return false;
    }

    vg_menu_notice_t notice = g_main.menu_ready ? vg_menu_get_notice() : MENU_NOTICE_NONE;

    // IO/parse failure?
    if (config_status.code != IO_OK || patch_status.code != IO_OK) {
        vg_main_draw_overlay_box(vg_main_get_osd_width(), "Error", NULL);

        // Draw short message, one font height per line (20 at 960x544)
        int line_height = osd_get_text_height();
        osd_set_back_color(0, 0, 0, 255);
        if (config_status.code == IO_ERROR_OPEN_FAILED) {
            osd_draw_string(20, 110, OSD_MSG_CONFIG_OPEN_FAILED);
            osd_draw_string(20, 110 + line_height, OSD_MSG_IOPLUS_HINT);
        } else if (patch_status.code == IO_ERROR_OPEN_FAILED) {
            osd_draw_string(20, 110, OSD_MSG_PATCH_OPEN_FAILED);
            osd_draw_string(20, 110 + line_height, OSD_MSG_IOPLUS_HINT);
        } else if (config_status.code != IO_OK) {
            // Which file, and where
            const char *file = vg_config_get_error_path();
            if (!strncmp(file, VG_DIR, strlen(VG_DIR)))
                file += strlen(VG_DIR);
            char where[96];
            snprintf(where, sizeof(where), "%s, line %u", file, (unsigned int)config_status.line);
            osd_draw_string(20, 110, OSD_MSG_CONFIG_ERROR);
            osd_draw_string(20, 110 + line_height, where);
        } else if (patch_status.code != IO_OK) {
            osd_draw_string(20, 110, OSD_MSG_PATCH_ERROR);
        }

        // Draw the end of the log
        if (config.log_enabled) {
            osd_draw_log(20, 110 + 2 * line_height, pParam->height, g_osd_buffer);
        }
    }
    // Wrong version
    else if (g_main.patch_match == MODULE_NID_MISMATCH) {
        vg_main_draw_overlay_box(vg_main_get_osd_width(), "Error", NULL);
        osd_set_back_color(0, 0, 0, 255);
        osd_draw_string(480 - osd_get_text_width(OSD_MSG_GAME_WRONG_VERSION) / 2, 272 - 20,
                OSD_MSG_GAME_WRONG_VERSION);
    }
    // In-game menu saves
    else if (notice == MENU_NOTICE_SAVING) {
        vg_main_draw_overlay_box(180, OSD_MSG_CONFIG_SAVING, NULL);
    } else if (notice == MENU_NOTICE_SAVED) {
        vg_main_draw_overlay_box(180, OSD_MSG_CONFIG_SAVED, OSD_MSG_CONFIG_SAVED_2);
    } else if (notice == MENU_NOTICE_SAVE_FAILED) {
        vg_main_draw_overlay_box(180, OSD_MSG_CONFIG_SAVE_FAILED, NULL);
    }
    // Active settings
    else {
        // MSAA
        char msaa_sm_buf[16] = "";
        if (config.msaa_enabled == FT_ENABLED) {
            snprintf(msaa_sm_buf, 16, "%s",
                    (config.msaa == MSAA_4X ? "4x" :
                    (config.msaa == MSAA_2X ? "2x" : "1x")));
        }

        // 2nd line
        char fps_buf[16] = "";
        if (config.fps_enabled == FT_ENABLED) {
            snprintf(fps_buf, 16, "%d FPS",
                    config.fps == FPS_60 ? 60 : (config.fps == FPS_30 ? 30 : 20));
        } else if (config.fps_enabled != FT_UNSUPPORTED) {
            snprintf(fps_buf, 16, "FPS: default");
        }

        // 1st line
        char res_buf[32] = "";
        if (config.fb_enabled == FT_ENABLED) {
            snprintf(res_buf, 32, "%dx%d",
                    config.fb.width,
                    config.fb.height);
        } else if (config.ib_enabled == FT_ENABLED) {
            if (config.ib_count == 1) {
                snprintf(res_buf, 32, "%dx%d",
                        config.ib[0].width,
                        config.ib[0].height);
            } else {
                snprintf(res_buf, 32, "%dx%d >> %dx%d",
                        config.ib[0].width,
                        config.ib[0].height,
                        config.ib[config.ib_count - 1].width,
                        config.ib[config.ib_count - 1].height);
            }
        } else if (config.fb_enabled != FT_UNSUPPORTED
                    || config.ib_enabled != FT_UNSUPPORTED) {
            snprintf(res_buf, 32, "Res: default");
        } else if (config.msaa_enabled == FT_ENABLED) {
            snprintf(res_buf, 32, "MSAA: %s", msaa_sm_buf);
        } else if (config.msaa_enabled != FT_UNSUPPORTED) {
            snprintf(res_buf, 16, "MSAA: default");
        }

        char res_line[64] = "";
        if (res_buf[0] != '\0') {
            if (config.msaa_enabled == FT_ENABLED
                    && (config.fb_enabled != FT_UNSUPPORTED
                    || config.ib_enabled != FT_UNSUPPORTED))
                snprintf(res_line, sizeof(res_line), "%s (%s)", res_buf, msaa_sm_buf);
            else
                snprintf(res_line, sizeof(res_line), "%s", res_buf);
        }

        // Resolution above FPS; a single line sits at the bottom
        vg_main_draw_overlay_box(vg_main_get_osd_width(), res_line[0] ? res_line : NULL, fps_buf[0] ? fps_buf : NULL);
    }

    return false;
}

static int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam, int sync) {
    // NULL (or a NULL base) is a legal call that blanks the display;
    // there is nothing to draw the OSD onto
    if (pParam == NULL || pParam->base == NULL || pParam->width == 0 || pParam->height == 0
            || pParam->pitch < pParam->width
            || (g_main.osd_done && !g_main.menu_ready))
        return TAI_CONTINUE(int, g_main.osd_hook_ref, pParam, sync);

    // The OSD has one set of state: if another thread is presenting
    // at the same time, this frame goes out without it
    bool menu_open = false;
    if (!__atomic_test_and_set(&g_main.osd_busy, __ATOMIC_ACQUIRE)) {
        menu_open = vg_main_draw_osd(pParam);
        __atomic_clear(&g_main.osd_busy, __ATOMIC_RELEASE);
    }

    int ret = TAI_CONTINUE(int, g_main.osd_hook_ref, pParam, sync);

    // Keep the menu on screen for a whole refresh
    if (menu_open) {
        sceDisplayWaitVblankStart();
    }

    return ret;
}

const char *vg_main_get_self_filename() {
    const char *self = strrchr(g_main.sce_info.path, '/');
    return self ? self + 1 : g_main.sce_info.path;
}

vg_module_match_t vg_main_match_current_module(const char titleid[], const char self[], uint32_t nid, bool exact) {
    if (strncasecmp(titleid, g_main.titleid, TITLEID_LEN) && (exact || strncasecmp(titleid, TITLEID_ANY, TITLEID_LEN)))
        return MODULE_TITLE_MISMATCH;
    if (exact ? strcmp(self, vg_main_get_self_filename()) : self[0] && !strstr(g_main.sce_info.path, self))
        return MODULE_SELF_MISMATCH;
    if (nid != g_main.tai_info.module_nid && (exact || nid != NID_ANY))
        return MODULE_NID_MISMATCH;
    return MODULE_MATCH;
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
    bool menu_wanted = false;

    g_main.osd_hook = -1;
    g_main.inject_num = 0;
    for (int i = 0; i < MAX_INJECT_NUM; i++) {
        g_main.inject[i] = -1;
    }
    for (int i = 0; i < MAX_HOOK_NUM; i++) {
        g_main.hook[i] = -1;
    }
    for (int i = 0; i < INPUT_HOOK_NUM; i++) {
        g_main.input_hook[i] = -1;
    }

    // Get app titleid
    sceAppMgrAppParamGetString(0, 12, g_main.titleid, 16);

    // Exit if using VitaShell
    if (!strncmp(g_main.titleid, "VITASHELL", TITLEID_LEN)) {
        goto EXIT;
    }

    // Get eboot.bin info
    g_main.tai_info.size = sizeof(tai_module_info_t);
    g_main.sce_info.size = sizeof(SceKernelModuleInfo);
    taiGetModuleInfo(TAI_MAIN_MODULE, &g_main.tai_info);
    sceKernelGetModuleInfo(g_main.tai_info.modid, &g_main.sce_info);

    // Create VitaGrafix folder (if doesn't exist)
    sceIoMkdir(VG_DIR, 0777);

    // Log basic info
    vg_log_printf("VitaGrafix " VG_VERSION "\n");
    vg_log_printf("=======================================\n");
    vg_log_printf("[MAIN] Title ID: %s\n", g_main.titleid);
    vg_log_printf("[MAIN] SELF: %s\n", g_main.sce_info.path);
    vg_log_printf("[MAIN] NID: 0x%08X\n", g_main.tai_info.module_nid);
    vg_log_printf("=======================================\n");

    // Parse config.txt
    vg_io_status_t config_status = vg_config_parse();
    vg_io_status_t patch_status = {IO_OK, 0, 0};
    vg_config_t *config = vg_config_get();

    if (config->log_enabled == FT_ENABLED) {
        vg_log_prepare();
        vg_log_set_enabled(true);
    }

    if (config_status.code != IO_OK) {
        config->enabled = FT_ENABLED;
        config->osd_enabled = FT_ENABLED;
        vg_log_printf("[PATCH] Failed to parse config (line %d, pos %d): %s\n",
                    config_status.line, config_status.pos_line, vg_io_status_code_to_string(config_status.code));
    }

    // Effective configuration, so a log always shows what was requested
    vg_log_printf("[CONFIG] ENABLED=%d OSD=%d FB=%d:%dx%d IB=%d:%d#%dx%d..%dx%d FPS=%d:%d MSAA=%d:%d\n",
                config->enabled, config->osd_enabled,
                config->fb_enabled, config->fb.width, config->fb.height,
                config->ib_enabled, config->ib_count, config->ib[0].width, config->ib[0].height,
                config->ib[config->ib_count > 0 ? config->ib_count - 1 : 0].width,
                config->ib[config->ib_count > 0 ? config->ib_count - 1 : 0].height,
                config->fps_enabled, config->fps, config->msaa_enabled, config->msaa);

    // Exit now?
    if (config->enabled == FT_DISABLED)
        goto EXIT;

    // Skip parsing patchlist if there was an error in config
    if (config_status.code != IO_OK)
        goto EXIT_HOOK_OSD;

    // Parse patchlist & apply patches
    patch_status = vg_patch_parse_and_apply();
    if (patch_status.code != IO_OK) {
        config->osd_enabled = FT_ENABLED;
        vg_log_printf("[PATCH] Failed to parse patchlist (line %d, pos %d): %s\n",
                    patch_status.line, patch_status.pos_line, vg_io_status_code_to_string(patch_status.code));
        goto EXIT_HOOK_OSD;
    }

    // Exit if game is not supported / is self shell
    if (g_main.patch_match == MODULE_SELF_MISMATCH || g_main.patch_match == MODULE_TITLE_MISMATCH)
        goto EXIT;

    // In-game menu, for a game this patch matches that has something to configure
    menu_wanted = config->osd_enabled == FT_ENABLED && g_main.patch_match == MODULE_MATCH && vg_menu_init();

EXIT_HOOK_OSD:
    // Hook sceDisplaySetFrameBuf for OSD
    if (config->osd_enabled == FT_ENABLED) {
        g_main.osd_timer = 0;
        g_main.osd_hook = taiHookFunctionImport(
                    &g_main.osd_hook_ref,
                    TAI_MAIN_MODULE,
                    TAI_ANY_LIBRARY,
                    0x7A410B64,
                    sceDisplaySetFrameBuf_patched);
        g_main.osd_done = false;
        vg_log_printf("[MAIN] OSD hook on sceDisplaySetFrameBuf: 0x%X (match=%d)\n", g_main.osd_hook, g_main.patch_match);

        if (g_main.osd_hook >= 0 && menu_wanted) {
            int hooked = 0;
            for (int i = 0; i < INPUT_HOOK_NUM; i++) {
                g_main.input_hook[i] = taiHookFunctionImport(&g_main.input_hook_ref[i], TAI_MAIN_MODULE,
                        TAI_ANY_LIBRARY, g_input_hooks[i].nid, g_input_hooks[i].func);
                if (g_main.input_hook[i] >= 0)
                    hooked++;
            }
            // Without a single input hook the game would act on everything done in the menu
            g_main.menu_ready = hooked > 0;
            vg_log_printf("[MAIN] Menu %s, %d input hooks\n", hooked > 0 ? "ready" : "unavailable", hooked);
        }

        if (config_status.code != IO_OK || patch_status.code != IO_OK) {
            vg_log_read(g_osd_buffer, STRING_BUFFER_SIZE);
        }
    }

EXIT:
    vg_log_flush();
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    // A menu save may still be writing the config
    vg_config_save_wait();

    // Release OSD hook
    if (g_main.osd_hook >= 0) {
        taiHookRelease(g_main.osd_hook, g_main.osd_hook_ref);
    }

    // Release input hooks
    for (int i = 0; i < INPUT_HOOK_NUM; i++) {
        if (g_main.input_hook[i] >= 0) {
            taiHookRelease(g_main.input_hook[i], g_main.input_hook_ref[i]);
        }
    }

    // Release game patches
    for (uint32_t i = g_main.inject_num; i > 0; i--) {
        if (g_main.inject[i - 1] >= 0)
            taiInjectRelease(g_main.inject[i - 1]);
    }
    g_main.inject_num = 0;

    // Release game hooks: we need to loop the whole array since hooks are indexed by their id
    for (uint8_t i = MAX_HOOK_NUM; i > 0; i--) {
        if (g_main.hook[i - 1] >= 0)
            taiHookRelease(g_main.hook[i - 1], g_main.hook_ref[i - 1]);
    }

    // The shader hook is gone: its lock can go too
    vg_gxp_uninstall();

    return SCE_KERNEL_STOP_SUCCESS;
}
