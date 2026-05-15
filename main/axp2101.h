/**
 * @file axp2101.h
 * @brief AXP2101 Power Management IC driver (using XPowersLib)
 *
 * Full PMU driver with charging management, ADC measurement,
 * and battery status. The chip sits on the same I2C bus as the
 * touch controller.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize AXP2101 via XPowersLib.
 * Configures ADC, charging parameters, and interrupts.
 * Must be called after bsp_i2c_init() (which happens inside bsp_display_start).
 */
void axp2101_init(void);

/**
 * Read battery percentage from AXP2101 fuel gauge.
 * @return 0-100 on success, -1 if unavailable.
 */
int battery_get_percent(void);

/**
 * Check if battery is currently charging.
 * @return true if charging.
 */
bool battery_is_charging(void);

/**
 * Get battery voltage in millivolts.
 * @return voltage in mV, or -1 if unavailable.
 */
int battery_get_voltage(void);

/**
 * Get VBUS (USB) voltage in millivolts.
 * @return voltage in mV, or -1 if unavailable.
 */
int battery_get_vbus_voltage(void);

/**
 * Get system voltage in millivolts.
 * @return voltage in mV, or -1 if unavailable.
 */
int battery_get_sys_voltage(void);

/**
 * Get PMU die temperature in Celsius.
 * @return temperature in °C, or -999.0 if unavailable.
 */
float battery_get_temperature(void);

/**
 * Get charger status code.
 * @return charger status (0=tri, 1=pre, 2=CC, 3=CV, 4=done, 5=stop), -1 if unavailable.
 */
int battery_get_charger_status(void);

/**
 * Check if VBUS (USB power) is connected.
 * @return true if VBUS present.
 */
bool battery_is_vbus_in(void);

/**
 * Check if battery is physically connected.
 * @return true if battery detected.
 */
bool battery_is_battery_connected(void);

/**
 * Dump full PMU status to log (for debugging).
 */
void axp2101_dump_status(void);

#ifdef __cplusplus
}
#endif
