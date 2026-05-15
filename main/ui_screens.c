/**
 * @file ui_screens.c
 * @brief UI screens: init screen, image display with tile-reveal mechanic
 *
 * The image is displayed centered and covered by a grid of semi-transparent
 * dark tiles. When the gravity ball passes over a tile, that tile fades out
 * to reveal the original image underneath.
 */
#include "ui_screens.h"
#include "network.h"
#include "gravity_ball.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"
#include "string.h"
#include "stdio.h"
#include "errno.h"
#include <math.h>

static const char *TAG = "ui";

/* Init screen status label */
static lv_obj_t *s_status_label = NULL;

/* Pending log message buffer (written by net_task, read by LVGL task) */
#define UI_LOG_BUF_SIZE 128
static char s_ui_log_buf[UI_LOG_BUF_SIZE];

/* Image display state */
static int s_pic_index = 0;
static bool s_slideshow_active = false;

/* Tile grid configuration */
#define TILE_COLS       8
#define TILE_ROWS       8
#define TILE_COUNT      (TILE_COLS * TILE_ROWS)
#define SCREEN_SIZE     480
#define TILE_W          (SCREEN_SIZE / TILE_COLS)   /* 60 px */
#define TILE_H          (SCREEN_SIZE / TILE_ROWS)   /* 60 px */
#define TILE_DARK_OPA   200   /* Opacity of dark overlay (0=transparent, 255=opaque) */

/* Tile state */
static lv_obj_t *s_tiles[TILE_COUNT];
static bool      s_tile_revealed[TILE_COUNT];
static lv_timer_t *s_reveal_timer = NULL;

/* Current image object */
static lv_obj_t *s_current_img = NULL;

/* Hardware JPEG decode buffer (PSRAM): reused across slides */
static uint8_t          *s_jpeg_rgb_buf  = NULL;
static size_t            s_jpeg_rgb_size = 0;
static lv_image_dsc_t    s_img_dsc;

/* -------------------------------------------------------
 * UI Log (async callback)
 * ------------------------------------------------------- */
static void ui_log_async_cb(void *arg)
{
    if (s_status_label == NULL) return;
    char new_text[512];
    const char *cur = lv_label_get_text(s_status_label);
    if (cur && strlen(cur) > 0) {
        snprintf(new_text, sizeof(new_text), "%s\n%s", cur, (char *)arg);
    } else {
        snprintf(new_text, sizeof(new_text), "%s", (char *)arg);
    }
    lv_label_set_text(s_status_label, new_text);
}

void ui_log(const char *msg)
{
    strlcpy(s_ui_log_buf, msg, UI_LOG_BUF_SIZE);
    lv_async_call(ui_log_async_cb, s_ui_log_buf);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* -------------------------------------------------------
 * Tile reveal logic
 * ------------------------------------------------------- */

/* Animation helper: set bg_opa on object */
static void lv_obj_set_style_bg_opa_anim_cb(void *obj, int32_t val)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

/**
 * LVGL timer callback: check ball position against tile grid,
 * reveal tiles that the ball overlaps with.
 */
static void tile_reveal_timer_cb(lv_timer_t *timer)
{
    if (!s_slideshow_active) return;

    float ball_x, ball_y;
    gravity_ball_get_position(&ball_x, &ball_y);
    int ball_r = gravity_ball_get_radius();

    /* Check each tile for overlap with the ball circle */
    for (int row = 0; row < TILE_ROWS; row++) {
        for (int col = 0; col < TILE_COLS; col++) {
            int idx = row * TILE_COLS + col;
            if (s_tile_revealed[idx]) continue;

            /* Tile rectangle bounds */
            int tile_x1 = col * TILE_W;
            int tile_y1 = row * TILE_H;
            int tile_x2 = tile_x1 + TILE_W;
            int tile_y2 = tile_y1 + TILE_H;

            /* Find closest point on tile rect to ball center */
            float closest_x = ball_x;
            float closest_y = ball_y;
            if (closest_x < tile_x1) closest_x = tile_x1;
            else if (closest_x > tile_x2) closest_x = tile_x2;
            if (closest_y < tile_y1) closest_y = tile_y1;
            else if (closest_y > tile_y2) closest_y = tile_y2;

            /* Distance from ball center to closest point */
            float dx = ball_x - closest_x;
            float dy = ball_y - closest_y;
            float dist_sq = dx * dx + dy * dy;

            if (dist_sq <= (float)(ball_r * ball_r)) {
                /* Ball overlaps this tile - reveal it with fade animation */
                s_tile_revealed[idx] = true;
                if (s_tiles[idx] != NULL) {
                    /* Animate opacity from dark to transparent */
                    lv_anim_t anim;
                    lv_anim_init(&anim);
                    lv_anim_set_var(&anim, s_tiles[idx]);
                    lv_anim_set_values(&anim, TILE_DARK_OPA, 0);
                    lv_anim_set_duration(&anim, 300);
                    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_style_bg_opa_anim_cb);
                    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
                    lv_anim_start(&anim);
                }
            }
        }
    }
}

