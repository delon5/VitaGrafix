#include <vitasdk.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "menu.h"
#include "osd.h"

#define MENU_HEADER_ROWS 3

// The menu only exists while the game presents frames. If it stops (a loading
// screen, or a game that only draws after input), the game gets its input back.
#define MENU_STALE_AFTER 500000

// Bits that report a state rather than a button, the game keeps seeing them
#define MENU_CTRL_STATUS (SCE_CTRL_INTERCEPTED | SCE_CTRL_HEADPHONE)

static const char *const g_menu_legend[] = {
    "UP/DOWN select  LEFT/RIGHT change",
    "CROSS on/off  START save and close",
    "SELECT+R close without saving",
};
#define MENU_LEGEND_COUNT (sizeof(g_menu_legend) / sizeof(g_menu_legend[0]))

static vg_menu_t g_menu = {0};

bool vg_menu_init() {
    const vg_config_t *config = vg_config_get();

    // Prepare rows that will be rendered based on feature availability
    g_menu.row_count = 0;
    g_menu.item_count = 0;

    if (vg_config_is_feature_supported(FEATURE_FB)) {
        g_menu.rows[g_menu.row_count++] = MENU_ROW_FB;
        g_menu.items[g_menu.item_count++] = MENU_ITEM_FB;
    }
    if (vg_config_is_feature_supported(FEATURE_IB)) {
        g_menu.rows[g_menu.row_count++] = MENU_ROW_IB;
        g_menu.items[g_menu.item_count++] = MENU_ITEM_IB;
    }
    if (vg_config_is_feature_supported(FEATURE_FPS)) {
        g_menu.rows[g_menu.row_count++] = MENU_ROW_FPS;
        g_menu.items[g_menu.item_count++] = MENU_ITEM_FPS;
    }
    if (vg_config_is_feature_supported(FEATURE_MSAA)) {
        g_menu.rows[g_menu.row_count++] = MENU_ROW_MSAA;
        g_menu.items[g_menu.item_count++] = MENU_ITEM_MSAA;
    }

    if (g_menu.item_count > 0) {
        g_menu.selected_item = g_menu.items[0];
    } else {
        g_menu.selected_item = MENU_ITEM_INVALID;
    }

    g_menu.snapshot = *config;
    g_menu.previous_buttons = 0;
    g_menu.input_block = 0;
    g_menu.last_frame = 0;
    g_menu.open = false;
    g_menu.notice = MENU_NOTICE_NONE;

    return g_menu.item_count > 0;
}

bool vg_menu_is_open() {
    return g_menu.open;
}

vg_menu_notice_t vg_menu_get_notice() {
    return g_menu.notice;
}

// What the input hooks hide from the game now: 0, MENU_BLOCK_ALL or held buttons
static uint32_t vg_menu_input_block() {
    uint32_t block = g_menu.input_block;
    if (!block)
        return 0;

    // Signed: a frame stamped after this thread read the clock is not stale; and far
    // in either direction (no frame for over half the 32-bit clock) is
    SceInt32 age = sceKernelGetProcessTimeLow() - g_menu.last_frame;
    if (age >= MENU_STALE_AFTER || age <= -MENU_STALE_AFTER)
        return 0;

    return block;
}

// The "2" reads report the L/R triggers as L1/R1
static uint32_t vg_menu_alias_triggers(uint32_t buttons) {
    return buttons
        | (buttons & SCE_CTRL_LTRIGGER ? SCE_CTRL_L1 : 0)
        | (buttons & SCE_CTRL_RTRIGGER ? SCE_CTRL_R1 : 0);
}

