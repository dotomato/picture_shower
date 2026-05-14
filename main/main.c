#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "errno.h"
#include "stdio.h"
#include "string.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"
#include "esp_timer.h"

static const char *TAG = "startup";
static const char *WIFI_TAG = "wifi";

/* Print a one-line RAM snapshot: internal / DMA / PSRAM free bytes */
#define LOG_RAM(label) \
    ESP_LOGI(TAG, "[RAM] %-30s  int=%lu  dma=%lu  psram=%lu", (label), \
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), \
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA), \
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM))

/* WiFi event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      1

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_count = 0;



/* Forward declarations */
static void show_image_screen(const char *spiffs_path);
static void show_init_screen(void);
static void ui_log(const char *msg);
static void fetch_piclist(void);
static void net_task(void *arg);

/* Init screen status label (global, updated via lv_async_call) */
static lv_obj_t *s_status_label = NULL;

/* Pending log message buffer (written by net_task, read by LVGL task) */
#define UI_LOG_BUF_SIZE 128
static char s_ui_log_buf[UI_LOG_BUF_SIZE];

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

/* Thread-safe: post a log line to the init screen from any task */
static void ui_log(const char *msg)
{
    strlcpy(s_ui_log_buf, msg, UI_LOG_BUF_SIZE);
    lv_async_call(ui_log_async_cb, s_ui_log_buf);
    /* Small delay so LVGL task has a chance to render */
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* Image slideshow list */
#define MAX_PIC_COUNT 16
static char s_pic_list[MAX_PIC_COUNT][64];
static int  s_pic_count = 0;
static int  s_pic_index = 0;
static lv_timer_t *s_slideshow_timer = NULL;

/* Hardware JPEG decode buffer (PSRAM): reused across slides to avoid repeated alloc/free */
static uint8_t          *s_jpeg_rgb_buf  = NULL;  /* decoded RGB565 pixels in PSRAM */
static size_t            s_jpeg_rgb_size = 0;     /* allocated size in bytes */
static lv_image_dsc_t    s_img_dsc;               /* LVGL image descriptor wrapping the buffer */

static void slideshow_timer_cb(lv_timer_t *timer)
{
    if (s_pic_count == 0) return;
    s_pic_index = (s_pic_index + 1) % s_pic_count;
    show_image_screen(s_pic_list[s_pic_index]);
}

static void start_slideshow_async_cb(void *arg)
{
    if (s_pic_count == 0) return;
    /* Show first image immediately */
    s_pic_index = 0;
    show_image_screen(s_pic_list[0]);
    /* Start 10-second timer to cycle through images */
    if (s_slideshow_timer != NULL) {
        lv_timer_del(s_slideshow_timer);
    }
    s_slideshow_timer = lv_timer_create(slideshow_timer_cb, 3000, NULL);
}

/* -------------------------------------------------------
 * WiFi event handler
 * ------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(WIFI_TAG, "Retrying WiFi connection (%d/%d)...", s_retry_count, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(WIFI_TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(WIFI_TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* -------------------------------------------------------
 * Read WiFi config from /spiffs/wifi_config.txt
 * Format: first line = SSID, second line = password
 * ------------------------------------------------------- */
static esp_err_t read_wifi_config(char *ssid, size_t ssid_len,
                                  char *password, size_t pass_len)
{
    FILE *f = fopen("/spiffs/wifi_config.txt", "r");
    if (f == NULL) {
        ESP_LOGE(WIFI_TAG, "wifi_config.txt not found");
        return ESP_ERR_NOT_FOUND;
    }

    /* Read SSID (first line) */
    if (fgets(ssid, ssid_len, f) == NULL) {
        fclose(f);
        ESP_LOGE(WIFI_TAG, "Failed to read SSID");
        return ESP_FAIL;
    }
    
    /* Remove trailing whitespace characters (newline, carriage return, space, tab) */
    char *end = ssid + strlen(ssid) - 1;
    while (end >= ssid && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
        *end-- = '\0';
    }

    /* Read password (second line) */
    if (fgets(password, pass_len, f) == NULL) {
        fclose(f);
        ESP_LOGE(WIFI_TAG, "Failed to read password");
        return ESP_FAIL;
    }
    
    /* Remove trailing whitespace characters (newline, carriage return, space, tab) */
    end = password + strlen(password) - 1;
    while (end >= password && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
        *end-- = '\0';
    }

    fclose(f);
    ESP_LOGI(WIFI_TAG, "Parsed SSID: [%s]", ssid);
    ESP_LOGI(WIFI_TAG, "Parsed password: [%s]", password);
    return ESP_OK;
}

/* -------------------------------------------------------
 * Initialize and connect WiFi (non-blocking after start)
 * ------------------------------------------------------- */
static void wifi_init(void)
{
    char ssid[64] = {0};
    char password[128] = {0};

    if (read_wifi_config(ssid, sizeof(ssid), password, sizeof(password)) != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "Failed to read WiFi config, skipping WiFi init");
        return;
    }

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, "tomato_mini"));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid,     ssid,     sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(WIFI_TAG, "WiFi init done, connecting to SSID: %s", ssid);

    /* WiFi started, connection result will be signaled via event group */
    ESP_LOGI(WIFI_TAG, "WiFi init done, connecting to SSID: %s (async)", ssid);
}

