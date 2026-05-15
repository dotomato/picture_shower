/**
 * @file network.h
 * @brief Network operations: HTTP download, piclist fetch/parse, net_task
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Download a file from URL and save to SPIFFS path.
 * @return ESP_OK on success.
 */
esp_err_t download_file(const char *url, const char *save_path);

/**
 * Fetch piclist.txt from server, compare md5, download images if needed.
 * Populates the internal image list.
 */
void fetch_piclist(void);

/**
 * Load image list from local cached /spiffs/piclist.txt.
 */
void load_piclist_from_cache(void);

/**
 * Get the number of images in the current list.
 */
int piclist_get_count(void);

/**
 * Get the SPIFFS path of an image by index.
 */
const char *piclist_get_path(int index);

/**
 * Background network task: WiFi init, fetch images, start slideshow.
 * Should be launched as a FreeRTOS task.
 */
void net_task(void *arg);

#ifdef __cplusplus
}
#endif