void vg_menu_filter_ctrl(SceCtrlData *ctrl, int count, bool negative) {
    if (ctrl == NULL || count <= 0)
        return;

    uint32_t block = vg_menu_input_block();
    if (!block)
        return;

    for (int i = 0; i < count; i++) {
        // Negative reads report a pressed button as a cleared bit
        if (block == MENU_BLOCK_ALL) {
            ctrl[i].buttons = negative ? ctrl[i].buttons | ~MENU_CTRL_STATUS : ctrl[i].buttons & MENU_CTRL_STATUS;
            ctrl[i].lx = ctrl[i].ly = ctrl[i].rx = ctrl[i].ry = 128;
            memset(&ctrl[i].up, 0, offsetof(SceCtrlData, reserved) - offsetof(SceCtrlData, up));
        } else {
            ctrl[i].buttons = negative ? ctrl[i].buttons | block : ctrl[i].buttons & ~block;
        }
    }
}

void vg_menu_filter_touch(SceTouchData *data, int count) {
    if (data == NULL || count <= 0 || vg_menu_input_block() != MENU_BLOCK_ALL)
        return;

    for (int i = 0; i < count; i++) {
        data[i].reportNum = 0;
    }
}

static int vg_menu_adjust_value(int value, int direction, int minimum, int maximum, int step) {
    int amount = (direction < 0 ? -direction : direction) * step;

    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    if (direction < 0) {
        return value - amount > minimum ? value - amount : minimum;
    }
    if (amount >= maximum - value) {
        return maximum;
    }
    return value + amount;
}

// The IB choices: the framebuffer sizes, largest first, plus the value the config had
// when the menu opened if it is none of them (in size order, so it can be picked again)
static int vg_menu_get_ib_choices(vg_res_t choices[FRAMEBUFFER_RESOLUTION_COUNT + 1]) {
    vg_res_t custom = g_menu.snapshot.ib[0];
    bool custom_listed = false;
    int count = 0;

    for (int i = 0; i < FRAMEBUFFER_RESOLUTION_COUNT; i++) {
        vg_res_t preset = vg_config_framebuffer_resolutions[i];
        if (preset.width == custom.width && preset.height == custom.height)
            custom_listed = true;
    }

    for (int i = 0; i < FRAMEBUFFER_RESOLUTION_COUNT; i++) {
        vg_res_t preset = vg_config_framebuffer_resolutions[i];
        if (!custom_listed && (uint32_t)custom.width * custom.height >= (uint32_t)preset.width * preset.height) {
            choices[count++] = custom;
            custom_listed = true;
        }
        choices[count++] = preset;
    }
    if (!custom_listed) {
        choices[count++] = custom;
    }

    return count;
}

// The choice the config is on now, or the closest one by size
static int vg_menu_find_ib_choice(const vg_res_t choices[], int count, const vg_res_t *res) {
    int index = 0;
    int64_t best = -1;

    for (int i = 0; i < count; i++) {
        if (choices[i].width == res->width && choices[i].height == res->height)
            return i;

        int64_t difference = (int64_t)((uint32_t)choices[i].width * choices[i].height)
            - (int64_t)((uint32_t)res->width * res->height);
        if (difference < 0)
            difference = -difference;
        if (best < 0 || difference < best) {
            best = difference;
            index = i;
        }
    }

    return index;
}

