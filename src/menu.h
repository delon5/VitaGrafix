#ifndef _MENU_H_
#define _MENU_H_

#include <stdbool.h>
#include <vitasdk.h>

#include "config.h"

typedef enum {
    MENU_ROW_FB,
    MENU_ROW_IB,
    MENU_ROW_FPS,
    MENU_ROW_MSAA,
    MENU_ROW_INVALID
} vg_menu_row_t;

typedef enum {
    MENU_ITEM_FB,
    MENU_ITEM_IB,
    MENU_ITEM_FPS,
    MENU_ITEM_MSAA,
    MENU_ITEM_INVALID
} vg_menu_item_t;

typedef enum {
    MENU_NOTICE_NONE,
    MENU_NOTICE_SAVING,
    MENU_NOTICE_SAVED,
    MENU_NOTICE_SAVE_FAILED
} vg_menu_notice_t;

// vg_menu_t.input_block while the menu is open: the game gets no input at all
#define MENU_BLOCK_ALL 0xFFFFFFFF

typedef struct {
    // Read by the input hooks on the game's threads, in one word so a read is never
    // half old and half new: 0, MENU_BLOCK_ALL, or the buttons that were held when
    // the menu closed, hidden from the game until released
    volatile uint32_t input_block;
    volatile SceUInt32 last_frame;     // when the display hook last ran

    // Everything else belongs to the display hook
    bool open;
    uint32_t previous_buttons;

    vg_menu_row_t rows[MENU_ROW_INVALID];
    int row_count;

    vg_menu_item_t items[MENU_ITEM_INVALID];
    int item_count;

    vg_menu_item_t selected_item;

    // Configuration when the menu was opened: restored when it closes without saving,
    // and an IB setting from it that is not one of the presets stays selectable
    vg_config_t snapshot;

    vg_menu_notice_t notice;
} vg_menu_t;

bool vg_menu_init();
bool vg_menu_is_open();
vg_menu_notice_t vg_menu_get_notice();
bool vg_menu_update(const SceCtrlData *ctrl);
void vg_menu_draw();

// Input filters for the game's controller and touch reads
void vg_menu_filter_ctrl(SceCtrlData *ctrl, int count, bool negative);
void vg_menu_filter_touch(SceTouchData *data, int count);

#endif