/* -------------------------------------------------------
 * Download a file from URL and save to SPIFFS path
 * Returns ESP_OK on success
 * ------------------------------------------------------- */
static esp_err_t download_file(const char *url, const char *save_path)
{
    ESP_LOGI(TAG, "Downloading: %s -> %s", url, save_path);

    esp_http_client_config_t config = {
        .url         = url,
        .timeout_ms  = 15000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status=%d, content_length=%d", status, content_len);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP error status: %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    FILE *f = fopen(save_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", save_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char *buf = malloc(1024);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate download buffer");
        fclose(f);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    int total = 0;
    int read_len;
    while ((read_len = esp_http_client_read(client, buf, 1024)) > 0) {
        fwrite(buf, 1, read_len, f);
        total += read_len;
    }
    free(buf);
    fclose(f);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Download complete: %d bytes saved to %s", total, save_path);
    return (total > 0) ? ESP_OK : ESP_FAIL;
}

/* -------------------------------------------------------
 * Strip trailing whitespace (newline, CR, space, tab) in-place
 * ------------------------------------------------------- */
static void strip_trailing(char *s)
{
    char *end = s + strlen(s) - 1;
    while (end >= s && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
        *end-- = '\0';
    }
}

/* -------------------------------------------------------
 * Read local cached piclist md5 (first line of /spiffs/piclist.txt)
 * Returns ESP_OK and fills md5_out (33 bytes) on success
 * ------------------------------------------------------- */
static esp_err_t read_cached_md5(char *md5_out, size_t len)
{
    FILE *f = fopen("/spiffs/piclist.txt", "r");
    if (f == NULL) return ESP_ERR_NOT_FOUND;
    char line[64] = {0};
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return ESP_FAIL;
    }
    fclose(f);
    strip_trailing(line);
    strlcpy(md5_out, line, len);
    return ESP_OK;
}

/* -------------------------------------------------------
 * Load image list from local cached /spiffs/piclist.txt
 * (skips first line which is the md5)
 * ------------------------------------------------------- */
static void load_piclist_from_cache(void)
{
    FILE *f = fopen("/spiffs/piclist.txt", "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Cannot open cached piclist.txt");
        return;
    }
    char line[64];
    bool first_line = true;
    s_pic_count = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        strip_trailing(line);
        if (strlen(line) == 0) continue;
        if (first_line) { first_line = false; continue; } /* skip md5 line */
        if (s_pic_count < MAX_PIC_COUNT) {
            snprintf(s_pic_list[s_pic_count], sizeof(s_pic_list[0]), "/spiffs/%s", line);
            ESP_LOGI(TAG, "  cached: %s", s_pic_list[s_pic_count]);
            s_pic_count++;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "Loaded %d image(s) from cache", s_pic_count);
}

