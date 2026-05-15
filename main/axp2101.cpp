/**
 * @file axp2101.cpp
 * @brief AXP2101 Power Management IC driver using XPowersLib
 *
 * Provides full PMU initialization, charging configuration,
 * ADC measurement, and battery status via XPowersLib C++ library.
 * Exposes C-compatible API declared in axp2101.h.
 */
#include <cstring>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

extern "C" {
#include "axp2101.h"
}

static const char *TAG = "AXP2101";

static XPowersPMU PMU;
static bool s_pmu_ok = false;
static i2c_master_dev_handle_t s_pmu_dev = NULL;

/* ── I2C read/write callbacks for XPowersLib ── */

static int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    if (s_pmu_dev == NULL) return -1;
    esp_err_t ret = i2c_master_transmit_receive(s_pmu_dev, &regAddr, 1, data, len, 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed: reg=0x%02X err=%s", regAddr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

static int pmu_register_write(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    if (s_pmu_dev == NULL) return -1;
    uint8_t *buf = (uint8_t *)malloc(len + 1);
    if (!buf) return -1;
    buf[0] = regAddr;
    memcpy(&buf[1], data, len);
    esp_err_t ret = i2c_master_transmit(s_pmu_dev, buf, len + 1, 100);
    free(buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed: reg=0x%02X err=%s", regAddr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

/* ── Public C API ── */

void axp2101_init(void)
{
    /* Get BSP I2C bus handle */
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "I2C bus not ready");
        return;
    }

    /* Add AXP2101 as I2C device */
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = AXP2101_SLAVE_ADDRESS;
    dev_cfg.scl_speed_hz    = 400000;

    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_pmu_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device (0x%02X): %s",
                 AXP2101_SLAVE_ADDRESS, esp_err_to_name(err));
        return;
    }

    /* Initialize XPowersLib PMU */
    if (!PMU.begin(AXP2101_SLAVE_ADDRESS, pmu_register_read, pmu_register_write)) {
        ESP_LOGE(TAG, "PMU init FAILED!");
        i2c_master_bus_rm_device(s_pmu_dev);
        s_pmu_dev = NULL;
        return;
    }
    ESP_LOGI(TAG, "PMU init SUCCESS!");

    /* Print all power channel status */
    ESP_LOGI(TAG, "=== DCDC Channels ===");
    ESP_LOGI(TAG, "DC1: %s  %u mV", PMU.isEnableDC1() ? "ON " : "OFF", PMU.getDC1Voltage());
    ESP_LOGI(TAG, "DC2: %s  %u mV", PMU.isEnableDC2() ? "ON " : "OFF", PMU.getDC2Voltage());
    ESP_LOGI(TAG, "DC3: %s  %u mV", PMU.isEnableDC3() ? "ON " : "OFF", PMU.getDC3Voltage());
    ESP_LOGI(TAG, "DC4: %s  %u mV", PMU.isEnableDC4() ? "ON " : "OFF", PMU.getDC4Voltage());
    ESP_LOGI(TAG, "DC5: %s  %u mV", PMU.isEnableDC5() ? "ON " : "OFF", PMU.getDC5Voltage());
    ESP_LOGI(TAG, "=== ALDO Channels ===");
    ESP_LOGI(TAG, "ALDO1: %s  %u mV", PMU.isEnableALDO1() ? "ON " : "OFF", PMU.getALDO1Voltage());
    ESP_LOGI(TAG, "ALDO2: %s  %u mV", PMU.isEnableALDO2() ? "ON " : "OFF", PMU.getALDO2Voltage());
    ESP_LOGI(TAG, "ALDO3: %s  %u mV", PMU.isEnableALDO3() ? "ON " : "OFF", PMU.getALDO3Voltage());
    ESP_LOGI(TAG, "ALDO4: %s  %u mV", PMU.isEnableALDO4() ? "ON " : "OFF", PMU.getALDO4Voltage());
    ESP_LOGI(TAG, "=== BLDO Channels ===");
    ESP_LOGI(TAG, "BLDO1: %s  %u mV", PMU.isEnableBLDO1() ? "ON " : "OFF", PMU.getBLDO1Voltage());
    ESP_LOGI(TAG, "BLDO2: %s  %u mV", PMU.isEnableBLDO2() ? "ON " : "OFF", PMU.getBLDO2Voltage());

    /* Enable ADC measurements */
    PMU.enableVbusVoltageMeasure();
    PMU.enableBattVoltageMeasure();
    PMU.enableSystemVoltageMeasure();
    PMU.enableTemperatureMeasure();

    /*
     * IMPORTANT: Disable TS pin measurement on boards without battery
     * temperature sensor, otherwise charging will be abnormal.
     */
    PMU.disableTSPinMeasure();

    /* Disable all interrupts, then enable only what we need */
    PMU.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    PMU.clearIrqStatus();
    PMU.enableIRQ(
        XPOWERS_AXP2101_BAT_INSERT_IRQ    | XPOWERS_AXP2101_BAT_REMOVE_IRQ   |
        XPOWERS_AXP2101_VBUS_INSERT_IRQ   | XPOWERS_AXP2101_VBUS_REMOVE_IRQ  |
        XPOWERS_AXP2101_PKEY_SHORT_IRQ    | XPOWERS_AXP2101_PKEY_LONG_IRQ    |
        XPOWERS_AXP2101_BAT_CHG_DONE_IRQ  | XPOWERS_AXP2101_BAT_CHG_START_IRQ
    );

    /* Configure charging parameters */
    PMU.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    PMU.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
    PMU.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    PMU.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

    ESP_LOGI(TAG, "Battery: %d%%", PMU.getBatteryPercent());

    s_pmu_ok = true;
}