/**
 * Create the tile overlay grid on top of the image.
 * Each tile is a dark semi-transparent rectangle.
 */
static void create_tile_overlay(lv_obj_t *parent)
{
    for (int row = 0; row < TILE_ROWS; row++) {
        for (int col = 0; col < TILE_COLS; col++) {
            int idx = row * TILE_COLS + col;
            s_tile_revealed[idx] = false;

            lv_obj_t *tile = lv_obj_create(parent);
            lv_obj_remove_style_all(tile);
            lv_obj_set_size(tile, TILE_W, TILE_H);
            lv_obj_set_pos(tile, col * TILE_W, row * TILE_H);

            /* Dark semi-transparent background */
            lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(tile, TILE_DARK_OPA, 0);

            /* No border, no radius for seamless grid */
            lv_obj_set_style_border_width(tile, 0, 0);
            lv_obj_set_style_radius(tile, 0, 0);

            /* Disable scrolling and input on tiles */
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

            s_tiles[idx] = tile;
        }
    }
}

/* -------------------------------------------------------
 * Image navigation
 * ------------------------------------------------------- */

static void show_next_async_cb(void *arg)
{
    if (!s_slideshow_active) return;
    int count = piclist_get_count();
    if (count == 0) return;
    s_pic_index = (s_pic_index + 1) % count;
    ui_show_image_screen(piclist_get_path(s_pic_index));
}

static void show_prev_async_cb(void *arg)
{
    if (!s_slideshow_active) return;
    int count = piclist_get_count();
    if (count == 0) return;
    s_pic_index = (s_pic_index - 1 + count) % count;
    ui_show_image_screen(piclist_get_path(s_pic_index));
}

static void start_slideshow_async_cb(void *arg)
{
    int count = piclist_get_count();
    if (count == 0) return;
    s_slideshow_active = true;
    s_pic_index = 0;
    ui_show_image_screen(piclist_get_path(0));
}

void ui_start_slideshow(void)
{
    lv_async_call(start_slideshow_async_cb, NULL);
}

void ui_next_image(void)
{
    lv_async_call(show_next_async_cb, NULL);
}

void ui_prev_image(void)
{
    lv_async_call(show_prev_async_cb, NULL);
}

/* -------------------------------------------------------
 * Init screen
 * ------------------------------------------------------- */
void ui_show_init_screen(void)
{
    lv_obj_t *act_scr = lv_scr_act();
    lv_obj_clean(act_scr);
    lv_obj_set_style_bg_color(act_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(act_scr, LV_OPA_COVER, 0);

    s_status_label = lv_label_create(act_scr);
    lv_obj_set_width(s_status_label, 460);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_status_label, "Initializing...");
    lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 10, 10);
    ESP_LOGI(TAG, "Init screen displayed");
}

/* -------------------------------------------------------
 * Image screen: hardware JPEG decode -> LVGL raw image + tile overlay
 * ------------------------------------------------------- */