static void vg_menu_move_selected_item_value(int direction) {
    vg_config_t *config = vg_config_get();

    // LEFT / RIGHT on an Off row turns it on at the value it shows,
    // the next press changes that value
    switch (g_menu.selected_item) {
        case MENU_ITEM_FB: {
            if (!vg_config_is_feature_enabled(FEATURE_FB)) {
                config->fb_enabled = FT_ENABLED;
                break;
            }

            int index = 0;

            for (int i = 0; i < FRAMEBUFFER_RESOLUTION_COUNT; i++) {
                if (config->fb.width == vg_config_framebuffer_resolutions[i].width
                        && config->fb.height == vg_config_framebuffer_resolutions[i].height) {
                    index = i;
                    break;
                }
            }

            index = vg_menu_adjust_value(index, -direction, 0,
                    FRAMEBUFFER_RESOLUTION_COUNT - 1, 1);

            config->fb = vg_config_framebuffer_resolutions[index];
            break;
        }

        case MENU_ITEM_FPS: {
            if (!vg_config_is_feature_enabled(FEATURE_FPS)) {
                config->fps_enabled = FT_ENABLED;
                break;
            }

            int index = config->fps == FPS_20 ? 0 : config->fps == FPS_30 ? 1 : 2;
            index = vg_menu_adjust_value(index, direction, 0, 2, 1);

            config->fps = index == 0 ? FPS_20 : index == 1 ? FPS_30 : FPS_60;
            break;
        }

        case MENU_ITEM_MSAA: {
            if (!vg_config_is_feature_enabled(FEATURE_MSAA)) {
                config->msaa_enabled = FT_ENABLED;
                break;
            }

            int index = config->msaa == MSAA_NONE ? 0 : config->msaa == MSAA_2X ? 1 : 2;
            index = vg_menu_adjust_value(index, direction, 0, 2, 1);

            config->msaa = index == 0 ? MSAA_NONE : index == 1 ? MSAA_2X : MSAA_4X;
            break;
        }

        case MENU_ITEM_IB: {
            if (!vg_config_is_feature_enabled(FEATURE_IB)) {
                config->ib_enabled = FT_ENABLED;
                break;
            }

            // One resolution for every internal resolution the patch uses
            vg_res_t choices[FRAMEBUFFER_RESOLUTION_COUNT + 1];
            int count = vg_menu_get_ib_choices(choices);

            int index = vg_menu_find_ib_choice(choices, count, &config->ib[0]);
            index = vg_menu_adjust_value(index, -direction, 0, count - 1, 1);

            config->ib[0] = choices[index];
            config->ib_count = 1;
            vg_config_propagate_ib();
            break;
        }

        default:
            break;
    }
}

static void vg_menu_move_selected_item(int direction) {
    int selected = 0;
    for (int i = 0; i < g_menu.item_count; i++) {
        if (g_menu.items[i] == g_menu.selected_item) {
            selected = i;
            break;
        }
    }

    g_menu.selected_item = g_menu.items[(selected + direction + g_menu.item_count) % g_menu.item_count];
}

static void vg_menu_open() {
    g_menu.snapshot = *vg_config_get();
    g_menu.selected_item = g_menu.items[0];
    g_menu.open = true;
    g_menu.input_block = MENU_BLOCK_ALL;
}

static void vg_menu_close(uint32_t buttons, bool keep_changes) {
    if (!keep_changes) {
        *vg_config_get() = g_menu.snapshot;
    }

    // The buttons that closed the menu (START, or SELECT + R) would reach the
    // game on the next read, hide them until they are released
    g_menu.open = false;
    g_menu.input_block = vg_menu_alias_triggers(buttons) & ~MENU_CTRL_STATUS;
}

