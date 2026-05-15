/**
 * @file axp2101.h
 * @brief AXP2101 Power Management IC – lightweight I2C driver
 *
 * Only reads battery percentage and charging status.
 * The chip sits on the same I2C bus as the touch controller.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize AXP2101 I2C device handle.
 * Must be called after bsp_i2c_init() (which happens inside bsp_display_start).
 */
void axp2101_init(void);

/**
 * Read battery percentage from AXP2101.
 * @return 0-100 on success, -1 if unavailable.
 */
int battery_get_percent(void);

/**
 * Check if USB/VBUS is connected (charging or powered).
 * @return true if VBUS present.
 */
bool battery_is_charging(void);

#ifdef __cplusplus
}
#endif