void ui_show_image_screen(const char *spiffs_path)
{
    ESP_LOGI(TAG, "show_image_screen: %s", spiffs_path);

    /* Stop existing reveal timer */
    if (s_reveal_timer != NULL) {
        lv_timer_del(s_reveal_timer);
        s_reveal_timer = NULL;
    }

    /* Clear tile pointers */
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_tile_revealed, 0, sizeof(s_tile_revealed));

    /* ---- 1. Read JPEG file into PSRAM ---- */
    FILE *f = fopen(spiffs_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "File not found: %s (errno=%d)", spiffs_path, errno);
        return;
    }
    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    rewind(f);

    uint8_t *jpeg_buf = heap_caps_malloc(file_sz, MALLOC_CAP_SPIRAM);
    if (jpeg_buf == NULL) {
        ESP_LOGE(TAG, "Failed to alloc JPEG input buf (%ld bytes) in PSRAM", file_sz);
        fclose(f);
        return;
    }
    fread(jpeg_buf, 1, file_sz, f);
    fclose(f);
    ESP_LOGI(TAG, "JPEG file loaded: %ld bytes", file_sz);

    /* ---- 2. Hardware JPEG decode to RGB565_LE ---- */
    int64_t t0 = esp_timer_get_time();

    jpeg_dec_config_t dec_cfg = DEFAULT_JPEG_DEC_CONFIG();
    dec_cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

    jpeg_dec_handle_t jpeg_dec = NULL;
    jpeg_error_t jerr = jpeg_dec_open(&dec_cfg, &jpeg_dec);
    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open failed: %d", jerr);
        heap_caps_free(jpeg_buf);
        return;
    }

    jpeg_dec_io_t jpeg_io = {
        .inbuf     = jpeg_buf,
        .inbuf_len = (int)file_sz,
    };
    jpeg_dec_header_info_t hdr = {0};
    jerr = jpeg_dec_parse_header(jpeg_dec, &jpeg_io, &hdr);
    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_parse_header failed: %d", jerr);
        jpeg_dec_close(jpeg_dec);
        heap_caps_free(jpeg_buf);
        return;
    }
    ESP_LOGI(TAG, "JPEG header: %dx%d", hdr.width, hdr.height);

    /* Allocate / reuse output buffer in PSRAM (16-byte aligned) */
    size_t needed = (size_t)hdr.width * hdr.height * 2;
    if (s_jpeg_rgb_buf == NULL || s_jpeg_rgb_size < needed) {
        if (s_jpeg_rgb_buf) {
            jpeg_free_align(s_jpeg_rgb_buf);
        }
        s_jpeg_rgb_buf = heap_caps_aligned_alloc(16, needed, MALLOC_CAP_SPIRAM);
        if (s_jpeg_rgb_buf == NULL) {
            ESP_LOGE(TAG, "Failed to alloc RGB buf (%u bytes) in PSRAM", (unsigned)needed);
            jpeg_dec_close(jpeg_dec);
            heap_caps_free(jpeg_buf);
            return;
        }
        s_jpeg_rgb_size = needed;
        ESP_LOGI(TAG, "RGB565 buf allocated: %u bytes in PSRAM", (unsigned)needed);
    }

    jpeg_io.outbuf = s_jpeg_rgb_buf;
    jerr = jpeg_dec_process(jpeg_dec, &jpeg_io);
    jpeg_dec_close(jpeg_dec);
    heap_caps_free(jpeg_buf);

    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_process failed: %d", jerr);
        return;
    }

    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "HW JPEG decode done in %lld ms", (t1 - t0) / 1000);

    /* ---- 3. Build LVGL image descriptor ---- */
    memset(&s_img_dsc, 0, sizeof(s_img_dsc));
    s_img_dsc.header.magic     = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf        = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w         = hdr.width;
    s_img_dsc.header.h         = hdr.height;
    s_img_dsc.header.stride    = hdr.width * 2;
    s_img_dsc.data_size        = needed;
    s_img_dsc.data             = s_jpeg_rgb_buf;

    /* ---- 4. Display on screen with tile overlay ---- */
    s_status_label = NULL;
    lv_obj_t *act_scr = lv_scr_act();
    lv_obj_clean(act_scr);
    lv_obj_set_style_bg_color(act_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(act_scr, LV_OPA_COVER, 0);

    /* Image: centered, no movement */
    lv_obj_t *img = lv_image_create(act_scr);
    lv_image_set_src(img, &s_img_dsc);
    int32_t center_x = (SCREEN_SIZE - (int32_t)hdr.width) / 2;
    int32_t center_y = (SCREEN_SIZE - (int32_t)hdr.height) / 2;
    lv_obj_set_pos(img, center_x, center_y);
    s_current_img = img;

    /* Create dark tile overlay on top of the image */
    create_tile_overlay(act_scr);

    /* Start the reveal timer (checks ball position periodically) */
    s_reveal_timer = lv_timer_create(tile_reveal_timer_cb, 33, NULL);  /* ~30 Hz */

    ESP_LOGI(TAG, "show_image_screen done: %s (tiles=%dx%d)", spiffs_path, TILE_COLS, TILE_ROWS);
}
