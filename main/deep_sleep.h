/**
 * @file deep_sleep.h
 * @brief Deep sleep management module
 *
 * Handles GPIO16 button interrupt to enter deep sleep mode.
 * Turns off the display before sleeping and turns it on after wakeup.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize deep sleep module
 *
 * Sets up GPIO16 interrupt. When the button is pressed, the display
 * is turned off and the system enters deep sleep. On wakeup (button
 * press again), the system restarts and the display is turned back on.
 */
void deep_sleep_init(void);

#ifdef __cplusplus
}
#endif
