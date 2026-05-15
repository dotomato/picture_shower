/**
 * @file ui_screens.h
 * @brief UI screens: init screen, image display with tile-reveal mechanic
 */
#pragma once

#include <stdbool.h>

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

/**
 * Get the current picture index.
 */
int ui_get_pic_index(void);

/**
 * Get the revealed state of all tiles.
 * @param out_revealed Output array (must have at least 'count' elements)
 * @param count Number of tiles to read
 */
void ui_get_tile_revealed(bool *out_revealed, int count);

/**
 * Restore game state: show image at given index with pre-revealed tiles.
 * Must be called from a context where LVGL async calls are safe.
 * @param pic_index Image index to display
 * @param revealed Array of tile revealed states
 * @param count Number of tiles
 */
void ui_restore_state(int pic_index, const bool *revealed, int count);

#ifdef __cplusplus
}
#endif
