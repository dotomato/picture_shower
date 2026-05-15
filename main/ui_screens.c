/**
 * @file ui_screens.c
 * @brief UI screens: init screen, image display, slideshow
 */
#include "ui_screens.h"
#include "network.h"

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

static const char *TAG = "ui";

/* Init screen status label */
static lv_obj_t *s_status_label = NULL;

/* Pending log message buffer (written by net_task, read by LVGL task) */
#define UI_LOG_BUF_SIZE 128
static char s_ui_log_buf[UI_LOG_BUF_SIZE];

/* Slideshow state */
static int s_pic_index = 0;
static lv_timer_t *s_slideshow_timer = NULL;

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
    /* Append new line to existing text */
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
    /* Small delay so LVGL task has a chance to render */
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* -------------------------------------------------------
 * Slideshow
 * ------------------------------------------------------- */
static void slideshow_timer_cb(lv_timer_t *timer)
{
    int count = piclist_get_count();
    if (count == 0) return;
    s_pic_index = (s_pic_index + 1) % count;
    ui_show_image_screen(piclist_get_path(s_pic_index));
}

static void start_slideshow_async_cb(void *arg)
{
    int count = piclist_get_count();
    if (count == 0) return;
    /* Show first image immediately */
    s_pic_index = 0;
    ui_show_image_screen(piclist_get_path(0));
    /* Start 3-second timer to cycle through images */
    if (s_slideshow_timer != NULL) {
        lv_timer_del(s_slideshow_timer);
    }
    s_slideshow_timer = lv_timer_create(slideshow_timer_cb, 3000, NULL);
}

void ui_start_slideshow(void)
{
    lv_async_call(start_slideshow_async_cb, NULL);
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

    /* Status label: large font, white, top-left aligned, word-wrap */
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
 * Image screen: hardware JPEG decode -> LVGL raw image
 * ------------------------------------------------------- */
void ui_show_image_screen(const char *spiffs_path)
{
    ESP_LOGI(TAG, "show_image_screen: %s", spiffs_path);

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
    size_t needed = (size_t)hdr.width * hdr.height * 2;  /* RGB565 = 2 bytes/pixel */
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
    heap_caps_free(jpeg_buf);  /* JPEG source no longer needed */

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

    /* ---- 4. Display on screen ---- */
    s_status_label = NULL;
    lv_obj_t *act_scr = lv_scr_act();
    lv_obj_clean(act_scr);
    lv_obj_set_style_bg_color(act_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(act_scr, LV_OPA_COVER, 0);

    lv_obj_t *img = lv_image_create(act_scr);
    lv_image_set_src(img, &s_img_dsc);
    lv_obj_center(img);

    ESP_LOGI(TAG, "show_image_screen done: %s", spiffs_path);
}