/* -------------------------------------------------------
 * Fetch piclist.txt from server, compare md5, download
 * images only when md5 changed, then start slideshow
 * ------------------------------------------------------- */
static void fetch_piclist(void)
{
    static const char *PICLIST_URL = "http://h1.tomatochen.top:8001/piclist.txt";
    ESP_LOGI(TAG, "Fetching: %s", PICLIST_URL);

    esp_http_client_config_t config = {
        .url         = PICLIST_URL,
        .timeout_ms  = 5000,
        .buffer_size = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status=%d, content_length=%d", status, content_len);

    /* Read full response into buffer (max 512 bytes) */
    char *body = calloc(1, 512);
    if (body == NULL) {
        ESP_LOGE(TAG, "Failed to allocate piclist buffer");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }
    int total = 0, read_len;
    while ((read_len = esp_http_client_read(client, body + total, 511 - total)) > 0) {
        total += read_len;
    }
    body[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* --- Parse first line as md5 --- */
    char remote_md5[64] = {0};
    char *first_nl = strchr(body, '\n');
    if (first_nl == NULL) {
        ESP_LOGE(TAG, "piclist.txt format error: no newline found");
        free(body);
        return;
    }
    size_t md5_len = first_nl - body;
    if (md5_len >= sizeof(remote_md5)) md5_len = sizeof(remote_md5) - 1;
    strncpy(remote_md5, body, md5_len);
    remote_md5[md5_len] = '\0';
    strip_trailing(remote_md5);
    ESP_LOGI(TAG, "Remote md5: [%s]", remote_md5);

    /* --- Compare with cached md5 --- */
    char cached_md5[64] = {0};
    bool md5_match = false;
    if (read_cached_md5(cached_md5, sizeof(cached_md5)) == ESP_OK) {
        ESP_LOGI(TAG, "Cached md5: [%s]", cached_md5);
        md5_match = (strcmp(remote_md5, cached_md5) == 0);
    } else {
        ESP_LOGI(TAG, "No cached piclist found");
    }

    if (md5_match) {
        /* md5 unchanged: load image list from cache, skip download */
        ESP_LOGI(TAG, "md5 match, loading images from cache");
        ui_log("Images up to date, loading cache...");
        free(body);
        load_piclist_from_cache();
    } else {
        /* md5 changed: download all images and save new piclist */
        ESP_LOGI(TAG, "md5 changed, downloading images...");
        ui_log("Downloading images...");
        s_pic_count = 0;

        /* Parse image filenames (lines after the first) */
        char *line = first_nl + 1;
        char *next;
        while (line && *line) {
            next = strchr(line, '\n');
            if (next) *next = '\0';

            /* Strip \r */
            char *cr = strchr(line, '\r');
            if (cr) *cr = '\0';

            /* Skip empty lines */
            if (strlen(line) == 0) {
                line = next ? next + 1 : NULL;
                continue;
            }

            ESP_LOGI(TAG, "  %s", line);

            char url[128];
            char spiffs_path[64];
            char ui_msg[64];
            snprintf(url, sizeof(url), "http://h1.tomatochen.top:8001/%s", line);
            snprintf(spiffs_path, sizeof(spiffs_path), "/spiffs/%s", line);
            snprintf(ui_msg, sizeof(ui_msg), "  %s", line);
            ui_log(ui_msg);

            if (download_file(url, spiffs_path) == ESP_OK) {
                if (s_pic_count < MAX_PIC_COUNT) {
                    strlcpy(s_pic_list[s_pic_count], spiffs_path, sizeof(s_pic_list[0]));
                    s_pic_count++;
                }
            }

            line = next ? next + 1 : NULL;
        }

        ESP_LOGI(TAG, "Downloaded %d image(s)", s_pic_count);

        /* Save new piclist.txt to SPIFFS (overwrite) */
        FILE *f = fopen("/spiffs/piclist.txt", "w");
        if (f != NULL) {
            fwrite(body, 1, total, f);
            fclose(f);
            ESP_LOGI(TAG, "Saved new piclist.txt to SPIFFS");
        } else {
            ESP_LOGE(TAG, "Failed to save piclist.txt");
        }
        free(body);
    }

}

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

    /* NOTE: WiFi is initialized inside net_task AFTER LVGL first render,
     * to avoid WiFi occupying DMA RAM before SPI display is ready. */

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
    /* Increase LVGL worker task stack to 32KB for JPEG decoding, place in PSRAM to save internal RAM for WiFi */
    disp_cfg.lv_adapter_cfg.task_stack_size = 32 * 1024;
    disp_cfg.lv_adapter_cfg.stack_in_psram  = true;
    bsp_display_start_with_config(&disp_cfg);
    LOG_RAM("after bsp_display_start");

    if (ESP_OK != bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock, aborting startup screen");
        return;
    }

    /* Show initializing screen */
    show_init_screen();

    bsp_display_unlock();
    LOG_RAM("after show_init_screen");

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

/* -------------------------------------------------------
 * Background network task: wait for WiFi, fetch & display
 * Runs after LVGL is already started
 * ------------------------------------------------------- */
static void net_task(void *arg)
{
    LOG_RAM("net_task: start");

    /* Wait a bit for LVGL to complete the first render of init screen
     * before WiFi occupies DMA RAM */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Initialize WiFi here (after LVGL first render) */
    ui_log("Connecting to WiFi...");
    LOG_RAM("before wifi_init");
    wifi_init();
    LOG_RAM("after wifi_init");

    bool wifi_inited = (s_wifi_event_group != NULL);

    if (!wifi_inited) {
        ESP_LOGE(TAG, "net_task: WiFi init failed, loading cache");
        ui_log("WiFi init failed, loading cache...");
        load_piclist_from_cache();
        goto show_result;
    }

    ESP_LOGI(TAG, "net_task: waiting for WiFi (30s timeout)...");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "net_task: WiFi connected, starting download");
        LOG_RAM("after WiFi connected");
        ui_log("WiFi connected!");
        fetch_piclist();
        LOG_RAM("after fetch_piclist");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "net_task: WiFi connection failed, loading cache");
        ui_log("WiFi failed, loading cache...");
        load_piclist_from_cache();
    } else {
        ESP_LOGE(TAG, "net_task: WiFi connection timeout (30s), loading cache");
        ui_log("WiFi timeout, loading cache...");
        load_piclist_from_cache();
    }

    /* Deinit WiFi BEFORE triggering LVGL display, to free ~60-80KB DMA RAM for SPI */
    LOG_RAM("before wifi_deinit");
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_err_t netif_ret = esp_netif_deinit();
    if (netif_ret != ESP_OK && netif_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "esp_netif_deinit: %s (ignored)", esp_err_to_name(netif_ret));
    }
    LOG_RAM("after wifi_deinit");

show_result:
    /* Trigger slideshow (DMA RAM is available for SPI) */
    if (s_pic_count > 0) {
        ESP_LOGI(TAG, "Starting slideshow with %d image(s)", s_pic_count);
        lv_async_call(start_slideshow_async_cb, NULL);
    } else {
        ESP_LOGW(TAG, "No images available (no cache and no network)");
        ui_log("No images available.");
    }

    LOG_RAM("net_task: done");
    ESP_LOGI(TAG, "net_task: exiting");
    vTaskDelete(NULL);
}

/* -------------------------------------------------------
 * Init screen: black background + scrollable status label
 * Must be called with LVGL lock held
 * ------------------------------------------------------- */
static void show_init_screen(void)
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
 * spiffs_path: e.g. "/spiffs/pic1.jpg"
 * Must be called with LVGL lock held
 * ------------------------------------------------------- */
static void show_image_screen(const char *spiffs_path)
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
        /* jpeg_calloc_align uses internal RAM by default; force PSRAM */
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
    s_img_dsc.header.stride    = hdr.width * 2;  /* bytes per row */
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
