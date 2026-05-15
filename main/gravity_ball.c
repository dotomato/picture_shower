/**
 * @file gravity_ball.c
 * @brief Gravity-responsive ball simulation using QMI8658 accelerometer
 *
 * A semi-transparent white circle with a border is displayed on screen.
 * The ball moves according to the device's tilt and bounces off screen edges.
 *
 * Architecture:
 *   - A FreeRTOS task reads IMU data and stores it in shared variables.
 *   - An LVGL timer (runs inside lv_timer_handler, lock already held)
 *     reads the shared accel data, computes physics, and updates ball position.
 */
#include "gravity_ball.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "qmi8658.h"
#include <math.h>

static const char *TAG = "gravity_ball";

/* ---- Configuration ---- */
#define BALL_RADIUS         25      /* Ball radius in pixels */
#define BALL_BORDER_WIDTH   3       /* Border width in pixels */
#define BALL_OPACITY        180     /* 0=transparent, 255=opaque (semi-transparent) */
#define SCREEN_WIDTH        480
#define SCREEN_HEIGHT       480

#define ACCEL_SCALE         3.0f    /* Acceleration to velocity scale factor */
#define VELOCITY_DAMPING    0.95f   /* Velocity damping per frame (friction) */
#define BOUNCE_FACTOR       0.6f    /* Velocity retained after bounce (0..1) */
#define SENSOR_READ_MS      10      /* Sensor read interval (~100 Hz) */
#define LVGL_TIMER_MS       16      /* LVGL timer period (~60 FPS) */
#define CALIBRATION_SAMPLES 100     /* Number of samples for calibration */
#define DEADZONE            0.15f   /* Ignore small accelerations (m/s^2) */

/* ---- State ---- */
static qmi8658_dev_t *s_imu_dev = NULL;
static lv_obj_t      *s_ball    = NULL;
static lv_timer_t    *s_lv_timer = NULL;
static TaskHandle_t   s_task_handle = NULL;
static bool           s_running = false;

/* Ball physics state (only accessed from LVGL timer context) */
static float s_pos_x, s_pos_y;     /* Center position */
static float s_vel_x, s_vel_y;     /* Velocity in px/frame */

/* Calibration bias (written by sensor task during calibration, then read-only) */
static float s_bias_x = 0.0f;
static float s_bias_y = 0.0f;

/* Shared accelerometer data (written by sensor task, read by LVGL timer) */
static volatile float s_accel_x = 0.0f;
static volatile float s_accel_y = 0.0f;
static volatile bool  s_accel_valid = false;
static volatile bool  s_calibrated = false;
static portMUX_TYPE   s_accel_mux = portMUX_INITIALIZER_UNLOCKED;

/* Debug counter (only in LVGL timer) */
static int s_dbg_cnt = 0;

/* ---- Calibration (runs in sensor task) ---- */
static void perform_calibration(void)
{
    qmi8658_data_t data;
    float sum_x = 0.0f, sum_y = 0.0f;
    int valid = 0;

    ESP_LOGI(TAG, "Calibrating accelerometer (%d samples)...", CALIBRATION_SAMPLES);

    for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
        bool ready = false;
        qmi8658_is_data_ready(s_imu_dev, &ready);
        if (ready && qmi8658_read_sensor_data(s_imu_dev, &data) == ESP_OK) {
            sum_x += data.accelX;
            sum_y += data.accelY;
            valid++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (valid > 0) {
        s_bias_x = sum_x / valid;
        s_bias_y = sum_y / valid;
    }
    ESP_LOGI(TAG, "Calibration done. bias_x=%.3f, bias_y=%.3f (%d samples)",
             s_bias_x, s_bias_y, valid);
}

/* ---- Create ball LVGL object on sys_layer (survives screen changes) ---- */
static void create_ball_obj(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) return;
    lv_obj_t *sys_layer = lv_display_get_layer_sys(disp);

    s_ball = lv_obj_create(sys_layer);
    lv_obj_remove_style_all(s_ball);

    /* Prevent any layout from overriding manual position */
    lv_obj_set_style_layout(sys_layer, 0, 0);
    lv_obj_add_flag(s_ball, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_ball, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SNAPPABLE);

    /* Size = diameter */
    int32_t diameter = BALL_RADIUS * 2;
    lv_obj_set_size(s_ball, diameter, diameter);

    /* Make it a circle */
    lv_obj_set_style_radius(s_ball, LV_RADIUS_CIRCLE, 0);

    /* Semi-transparent white background */
    lv_obj_set_style_bg_color(s_ball, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_ball, BALL_OPACITY, 0);

    /* Border */
    lv_obj_set_style_border_color(s_ball, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_ball, BALL_BORDER_WIDTH, 0);
    lv_obj_set_style_border_opa(s_ball, LV_OPA_COVER, 0);

    /* Initial position: center of screen */
    s_pos_x = SCREEN_WIDTH / 2.0f;
    s_pos_y = SCREEN_HEIGHT / 2.0f;
    s_vel_x = 0.0f;
    s_vel_y = 0.0f;

    lv_obj_set_pos(s_ball,
                   (int32_t)(s_pos_x - BALL_RADIUS),
                   (int32_t)(s_pos_y - BALL_RADIUS));

    ESP_LOGI(TAG, "Ball object created (r=%d, opacity=%d)", BALL_RADIUS, BALL_OPACITY);
}

