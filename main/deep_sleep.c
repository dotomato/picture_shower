/**
 * @file deep_sleep.c
 * @brief Deep sleep management implementation
 *
 * Monitors GPIO16 button press to enter deep sleep.
 * Turns off the display before sleeping and turns it on after wakeup.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "bsp/display.h"

#include "deep_sleep.h"

#define DEEP_SLEEP_GPIO_PIN  GPIO_NUM_16

static const char *TAG = "deep_sleep";

static QueueHandle_t s_gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(s_gpio_evt_queue, &gpio_num, NULL);
}

static void deep_sleep_task(void *arg)
{
    uint32_t io_num;
    for (;;) {
        if (xQueueReceive(s_gpio_evt_queue, &io_num, portMAX_DELAY)) {
            int level = gpio_get_level(DEEP_SLEEP_GPIO_PIN);
            if (level == 0) {
                ESP_LOGI(TAG, "GPIO16 button RELEASED");
            } else {
                ESP_LOGI(TAG, "GPIO16 button PRESSED - waiting for release before deep sleep...");

                /* Wait for button release (GPIO16 goes low) */
                while (gpio_get_level(DEEP_SLEEP_GPIO_PIN) == 1) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }

                ESP_LOGI(TAG, "GPIO16 button RELEASED - turning off display and entering deep sleep");

                /* Turn off display backlight before sleeping */
                bsp_display_backlight_off();
                vTaskDelay(pdMS_TO_TICKS(50));

                /* Configure GPIO16 as EXT1 wakeup source (wake on high level = button press) */
                esp_sleep_enable_ext1_wakeup_io(1ULL << DEEP_SLEEP_GPIO_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);

                /* Short delay to allow log to flush */
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_deep_sleep_start();
            }
        }
    }
}

void deep_sleep_init(void)
{
    /* Turn on display backlight (in case waking up from deep sleep) */
    bsp_display_backlight_on();

    /* Configure GPIO16 as input with pull-up, interrupt on any edge */
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_ANYEDGE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << DEEP_SLEEP_GPIO_PIN),
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);

    /* Create event queue and task */
    s_gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(deep_sleep_task, "deep_sleep_task", 2048, NULL, 10, NULL);

    /* Install ISR service and add handler */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(DEEP_SLEEP_GPIO_PIN, gpio_isr_handler, (void *)DEEP_SLEEP_GPIO_PIN);

    ESP_LOGI(TAG, "Deep sleep module initialized (GPIO16, ANYEDGE, pull-up)");
}
