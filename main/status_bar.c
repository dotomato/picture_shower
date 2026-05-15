/**
 * @file status_bar.c
 * @brief Status Bar implementation (FPS | RAM | Battery)
 */
#include "status_bar.h"
#include "axp2101.h"

#include "lvgl.h"
#include "esp_heap_caps.h"

static lv_obj_t   *s_status_bar = NULL;    /* container panel */
static lv_obj_t   *s_fps_label  = NULL;
static lv_obj_t   *s_ram_label  = NULL;
static lv_obj_t   *s_bat_label  = NULL;
static uint32_t    s_fps_refr_cnt  = 0;
static uint32_t    s_fps_last_tick = 0;

static void fps_refr_event_cb(lv_event_t *e)
{
    (void)e;
    s_fps_refr_cnt++;
}

static void status_bar_update_cb(lv_timer_t *timer)
{
    (void)timer;

    /* --- FPS --- */
    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - s_fps_last_tick;
    if (elapsed > 0) {
        uint32_t fps = (s_fps_refr_cnt * 1000) / elapsed;
        uint32_t max_fps = 1000 / LV_DEF_REFR_PERIOD;
        if (fps > max_fps) fps = max_fps;
        lv_label_set_text_fmt(s_fps_label, "%" LV_PRIu32 " FPS", fps);
        s_fps_refr_cnt = 0;
        s_fps_last_tick = now;
    }

    /* --- Free RAM --- */
    uint32_t int_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    lv_label_set_text_fmt(s_ram_label, "RAM %" LV_PRIu32 "K|%" LV_PRIu32 "K",
                          int_free / 1024, psram_free / 1024);

    /* --- Battery --- */
    int bat = battery_get_percent();
    bool charging = battery_is_charging();
    if (bat < 0) {
        /* AXP2101 not responding or not initialized */
        lv_label_set_text(s_bat_label, LV_SYMBOL_USB);
    } else if (charging) {
        lv_label_set_text_fmt(s_bat_label, LV_SYMBOL_CHARGE " %d%%", bat);
    } else {
        const char *icon = (bat > 75) ? LV_SYMBOL_BATTERY_FULL :
                           (bat > 50) ? LV_SYMBOL_BATTERY_3 :
                           (bat > 25) ? LV_SYMBOL_BATTERY_2 :
                           (bat > 5)  ? LV_SYMBOL_BATTERY_1 :
                                        LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(s_bat_label, "%s %d%%", icon, bat);
    }
}

void status_bar_create(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) return;

    lv_obj_t *sys_layer = lv_display_get_layer_sys(disp);

    /* --- Container (horizontal row, semi-transparent dark pill) --- */
    s_status_bar = lv_obj_create(sys_layer);
    lv_obj_remove_style_all(s_status_bar);
    lv_obj_set_size(s_status_bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_status_bar, lv_color_black(), 0);
    lv_obj_set_style_radius(s_status_bar, 14, 0);
    lv_obj_set_style_pad_hor(s_status_bar, 12, 0);
    lv_obj_set_style_pad_ver(s_status_bar, 4, 0);
    lv_obj_set_style_pad_column(s_status_bar, 10, 0);
    lv_obj_set_layout(s_status_bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_status_bar, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_remove_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Shared label style: small green-on-transparent */
    #define SBAR_FONT &lv_font_montserrat_12

    /* FPS label */
    s_fps_label = lv_label_create(s_status_bar);
    lv_obj_set_style_text_color(s_fps_label, lv_color_make(0x00, 0xFF, 0x80), 0);
    lv_obj_set_style_text_font(s_fps_label, SBAR_FONT, 0);
    lv_label_set_text(s_fps_label, "-- FPS");

    /* Separator */
    lv_obj_t *sep1 = lv_label_create(s_status_bar);
    lv_obj_set_style_text_color(sep1, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_set_style_text_font(sep1, SBAR_FONT, 0);
    lv_label_set_text(sep1, "|");

    /* RAM label */
    s_ram_label = lv_label_create(s_status_bar);
    lv_obj_set_style_text_color(s_ram_label, lv_color_make(0x80, 0xD0, 0xFF), 0);
    lv_obj_set_style_text_font(s_ram_label, SBAR_FONT, 0);
    lv_label_set_text(s_ram_label, "RAM --K|--K");

    /* Separator */
    lv_obj_t *sep2 = lv_label_create(s_status_bar);
    lv_obj_set_style_text_color(sep2, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_set_style_text_font(sep2, SBAR_FONT, 0);
    lv_label_set_text(sep2, "|");

    /* Battery label */
    s_bat_label = lv_label_create(s_status_bar);
    lv_obj_set_style_text_color(s_bat_label, lv_color_make(0xFF, 0xE0, 0x40), 0);
    lv_obj_set_style_text_font(s_bat_label, SBAR_FONT, 0);
    lv_label_set_text(s_bat_label, LV_SYMBOL_USB);

    /* Register event callback to count refreshes */
    lv_display_add_event_cb(disp, fps_refr_event_cb, LV_EVENT_REFR_READY, NULL);

    /* Timer to update all fields every 500ms */
    s_fps_last_tick = lv_tick_get();
    lv_timer_create(status_bar_update_cb, 500, NULL);
}
