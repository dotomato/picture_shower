/**
 * @file axp2101.c
 * @brief AXP2101 Power Management IC – lightweight I2C driver
 */
#include "axp2101.h"

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "axp2101";

#define AXP2101_I2C_ADDR        0x34

/* Key register addresses */
#define AXP2101_REG_STATUS1     0x00   /* Power status (charging, VBUS, etc.) */
#define AXP2101_REG_STATUS2     0x01   /* Charging status detail */
#define AXP2101_REG_BAT_PERCENT 0xA4   /* Battery percentage (0-100) */

static i2c_master_dev_handle_t s_axp2101_dev = NULL;
static bool s_axp2101_ok = false;

/**
 * Read a single register from AXP2101.
 */
static esp_err_t axp2101_read_reg(uint8_t reg, uint8_t *out)
{
    if (!s_axp2101_ok || s_axp2101_dev == NULL) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_axp2101_dev, &reg, 1, out, 1, 100);
}

void axp2101_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "AXP2101: I2C bus not ready");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_axp2101_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101: failed to add I2C device (0x%02X): %s",
                 AXP2101_I2C_ADDR, esp_err_to_name(err));
        return;
    }

    /* Quick probe: read STATUS1 register to verify chip is present */
    uint8_t reg = AXP2101_REG_STATUS1;
    uint8_t val = 0;
    err = i2c_master_transmit_receive(s_axp2101_dev, &reg, 1, &val, 1, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101: chip not responding at 0x%02X", AXP2101_I2C_ADDR);
        i2c_master_bus_rm_device(s_axp2101_dev);
        s_axp2101_dev = NULL;
        return;
    }

    s_axp2101_ok = true;
    ESP_LOGI(TAG, "AXP2101: initialized OK (STATUS1=0x%02X)", val);
}

int battery_get_percent(void)
{
    uint8_t pct = 0;
    if (axp2101_read_reg(AXP2101_REG_BAT_PERCENT, &pct) != ESP_OK) {
        return -1;
    }
    return (pct > 100) ? 100 : (int)pct;
}

bool battery_is_charging(void)
{
    uint8_t status = 0;
    if (axp2101_read_reg(AXP2101_REG_STATUS1, &status) != ESP_OK) {
        return false;
    }
    /* Bit 5 of STATUS1: VBUS good indicator */
    return (status & (1 << 5)) != 0;
}
