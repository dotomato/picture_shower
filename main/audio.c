/**
 * @file audio.c
 * @brief Audio module implementation - synthesized collision sound effects
 *
 * Generates a short "tick/hit" sound using additive synthesis (decaying sine waves)
 * and plays it through the ES8311 codec via I2S.
 * A dedicated FreeRTOS task handles playback to avoid blocking the caller.
 */
#include "audio.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "audio";

/* Audio parameters */
#define SAMPLE_RATE     22050
#define HIT_DURATION_MS 30       /* Very short click/tick sound */
#define HIT_SAMPLES     (SAMPLE_RATE * HIT_DURATION_MS / 1000)  /* ~661 samples */
#define HIT_BUF_BYTES   (HIT_SAMPLES * sizeof(int16_t))

/* State */
static esp_codec_dev_handle_t s_spk_dev = NULL;
static int16_t *s_hit_pcm = NULL;          /* Pre-generated hit sound PCM buffer */
static volatile bool s_playing = false;
static TaskHandle_t s_play_task = NULL;
static SemaphoreHandle_t s_play_sem = NULL;
static bool s_initialized = false;

/**
 * @brief Generate a short percussive "tick" sound into the PCM buffer.
 *
 * Uses a mix of frequencies with fast exponential decay to simulate
 * a brief metallic/wooden hit sound.
 */
static void generate_hit_sound(int16_t *buf, int num_samples)
{
    /* Frequencies for the hit (Hz) - creates a percussive timbre */
    const float freqs[] = {800.0f, 1200.0f, 2400.0f, 3600.0f};
    const float amps[]  = {0.4f,   0.3f,    0.2f,    0.1f};
    const int num_freqs = sizeof(freqs) / sizeof(freqs[0]);

    /* Decay time constant (in samples) - very fast decay for a "tick" */
    const float decay_tau = (float)num_samples * 0.25f;

    for (int i = 0; i < num_samples; i++) {
        float t = (float)i / (float)SAMPLE_RATE;
        float envelope = expf(-(float)i / decay_tau);

        float sample = 0.0f;
        for (int f = 0; f < num_freqs; f++) {
            sample += amps[f] * sinf(2.0f * M_PI * freqs[f] * t);
        }
        sample *= envelope;

        /* Scale to int16 range with some headroom */
        int32_t val = (int32_t)(sample * 24000.0f);
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        buf[i] = (int16_t)val;
    }
}

/**
 * @brief Playback task - waits for semaphore, then writes PCM to codec.
 */
static void audio_play_task(void *arg)
{
    while (1) {
        /* Wait for play request */
        if (xSemaphoreTake(s_play_sem, portMAX_DELAY) == pdTRUE) {
            if (s_spk_dev == NULL || s_hit_pcm == NULL) continue;

            s_playing = true;
            esp_codec_dev_write(s_spk_dev, s_hit_pcm, HIT_BUF_BYTES);
            s_playing = false;
        }
    }
}

esp_err_t audio_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing audio (ES8311 codec)...");

    /* Initialize speaker codec */
    s_spk_dev = bsp_audio_codec_speaker_init();
    if (s_spk_dev == NULL) {
        ESP_LOGE(TAG, "Failed to initialize speaker codec");
        return ESP_FAIL;
    }

    /* Set volume */
    esp_codec_dev_set_out_vol(s_spk_dev, 70);

    /* Open codec with our sample format */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    esp_err_t ret = esp_codec_dev_open(s_spk_dev, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open codec device: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Allocate PCM buffer in PSRAM and generate the hit sound */
    s_hit_pcm = heap_caps_malloc(HIT_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (s_hit_pcm == NULL) {
        ESP_LOGE(TAG, "Failed to allocate hit PCM buffer");
        return ESP_ERR_NO_MEM;
    }
    generate_hit_sound(s_hit_pcm, HIT_SAMPLES);
    ESP_LOGI(TAG, "Hit sound generated: %d samples, %d bytes", HIT_SAMPLES, HIT_BUF_BYTES);

    /* Create playback semaphore and task */
    s_play_sem = xSemaphoreCreateBinary();
    if (s_play_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create play semaphore");
        return ESP_ERR_NO_MEM;
    }

    /* Create task with stack in PSRAM to save internal RAM */
    StaticTask_t *task_buf = heap_caps_malloc(sizeof(StaticTask_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StackType_t *stack_buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (task_buf == NULL || stack_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate audio task buffers");
        return ESP_ERR_NO_MEM;
    }

    s_play_task = xTaskCreateStatic(audio_play_task, "audio_play",
                                    4096, NULL, 4,
                                    stack_buf, task_buf);
    if (s_play_task == NULL) {
        ESP_LOGE(TAG, "Failed to create audio play task");
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Audio initialized successfully");
    return ESP_OK;
}

void audio_play_hit(void)
{
    if (!s_initialized || s_play_sem == NULL) return;

    /* If already playing, skip (don't queue up sounds) */
    if (s_playing) return;

    xSemaphoreGive(s_play_sem);
}
