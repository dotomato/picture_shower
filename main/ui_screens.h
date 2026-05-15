/**
 * @file ui_screens.h
 * @brief UI screens: init screen, image display with tile-reveal mechanic
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
 * Show a JPEG image from SPIFFS on screen with dark overlay tiles.
 * The image is centered and displayed dimly. Tiles are revealed when
 * the gravity ball passes over them.
 * Must be called from LVGL context (async call or with lock held).
 * @param spiffs_path e.g. "/spiffs/pic1.jpg"
 */
void ui_show_image_screen(const char *spiffs_path);

/**
 * Thread-safe: post a log line to the init screen from any task.
 */
void ui_log(const char *msg);

/**
 * Start the image display (async-safe, can be called from any task).
 * Shows the first image with tile overlay. No auto-advance.
 */
void ui_start_slideshow(void);

/**
 * Switch to the next image (async-safe, can be called from any task).
 * Resets all tiles to dark state.
 */
void ui_next_image(void);

/**
 * Switch to the previous image (async-safe, can be called from any task).
 * Resets all tiles to dark state.
 */
void ui_prev_image(void);

#ifdef __cplusplus
}
#endif
