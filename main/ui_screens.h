/**
 * @file ui_screens.h
 * @brief UI screens: init screen, image display, slideshow
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Show the initializing screen with a status label.
 * Must be called with LVGL lock held.
 */
void ui_show_init_screen(void);

/**
 * Show a JPEG image from SPIFFS on screen.
 * Must be called from LVGL context (async call or with lock held).
 * @param spiffs_path e.g. "/spiffs/pic1.jpg"
 */
void ui_show_image_screen(const char *spiffs_path);

/**
 * Thread-safe: post a log line to the init screen from any task.
 */
void ui_log(const char *msg);

/**
 * Start the image slideshow (async-safe, can be called from any task).
 * Uses the piclist from network module.
 */
void ui_start_slideshow(void);

#ifdef __cplusplus
}
#endif