/* ---- LVGL timer callback: physics + UI update (runs with LVGL lock held) ---- */
static void ball_physics_timer_cb(lv_timer_t *timer)
{
    if (!s_calibrated || !s_accel_valid || s_ball == NULL) {
        return;
    }

    /* Read shared accel data atomically */
    float ax, ay;
    portENTER_CRITICAL(&s_accel_mux);
    ax = s_accel_x;
    ay = s_accel_y;
    portEXIT_CRITICAL(&s_accel_mux);

    /* Debug: print every ~1 second (60 frames) */
    if (s_dbg_cnt++ % 60 == 0) {
        ESP_LOGI(TAG, "accel=(%.2f,%.2f) pos=(%.0f,%.0f) vel=(%.1f,%.1f)",
                 ax, ay, s_pos_x, s_pos_y, s_vel_x, s_vel_y);
    }

    /* Apply deadzone */
    if (fabsf(ax) < DEADZONE) ax = 0.0f;
    if (fabsf(ay) < DEADZONE) ay = 0.0f;

    /*
     * Map accelerometer axes to screen axes.
     * The board orientation: accelX -> screen horizontal (inverted),
     *                        accelY -> screen vertical.
     */
    float screen_ax = -ax * ACCEL_SCALE;
    float screen_ay =  ay * ACCEL_SCALE;

    /* Update velocity */
    s_vel_x += screen_ax;
    s_vel_y += screen_ay;

    /* Apply damping (friction) */
    s_vel_x *= VELOCITY_DAMPING;
    s_vel_y *= VELOCITY_DAMPING;

    /* Update position */
    s_pos_x += s_vel_x;
    s_pos_y += s_vel_y;

    /* Bounce off screen edges */
    float min_pos = (float)BALL_RADIUS;
    float max_x   = (float)(SCREEN_WIDTH  - BALL_RADIUS);
    float max_y   = (float)(SCREEN_HEIGHT - BALL_RADIUS);

    if (s_pos_x < min_pos) {
        s_pos_x = min_pos;
        s_vel_x = -s_vel_x * BOUNCE_FACTOR;
    } else if (s_pos_x > max_x) {
        s_pos_x = max_x;
        s_vel_x = -s_vel_x * BOUNCE_FACTOR;
    }

    if (s_pos_y < min_pos) {
        s_pos_y = min_pos;
        s_vel_y = -s_vel_y * BOUNCE_FACTOR;
    } else if (s_pos_y > max_y) {
        s_pos_y = max_y;
        s_vel_y = -s_vel_y * BOUNCE_FACTOR;
    }

    /* Update LVGL object position (no lock needed, we're inside lv_timer_handler) */
    lv_obj_set_pos(s_ball,
                   (int32_t)(s_pos_x - BALL_RADIUS),
                   (int32_t)(s_pos_y - BALL_RADIUS));
}

