/**
 * @file gravity_ball.h
 * @brief Gravity-responsive ball simulation using QMI8658 accelerometer
 *
 * Displays a semi-transparent white circle on screen that moves according
 * to the device's tilt (accelerometer data) and bounces off screen edges.
 */
#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize QMI8658 sensor and create the ball UI object.
 * Must be called with LVGL lock held.
 * @param bus_handle I2C master bus handle (from bsp_i2c_get_handle)
 * @return ESP_OK on success
 */
esp_err_t gravity_ball_init(i2c_master_bus_handle_t bus_handle);

/**
 * Start the gravity ball update task.
 * Reads accelerometer data and updates ball position periodically.
 */
void gravity_ball_start(void);

/**
 * Stop the gravity ball update task and clean up.
 */
void gravity_ball_stop(void);

/**
 * Get the current ball center position.
 * @param x pointer to receive X coordinate (center)
 * @param y pointer to receive Y coordinate (center)
 */
void gravity_ball_get_position(float *x, float *y);

/**
 * Get the ball radius in pixels.
 */
int gravity_ball_get_radius(void);

/**
 * Get the current ball velocity.
 * @param vx pointer to receive X velocity
 * @param vy pointer to receive Y velocity
 */
void gravity_ball_get_velocity(float *vx, float *vy);

/**
 * Set the ball position and velocity (for state restore).
 * Must be called after gravity_ball_init and before or after gravity_ball_start.
 */
void gravity_ball_set_state(float x, float y, float vx, float vy);

#ifdef __cplusplus
}
#endif
