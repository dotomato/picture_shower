/**
 * @file touch_input.c
 * @brief Touch input handler for image navigation
 *
 * Detects tap gestures on the touch screen to switch images.
 * - Tap on the right half of the screen: next image
 * - Tap on the left half of the screen: previous image
 */
#include "touch_input.h"
#include "ui_screens.h"

#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "touch_input";

#define SCREEN_SIZE         480
#define TAP_TIMEOUT_MS      300   /* Max duration for a tap (not a long press) */
#define TAP_MOVE_THRESHOLD  30    /* Max movement in px to still count as a tap */

/* State machine for tap detection */
static bool     s_touch_active = false;
static int32_t  s_touch_start_x = 0;
static int32_t  s_touch_start_y = 0;
static uint32_t s_touch_start_time = 0;

/**
 * LVGL input event callback on the active screen.
 * We use an invisible overlay object to capture all touch events.
 */
static lv_obj_t *s_touch_overlay = NULL;

static void touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        s_touch_active = true;
        s_touch_start_x = point.x;
        s_touch_start_y = point.y;
        s_touch_start_time = lv_tick_get();
    }
    else if (code == LV_EVENT_RELEASED) {
        if (!s_touch_active) return;
        s_touch_active = false;

        uint32_t elapsed = lv_tick_elaps(s_touch_start_time);
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);

        /* Check if it qualifies as a tap (short duration, small movement) */
        int32_t dx = point.x - s_touch_start_x;
        int32_t dy = point.y - s_touch_start_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;

        if (elapsed < TAP_TIMEOUT_MS && dx < TAP_MOVE_THRESHOLD && dy < TAP_MOVE_THRESHOLD) {
            /* It's a tap! Determine which half of the screen */
            if (s_touch_start_x > SCREEN_SIZE / 2) {
                ESP_LOGI(TAG, "Tap right -> next image");
                ui_next_image();
            } else {
                ESP_LOGI(TAG, "Tap left -> prev image");
                ui_prev_image();
            }
        }
    }
}

/**
 * Create an invisible full-screen overlay on sys_layer to capture touch events.
 * Using sys_layer ensures it persists across screen changes.
 */
void touch_input_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        ESP_LOGE(TAG, "No display available");
        return;
    }

    /* Create overlay on top layer (above normal screen, below sys_layer ball) */
    lv_obj_t *top_layer = lv_display_get_layer_top(disp);

    s_touch_overlay = lv_obj_create(top_layer);
    lv_obj_remove_style_all(s_touch_overlay);
    lv_obj_set_size(s_touch_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_pos(s_touch_overlay, 0, 0);

    /* Make it fully transparent (invisible) but clickable */
    lv_obj_set_style_bg_opa(s_touch_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_touch_overlay, 0, 0);
    lv_obj_clear_flag(s_touch_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_touch_overlay, LV_OBJ_FLAG_CLICKABLE);

    /* Register touch events */
    lv_obj_add_event_cb(s_touch_overlay, touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_touch_overlay, touch_event_cb, LV_EVENT_RELEASED, NULL);

    ESP_LOGI(TAG, "Touch input initialized (tap left=prev, tap right=next)");
}
