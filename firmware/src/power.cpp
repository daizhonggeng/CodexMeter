#include "power.h"
#include "display_cfg.h"
#include <Arduino.h>

// Poll intervals
#define BATTERY_POLL_MS   2000
#define CHARGING_POLL_MS  500

static int      cached_pct      = -1;
static bool     cached_charging = false;
static bool     pwr_pressed_flag = false;
static bool     shutdown_requested_flag = false;
static uint32_t last_battery_ms  = 0;
static uint32_t last_charging_ms = 0;
static uint32_t last_pwr_ms      = 0;
#define PWR_POLL_MS 50
#define PWR_SHUTDOWN_HOLD_MS 3000

#if BOARD_HAS_TCA9554_POWER
#define TCA9554_REG_OUTPUT   0x01
#define TCA9554_REG_CONFIG   0x03

static uint8_t tca_output_shadow = 0xff;
static uint8_t tca_config_shadow = 0xff;
static bool    tca_ready = false;

static bool tca_read_reg(uint8_t reg, uint8_t *value) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(TCA9554_ADDR, (uint8_t)1) != 1) return false;
    *value = Wire.read();
    return true;
}

static bool tca_write_reg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static bool tca_set_pin(uint8_t pin, bool level) {
    if (!tca_ready) return false;
    if (level) tca_output_shadow |= (uint8_t)(1U << pin);
    else       tca_output_shadow &= (uint8_t)~(1U << pin);
    return tca_write_reg(TCA9554_REG_OUTPUT, tca_output_shadow);
}

static void tca9554_power_init(void) {
    pinMode(PWR_KEY_PIN, INPUT_PULLUP);

    if (!tca_read_reg(TCA9554_REG_OUTPUT, &tca_output_shadow)) {
        Serial.println("TCA9554 output read failed");
        return;
    }
    if (!tca_read_reg(TCA9554_REG_CONFIG, &tca_config_shadow)) {
        Serial.println("TCA9554 config read failed");
        return;
    }

    tca_ready = true;
    tca_output_shadow |= (uint8_t)(1U << TCA9554_BAT_EN_PIN);
    tca_config_shadow &= (uint8_t)~(1U << TCA9554_BAT_EN_PIN);
    tca_write_reg(TCA9554_REG_OUTPUT, tca_output_shadow);
    tca_write_reg(TCA9554_REG_CONFIG, tca_config_shadow);
    Serial.println("TCA9554 battery hold enabled");
}

static void tca9554_power_tick(uint32_t now) {
    static bool was_down = false;
    static bool long_fired = false;
    static uint32_t down_started_ms = 0;

    bool down = digitalRead(PWR_KEY_PIN) == LOW;
    if (down && !was_down) {
        down_started_ms = now;
        long_fired = false;
    } else if (!down && was_down) {
        down_started_ms = 0;
        long_fired = false;
    }

    if (down && !long_fired && now - down_started_ms >= PWR_SHUTDOWN_HOLD_MS) {
        shutdown_requested_flag = true;
        long_fired = true;
    }
    was_down = down;
}
#endif

void power_init(void) {
#if !BOARD_HAS_PMU
    cached_pct = -1;
    cached_charging = false;
    Serial.println("PMU not present on this board profile");
#if BOARD_HAS_TCA9554_POWER
    tca9554_power_init();
#endif
    return;
#else
    if (!pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 init failed");
        return;
    }
    Serial.println("AXP2101 init OK");

    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();

    // Enable PWR button short-press IRQ (mid button for cycling screens)
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);

    cached_charging = pmu.isCharging();
    cached_pct = pmu.getBatteryPercent();
#endif
}

void power_tick(void) {
    uint32_t now = millis();
#if BOARD_HAS_TCA9554_POWER
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        tca9554_power_tick(now);
    }
#endif
#if !BOARD_HAS_PMU
    return;
#else
    if (now - last_charging_ms >= CHARGING_POLL_MS) {
        last_charging_ms = now;
        cached_charging = pmu.isCharging();
    }

    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        cached_pct = pmu.getBatteryPercent();
    }

    // Poll PWR button (AXP2101 short-press IRQ)
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        pmu.getIrqStatus();
        if (pmu.isPekeyShortPressIrq()) {
            pwr_pressed_flag = true;
        }
        pmu.clearIrqStatus();
    }
#endif
}

int power_battery_pct(void) {
    return cached_pct;
}

bool power_is_charging(void) {
    return cached_charging;
}

bool power_pwr_pressed(void) {
    if (pwr_pressed_flag) {
        pwr_pressed_flag = false;
        return true;
    }
    return false;
}

bool power_shutdown_requested(void) {
    if (shutdown_requested_flag) {
        shutdown_requested_flag = false;
        return true;
    }
    return false;
}

void power_shutdown_now(void) {
#if BOARD_HAS_TCA9554_POWER
    Serial.println("Releasing TCA9554 battery hold");
    tca_set_pin(TCA9554_BAT_EN_PIN, false);
#endif
}
