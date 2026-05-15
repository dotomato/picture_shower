/**
 * @file audio.h
 * @brief Audio module for collision sound effects
 *
 * Uses the ES8311 codec via BSP to play short synthesized hit sounds
 * when tiles are revealed by the gravity ball.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the audio subsystem (I2S + ES8311 codec)
 *
 * Must be called once during startup. Safe to call multiple times.
 *
 * @return ESP_OK on success
 */
esp_err_t audio_init(void);

/**
 * @brief Play a short collision/hit sound effect (non-blocking)
 *
 * Triggers a brief synthesized "tick" sound. If a sound is already playing,
 * the new request is ignored (no queue).
 */
void audio_play_hit(void);

#ifdef __cplusplus
}
#endif
