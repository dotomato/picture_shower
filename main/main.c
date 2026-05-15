/**
 * @file main.c
 * @brief Application entry point
 *
 * Initializes NVS, SPIFFS, display, status bar, and launches the network task.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "axp2101.h"
#include "status_bar.h"
#include "network.h"
#include "ui_screens.h"
#include "gravity_ball.h"
#include "touch_input.h"

static const char *TAG = "startup";

/* Print a one-line RAM snapshot: internal / DMA / PSRAM free bytes */
#define LOG_RAM(label) \
    ESP_LOGI(TAG, "[RAM] %-30s  int=%lu  dma=%lu  psram=%lu", (label), \
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), \
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA), \
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM))

static void spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path       = "/spiffs",
        .partition_label = "storage",
        .max_files       = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info("storage", &total, &used);
        ESP_LOGI(TAG, "SPIFFS mounted: total=%d, used=%d", total, used);
    }
}

void app_main(void)
{
    LOG_RAM("app_main: start");

    /* Initialize NVS (required by WiFi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    LOG_RAM("after nvs_flash_init");

    /* Mount SPIFFS first (wifi_config.txt lives here) */
    spiffs_init();
    LOG_RAM("after spiffs_init");

    /* Start display */
    ESP_LOGI(TAG, "Starting display...");
    bsp_display_cfg_t disp_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 1,
        },
    };
    /* Increase LVGL worker task stack to 32KB for JPEG decoding, place in PSRAM */
    disp_cfg.lv_adapter_cfg.task_stack_size = 32 * 1024;
    disp_cfg.lv_adapter_cfg.stack_in_psram  = true;
    bsp_display_start_with_config(&disp_cfg);
    LOG_RAM("after bsp_display_start");

    if (ESP_OK != bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock, aborting startup screen");
        return;
    }

    /* Initialize AXP2101 PMU (battery management) */
    axp2101_init();

    /* Create status bar on sys_layer (always visible: FPS | RAM | Battery) */
    status_bar_create();

    /* Show initializing screen */
    ui_show_init_screen();

    /* Initialize gravity ball (QMI8658 accelerometer) */
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    esp_err_t ball_ret = gravity_ball_init(i2c_bus);
    if (ball_ret == ESP_OK) {
        ESP_LOGI(TAG, "Gravity ball initialized");
    } else {
        ESP_LOGW(TAG, "Gravity ball init failed: %s", esp_err_to_name(ball_ret));
    }

    /* Initialize touch input (tap to switch images) */
    touch_input_init();

    bsp_display_unlock();
    LOG_RAM("after show_init_screen");

    /* Start gravity ball update task (runs outside LVGL lock) */
    if (ball_ret == ESP_OK) {
        gravity_ball_start();
    }

    /* Launch background net_task (WiFi init happens inside, after LVGL first render) */
    ESP_LOGI(TAG, "Creating net_task (stack=16KB, prio=3, PSRAM)...");
    StaticTask_t *task_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StackType_t  *stack_buf = heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
    if (task_buf == NULL || stack_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate net_task buffers (task_buf=%p, stack_buf=%p)", task_buf, stack_buf);
        if (task_buf)  heap_caps_free(task_buf);
        if (stack_buf) heap_caps_free(stack_buf);
    } else {
        TaskHandle_t task_handle = xTaskCreateStatic(net_task, "net_task",
                                                     16 * 1024, NULL, 3,
                                                     stack_buf, task_buf);
        if (task_handle == NULL) {
            ESP_LOGE(TAG, "Failed to create net_task!");
            heap_caps_free(task_buf);
            heap_caps_free(stack_buf);
        } else {
            ESP_LOGI(TAG, "net_task created successfully (PSRAM stack)");
        }
    }
    LOG_RAM("app_main: done");
}
