/**
 * @file game_state.h
 * @brief Game state save/load using RTC RAM
 *
 * Saves the current game state (image index, revealed tiles, ball position)
 * to RTC RAM before deep sleep, and restores it on wakeup.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAME_STATE_TILE_COUNT 64  /* 8x8 grid */

/**
 * @brief Game state structure stored in RTC RAM
 */
typedef struct {
    uint32_t magic;                          /* Magic number to validate data */
    int      pic_index;                      /* Current image index */
    bool     tile_revealed[GAME_STATE_TILE_COUNT]; /* Revealed tile flags */
    float    ball_pos_x;                     /* Ball X position */
    float    ball_pos_y;                     /* Ball Y position */
    float    ball_vel_x;                     /* Ball X velocity */
    float    ball_vel_y;                     /* Ball Y velocity */
} game_state_t;

/**
 * @brief Save current game state to RTC RAM
 *
 * Collects state from ui_screens and gravity_ball modules and
 * writes it to RTC slow memory.
 */
void game_state_save(void);

/**
 * @brief Check if a valid game state exists in RTC RAM
 * @return true if valid state is available (wakeup from deep sleep)
 */
bool game_state_is_valid(void);

/**
 * @brief Get pointer to the saved game state
 * @return Pointer to the game state structure (only valid if game_state_is_valid() returns true)
 */
const game_state_t *game_state_get(void);

/**
 * @brief Invalidate the saved game state (clear magic number)
 *
 * Call this after state has been restored to prevent re-loading stale data.
 */
void game_state_invalidate(void);

#ifdef __cplusplus
}
#endif
