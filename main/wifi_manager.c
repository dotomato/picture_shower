/**
 * @file wifi_manager.c
 * @brief WiFi initialization, connection, and event handling
 */
#include "wifi_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "string.h"
#include "stdio.h"

static const char *TAG = "wifi";

#define WIFI_MAX_RETRY  1

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_count = 0;

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
            ESP_LOGW(TAG, "Retrying WiFi connection (%d/%d)...", s_retry_count, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
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
        ESP_LOGE(TAG, "wifi_config.txt not found");
        return ESP_ERR_NOT_FOUND;
    }

    /* Read SSID (first line) */
    if (fgets(ssid, ssid_len, f) == NULL) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to read SSID");
        return ESP_FAIL;
    }

    /* Remove trailing whitespace characters */
    char *end = ssid + strlen(ssid) - 1;
    while (end >= ssid && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
        *end-- = '\0';
    }

    /* Read password (second line) */
    if (fgets(password, pass_len, f) == NULL) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to read password");
        return ESP_FAIL;
    }

    /* Remove trailing whitespace characters */
    end = password + strlen(password) - 1;
    while (end >= password && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
        *end-- = '\0';
    }

    fclose(f);
    ESP_LOGI(TAG, "Parsed SSID: [%s]", ssid);
    ESP_LOGI(TAG, "Parsed password: [%s]", password);
    return ESP_OK;
}

EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return s_wifi_event_group;
}

void wifi_manager_init(void)
{
    char ssid[64] = {0};
    char password[128] = {0};

    if (read_wifi_config(ssid, sizeof(ssid), password, sizeof(password)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WiFi config, skipping WiFi init");
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

    ESP_LOGI(TAG, "WiFi init done, connecting to SSID: %s (async)", ssid);
}

void wifi_manager_deinit(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_err_t ret = esp_netif_deinit();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "esp_netif_deinit: %s (ignored)", esp_err_to_name(ret));
    }
}
