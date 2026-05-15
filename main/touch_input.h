/**
 * @file touch_input.h
 * @brief Touch input handler for image navigation
 *
 * Monitors touch screen events and triggers image switching.
 * Uses a tap gesture on the screen to advance to the next image.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize touch input handling.
 * Creates an LVGL timer that monitors touch events and triggers
 * image navigation when a tap is detected.
 * Must be called with LVGL lock held.
 */
void touch_input_init(void);

#ifdef __cplusplus
}
#endif
