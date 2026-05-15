/**
 * @file network.c
 * @brief Network operations: HTTP download, piclist fetch/parse, net_task
 */
#include "network.h"
#include "wifi_manager.h"
#include "ui_screens.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "string.h"
#include "stdio.h"

static const char *TAG = "network";

/* Print a one-line RAM snapshot */
#define LOG_RAM(label) \
    ESP_LOGI(TAG, "[RAM] %-30s  int=%lu  dma=%lu  psram=%lu", (label), \
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), \
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA), \
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM))

/* Image list */
#define MAX_PIC_COUNT 16
static char s_pic_list[MAX_PIC_COUNT][64];
static int  s_pic_count = 0;

/* -------------------------------------------------------
 * Strip trailing whitespace in-place
 * ------------------------------------------------------- */
static void strip_trailing(char *s)
{
    char *end = s + strlen(s) - 1;
    while (end >= s && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
        *end-- = '\0';
    }
}

int piclist_get_count(void)
{
    return s_pic_count;
}

const char *piclist_get_path(int index)
{
    if (index < 0 || index >= s_pic_count) return NULL;
    return s_pic_list[index];
}

esp_err_t download_file(const char *url, const char *save_path)
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
 * Read local cached piclist md5 (first line of /spiffs/piclist.txt)
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

void load_piclist_from_cache(void)
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

void fetch_piclist(void)
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

void net_task(void *arg)
{
    LOG_RAM("net_task: start");

    /* Wait a bit for LVGL to complete the first render of init screen */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Initialize WiFi */
    ui_log("Connecting to WiFi...");
    LOG_RAM("before wifi_init");
    wifi_manager_init();
    LOG_RAM("after wifi_init");

    EventGroupHandle_t evt_grp = wifi_manager_get_event_group();
    bool wifi_inited = (evt_grp != NULL);

    if (!wifi_inited) {
        ESP_LOGE(TAG, "net_task: WiFi init failed, loading cache");
        ui_log("WiFi init failed, loading cache...");
        load_piclist_from_cache();
        goto show_result;
    }

    ESP_LOGI(TAG, "net_task: waiting for WiFi (30s timeout)...");

    EventBits_t bits = xEventGroupWaitBits(
        evt_grp,
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

    /* Deinit WiFi to free DMA RAM */
    LOG_RAM("before wifi_deinit");
    wifi_manager_deinit();
    LOG_RAM("after wifi_deinit");

show_result:
    /* Trigger slideshow */
    if (s_pic_count > 0) {
        ESP_LOGI(TAG, "Starting slideshow with %d image(s)", s_pic_count);
        ui_start_slideshow();
    } else {
        ESP_LOGW(TAG, "No images available (no cache and no network)");
        ui_log("No images available.");
    }

    LOG_RAM("net_task: done");
    ESP_LOGI(TAG, "net_task: exiting");
    vTaskDelete(NULL);
}