// Returns true when a notice should be shown
static bool vg_menu_check_input(const SceCtrlData *ctrl) {
    vg_config_t *config = vg_config_get();

    uint32_t buttons = ctrl->buttons;
    uint32_t pressed = buttons & ~g_menu.previous_buttons;
    g_menu.previous_buttons = buttons;

    if (!g_menu.open && g_menu.input_block) {
        g_menu.input_block &= vg_menu_alias_triggers(buttons);
    }

    // SELECT + RIGHT TRIGGER: Toggle menu. Closing it shows the last save's
    // result again (it may have arrived while the menu was open)
    if ((buttons & SCE_CTRL_SELECT) && (pressed & SCE_CTRL_RTRIGGER)) {
        if (g_menu.open) {
            vg_menu_close(buttons, false);
            return g_menu.notice != MENU_NOTICE_NONE;
        } else if (g_menu.item_count > 0) {
            vg_menu_open();
        }
        return false;
    }

    if (!g_menu.open) {
        return false;
    }

    // D-PAD UP / DOWN: Move between menu items
    if (pressed & SCE_CTRL_UP) {
        vg_menu_move_selected_item(-1);
        return false;
    }
    if (pressed & SCE_CTRL_DOWN) {
        vg_menu_move_selected_item(1);
        return false;
    }

    // D-PAD LEFT / RIGHT: Change item value
    if (pressed & SCE_CTRL_LEFT) {
        vg_menu_move_selected_item_value(-1);
        return false;
    }
    if (pressed & SCE_CTRL_RIGHT) {
        vg_menu_move_selected_item_value(1);
        return false;
    }

    // CROSS: Toggle selected item on/off (an IB list keeps all of its slots)
    if (pressed & SCE_CTRL_CROSS) {
        switch (g_menu.selected_item) {
            case MENU_ITEM_FB:
                config->fb_enabled = config->fb_enabled == FT_ENABLED ? FT_DISABLED : FT_ENABLED;
                break;

            case MENU_ITEM_IB:
                config->ib_enabled = config->ib_enabled == FT_ENABLED ? FT_DISABLED : FT_ENABLED;
                break;

            case MENU_ITEM_FPS:
                config->fps_enabled = config->fps_enabled == FT_ENABLED ? FT_DISABLED : FT_ENABLED;
                break;

            case MENU_ITEM_MSAA:
                config->msaa_enabled = config->msaa_enabled == FT_ENABLED ? FT_DISABLED : FT_ENABLED;
                break;

            default:
                break;
        }

        return false;
    }

    // START: Save config (on a worker thread) and close
    if (pressed & SCE_CTRL_START) {
        if (vg_config_save_get_state() == CONFIG_SAVE_RUNNING) {
            return false;
        }

        g_menu.notice = vg_config_save_start() ? MENU_NOTICE_SAVING : MENU_NOTICE_SAVE_FAILED;
        vg_menu_close(buttons, true);
        return true;
    }

    return false;
}

bool vg_menu_update(const SceCtrlData *ctrl) {
    bool show_notice = false;

    g_menu.last_frame = sceKernelGetProcessTimeLow();

    // A save finished, show how it went
    if (g_menu.notice == MENU_NOTICE_SAVING) {
        vg_config_save_state_t state = vg_config_save_get_state();
        if (state != CONFIG_SAVE_RUNNING) {
            g_menu.notice = state == CONFIG_SAVE_OK ? MENU_NOTICE_SAVED : MENU_NOTICE_SAVE_FAILED;
            show_notice = true;
        }
    }

    if (ctrl != NULL && vg_menu_check_input(ctrl)) {
        show_notice = true;
    }

    return show_notice;
}

static void vg_menu_draw_label(int x, int y, const char *label) {
    osd_set_text_color(255, 255, 255, 255);
    osd_draw_string(x, y, label);
}

static void vg_menu_draw_value(int x, int y, const char *value, bool selected) {
    osd_set_text_color(selected ? 255 : 230, selected ? 210 : 230, selected ? 64 : 230, 255);
    osd_draw_string(x, y, value);
}

