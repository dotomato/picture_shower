/**
 * @file game_state.c
 * @brief Game state save/load using RTC RAM
 *
 * Stores game state in RTC slow memory which persists across deep sleep cycles.
 */
#include "game_state.h"
#include "ui_screens.h"
#include "gravity_ball.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <string.h>

static const char *TAG = "game_state";

/* Magic number to validate RTC RAM data */
#define GAME_STATE_MAGIC 0xA5B4C3D2

/* Game state stored in RTC slow memory (persists across deep sleep) */
static RTC_DATA_ATTR game_state_t s_rtc_state;

void game_state_save(void)
{
    /* Collect image index and tile state from ui_screens */
    s_rtc_state.pic_index = ui_get_pic_index();
    ui_get_tile_revealed(s_rtc_state.tile_revealed, GAME_STATE_TILE_COUNT);

    /* Collect ball position and velocity from gravity_ball */
    gravity_ball_get_position(&s_rtc_state.ball_pos_x, &s_rtc_state.ball_pos_y);
    gravity_ball_get_velocity(&s_rtc_state.ball_vel_x, &s_rtc_state.ball_vel_y);

    /* Mark as valid */
    s_rtc_state.magic = GAME_STATE_MAGIC;

    ESP_LOGI(TAG, "Game state saved: pic_index=%d, ball=(%.0f,%.0f)",
             s_rtc_state.pic_index, s_rtc_state.ball_pos_x, s_rtc_state.ball_pos_y);
}

bool game_state_is_valid(void)
{
    return (s_rtc_state.magic == GAME_STATE_MAGIC);
}

const game_state_t *game_state_get(void)
{
    return &s_rtc_state;
}

void game_state_invalidate(void)
{
    s_rtc_state.magic = 0;
    ESP_LOGI(TAG, "Game state invalidated");
}