int battery_get_percent(void)
{
    if (!s_pmu_ok) return -1;
    int pct = PMU.getBatteryPercent();
    return (pct > 100) ? 100 : pct;
}

bool battery_is_charging(void)
{
    if (!s_pmu_ok) return false;
    return PMU.isCharging();
}

int battery_get_voltage(void)
{
    if (!s_pmu_ok) return -1;
    return (int)PMU.getBattVoltage();
}

int battery_get_vbus_voltage(void)
{
    if (!s_pmu_ok) return -1;
    return (int)PMU.getVbusVoltage();
}

int battery_get_sys_voltage(void)
{
    if (!s_pmu_ok) return -1;
    return (int)PMU.getSystemVoltage();
}

float battery_get_temperature(void)
{
    if (!s_pmu_ok) return -999.0f;
    return PMU.getTemperature();
}

int battery_get_charger_status(void)
{
    if (!s_pmu_ok) return -1;
    return (int)PMU.getChargerStatus();
}

bool battery_is_vbus_in(void)
{
    if (!s_pmu_ok) return false;
    return PMU.isVbusIn();
}

bool battery_is_battery_connected(void)
{
    if (!s_pmu_ok) return false;
    return PMU.isBatteryConnect();
}

void axp2101_dump_status(void)
{
    if (!s_pmu_ok) {
        ESP_LOGW(TAG, "PMU not initialized");
        return;
    }

    PMU.getIrqStatus();

    ESP_LOGI(TAG, "--- PMU Status ---");
    ESP_LOGI(TAG, "Temperature: %.2f C", PMU.getTemperature());
    ESP_LOGI(TAG, "Charging: %s", PMU.isCharging() ? "YES" : "NO");
    ESP_LOGI(TAG, "Discharge: %s", PMU.isDischarge() ? "YES" : "NO");
    ESP_LOGI(TAG, "Standby: %s", PMU.isStandby() ? "YES" : "NO");
    ESP_LOGI(TAG, "VBUS In: %s", PMU.isVbusIn() ? "YES" : "NO");
    ESP_LOGI(TAG, "VBUS Good: %s", PMU.isVbusGood() ? "YES" : "NO");

    uint8_t chg = PMU.getChargerStatus();
    const char *chg_str = "unknown";
    switch (chg) {
        case XPOWERS_AXP2101_CHG_TRI_STATE:  chg_str = "tri_charge";       break;
        case XPOWERS_AXP2101_CHG_PRE_STATE:  chg_str = "pre_charge";       break;
        case XPOWERS_AXP2101_CHG_CC_STATE:   chg_str = "constant_current"; break;
        case XPOWERS_AXP2101_CHG_CV_STATE:   chg_str = "constant_voltage"; break;
        case XPOWERS_AXP2101_CHG_DONE_STATE: chg_str = "charge_done";      break;
        case XPOWERS_AXP2101_CHG_STOP_STATE: chg_str = "not_charging";     break;
    }
    ESP_LOGI(TAG, "Charger: %s", chg_str);
    ESP_LOGI(TAG, "Batt Voltage: %d mV", PMU.getBattVoltage());
    ESP_LOGI(TAG, "VBUS Voltage: %d mV", PMU.getVbusVoltage());
    ESP_LOGI(TAG, "Sys  Voltage: %d mV", PMU.getSystemVoltage());

    if (PMU.isBatteryConnect()) {
        ESP_LOGI(TAG, "Battery: %d%%", PMU.getBatteryPercent());
    } else {
        ESP_LOGI(TAG, "Battery: not connected");
    }

    PMU.clearIrqStatus();
}