/* ---- Sensor reading task (only reads IMU, no LVGL calls) ---- */
static void sensor_read_task(void *arg)
{
    qmi8658_data_t data;

    /* Wait a bit for sensor to stabilize, then calibrate */
    vTaskDelay(pdMS_TO_TICKS(500));
    perform_calibration();
    s_calibrated = true;

    while (s_running) {
        esp_err_t ret = qmi8658_read_sensor_data(s_imu_dev, &data);
        if (ret == ESP_OK) {
            /* Apply calibration bias */
            float ax = data.accelX - s_bias_x;
            float ay = data.accelY - s_bias_y;

            /* Store to shared variables atomically */
            portENTER_CRITICAL(&s_accel_mux);
            s_accel_x = ax;
            s_accel_y = ay;
            s_accel_valid = true;
            portEXIT_CRITICAL(&s_accel_mux);
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_MS));
    }

    ESP_LOGI(TAG, "Sensor read task exiting");
    vTaskDelete(NULL);
}

/* ---- Public API ---- */

esp_err_t gravity_ball_init(i2c_master_bus_handle_t bus_handle)
{
    /* Allocate QMI8658 device struct */
    s_imu_dev = malloc(sizeof(qmi8658_dev_t));
    if (s_imu_dev == NULL) {
        ESP_LOGE(TAG, "Failed to allocate qmi8658_dev_t");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize QMI8658 */
    esp_err_t ret = qmi8658_init(s_imu_dev, bus_handle, QMI8658_ADDRESS_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "qmi8658_init failed: %s", esp_err_to_name(ret));
        free(s_imu_dev);
        s_imu_dev = NULL;
        return ret;
    }

    /* Configure accelerometer */
    qmi8658_set_accel_range(s_imu_dev, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(s_imu_dev, QMI8658_ACCEL_ODR_500HZ);
    qmi8658_set_accel_unit_mps2(s_imu_dev, true);

    /* Enable low-pass filter for smoother data */
    qmi8658_write_register(s_imu_dev, QMI8658_CTRL5, 0x03);

    ESP_LOGI(TAG, "QMI8658 initialized successfully");

    /* Create ball UI object (must be called with LVGL lock held) */
    create_ball_obj();

    /* Create LVGL timer for physics + UI update (runs inside lv_timer_handler) */
    s_lv_timer = lv_timer_create(ball_physics_timer_cb, LVGL_TIMER_MS, NULL);
    if (s_lv_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL timer");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void gravity_ball_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "Gravity ball already running");
        return;
    }
    if (s_imu_dev == NULL) {
        ESP_LOGE(TAG, "IMU not initialized, call gravity_ball_init first");
        return;
    }

    s_running = true;

    /* Create sensor reading task with stack in PSRAM to save internal RAM */
    StaticTask_t *task_buf = heap_caps_malloc(sizeof(StaticTask_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StackType_t  *stack_buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (task_buf == NULL || stack_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate task buffers");
        if (task_buf)  heap_caps_free(task_buf);
        if (stack_buf) heap_caps_free(stack_buf);
        s_running = false;
        return;
    }

    s_task_handle = xTaskCreateStatic(sensor_read_task, "imu_reader",
                                      4096, NULL, 2,
                                      stack_buf, task_buf);
    if (s_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor read task");
        heap_caps_free(task_buf);
        heap_caps_free(stack_buf);
        s_running = false;
    } else {
        ESP_LOGI(TAG, "Gravity ball started (sensor task + LVGL timer)");
    }
}

void gravity_ball_stop(void)
{
    s_running = false;
    /* Sensor task will self-delete when it sees s_running == false */
    s_task_handle = NULL;

    /* Delete LVGL timer and ball object (caller must hold LVGL lock) */
    if (s_lv_timer != NULL) {
        lv_timer_del(s_lv_timer);
        s_lv_timer = NULL;
    }

    if (s_ball != NULL) {
        lv_obj_del(s_ball);
        s_ball = NULL;
    }

    if (s_imu_dev != NULL) {
        free(s_imu_dev);
        s_imu_dev = NULL;
    }

    s_calibrated = false;
    s_accel_valid = false;

    ESP_LOGI(TAG, "Gravity ball stopped");
}

void gravity_ball_get_position(float *x, float *y)
{
    if (x) *x = s_pos_x;
    if (y) *y = s_pos_y;
}

int gravity_ball_get_radius(void)
{
    return BALL_RADIUS;
}