void vg_menu_draw() {
    if (!g_menu.open) {
        return;
    }

    const vg_config_t *config = vg_config_get();

    int line_height = osd_get_text_height() + 4;

    int legend_count = MENU_LEGEND_COUNT;

    int width = 0;
    for (int i = 0; i < legend_count; i++) {
        int legend_width = osd_get_text_width(g_menu_legend[i]);
        if (legend_width > width)
            width = legend_width;
    }
    width += 28;
    int height = (g_menu.row_count + MENU_HEADER_ROWS + legend_count) * line_height + 20;

    int x = (960 - width) / 2;
    int y = (544 - height) / 2;

    int label_x = x + 14;
    int value_x = x + 14 + osd_get_text_width("MSAA:") + 22; // account for the widest label
    osd_set_back_color(0, 0, 0, 255);
    osd_draw_rounded_rectangle(x, y, width, height, 7);

    // Text over the box needs no background of its own (an opaque one would
    // cut into the line above where lines overlap by a pixel)
    osd_set_back_color(0, 0, 0, 0);

    osd_set_text_color(255, 255, 255, 255);
    osd_draw_string(x + 14, y + 10, "VitaGrafix " VG_VERSION);

    osd_set_text_color(180, 180, 180, 255);

    const char *self = vg_main_get_self_filename();
    int self_length = strlen(self);
    if (self_length > SELF_LEN_MAX) {
        self_length = SELF_LEN_MAX;
    }
    char title_info[64];
    do {
        snprintf(title_info, sizeof(title_info), "%s / %.*s", g_main.titleid, self_length, self);
        self_length--;
    } while (self_length >= 0 && osd_get_text_width(title_info) > width - 28);
    osd_draw_string(x + 14, y + 10 + line_height, title_info);

    char nid_info[32];
    snprintf(nid_info, sizeof(nid_info), "Fingerprint: 0x%08X", g_main.tai_info.module_nid);
    osd_draw_string(x + 14, y + 10 + 2 * line_height - 4, nid_info);

    for (int i = 0; i < g_menu.row_count; i++) {
        int row_y = y + 10 + (i + MENU_HEADER_ROWS) * line_height;
        int row_value_x = value_x;

        vg_menu_row_t row = g_menu.rows[i];

        if (row == MENU_ROW_FB) {
            vg_menu_draw_label(label_x, row_y, "FB:");

            bool selected = g_menu.selected_item == MENU_ITEM_FB;
            char value[24];
            if (vg_config_is_feature_enabled(FEATURE_FB)) {
                snprintf(value, sizeof(value), selected ? "< %dx%d >" : "%dx%d", config->fb.width, config->fb.height);
            } else {
                snprintf(value, sizeof(value), selected ? "< Off >" : "Off");
            }

            if (!selected) {
                row_value_x = osd_get_text_end_x(row_value_x, "< ");
            }
            vg_menu_draw_value(row_value_x, row_y, value, selected);

            continue;
        }

        if (row == MENU_ROW_IB) {
            vg_menu_draw_label(label_x, row_y, "IB:");

            bool selected = g_menu.selected_item == MENU_ITEM_IB;
            const vg_res_t *res = &config->ib[0];
            char value[24];
            if (vg_config_is_feature_enabled(FEATURE_IB)) {
                snprintf(value, sizeof(value), selected ? "< %dx%d >" : "%dx%d", res->width, res->height);
            } else {
                snprintf(value, sizeof(value), selected ? "< Off >" : "Off");
            }

            if (!selected) {
                row_value_x = osd_get_text_end_x(row_value_x, "< ");
            }
            vg_menu_draw_value(row_value_x, row_y, value, selected);

            continue;
        }

        if (row == MENU_ROW_FPS) {
            vg_menu_draw_label(label_x, row_y, "FPS:");

            bool selected = g_menu.selected_item == MENU_ITEM_FPS;
            const char *value = !vg_config_is_feature_enabled(FEATURE_FPS) ? "Off" :
                    config->fps == FPS_20 ? "20" : config->fps == FPS_30 ? "30" : "60";
            char display_value[16];
            snprintf(display_value, sizeof(display_value), selected ? "< %s >" : "%s", value);
            if (!selected) {
                row_value_x = osd_get_text_end_x(row_value_x, "< ");
            }
            vg_menu_draw_value(row_value_x, row_y, display_value, selected);

            continue;
        }

        if (row == MENU_ROW_MSAA) {
            vg_menu_draw_label(label_x, row_y, "MSAA:");

            // "Off" leaves the game's own setting, "1x" turns multisampling off
            bool selected = g_menu.selected_item == MENU_ITEM_MSAA;
            const char *value = !vg_config_is_feature_enabled(FEATURE_MSAA) ? "Off" :
                    config->msaa == MSAA_NONE ? "1x" : config->msaa == MSAA_2X ? "2x" : "4x";
            char display_value[16];
            snprintf(display_value, sizeof(display_value), selected ? "< %s >" : "%s", value);
            if (!selected) {
                row_value_x = osd_get_text_end_x(row_value_x, "< ");
            }
            vg_menu_draw_value(row_value_x, row_y, display_value, selected);

            continue;
        }
    }

    osd_set_text_color(180, 180, 180, 255);
    for (int i = 0; i < legend_count; i++) {
        osd_draw_string(x + 14, y + height - (legend_count - i) * line_height - 4, g_menu_legend[i]);
    }
}
