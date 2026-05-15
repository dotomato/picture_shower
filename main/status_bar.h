/**
 * @file status_bar.h
 * @brief Status Bar on sys_layer
 *
 * Shows: FPS | Free RAM | Battery
 * Sits at the bottom of the screen, always on top.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create the status bar on the sys_layer.
 * Must be called with LVGL lock held, after display is created.
 */
void status_bar_create(void);

#ifdef __cplusplus
}
#endif
