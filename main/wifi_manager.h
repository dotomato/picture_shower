/**
 * @file wifi_manager.h
 * @brief WiFi initialization, connection, and event handling
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/**
 * Get the WiFi event group handle.
 * @return Event group handle, or NULL if WiFi was not initialized.
 */
EventGroupHandle_t wifi_manager_get_event_group(void);

/**
 * Read WiFi credentials from /spiffs/wifi_config.txt and start WiFi STA.
 * Non-blocking: connection result is signaled via event group bits.
 */
void wifi_manager_init(void);

/**
 * Fully deinitialize WiFi stack to free DMA RAM.
 */
void wifi_manager_deinit(void);

#ifdef __cplusplus
}
#endif
