#pragma once

#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#if BOARD_WAVESHARE_AMOLED_216
#include <XPowersLib.h>
#endif
#include <SensorQMI8658.hpp>
#include <Wire.h>

#if !defined(BOARD_WAVESHARE_AMOLED_216) && !defined(BOARD_WAVESHARE_LCD_349)
#define BOARD_WAVESHARE_AMOLED_216 1
#endif
#if !defined(BOARD_WAVESHARE_AMOLED_216)
#define BOARD_WAVESHARE_AMOLED_216 0
#endif
#if !defined(BOARD_WAVESHARE_LCD_349)
#define BOARD_WAVESHARE_LCD_349 0
#endif
#if !defined(BOARD_SELF_TEST)
#define BOARD_SELF_TEST 0
#endif
#if !defined(BOARD_CODEX_STRIP)
#define BOARD_CODEX_STRIP 0
#endif

#if BOARD_WAVESHARE_LCD_349

// ---- Display resolution (AXS15231B native portrait address space) ----
#define LCD_NATIVE_WIDTH   172
#define LCD_NATIVE_HEIGHT  640

// The final Codex pet strip is designed as a 640x172 landscape UI. The
// panel driver still receives 172x640 native portrait coordinates; main.cpp
// rotates each LVGL flush region into that address space.
#define LCD_WIDTH          LCD_NATIVE_HEIGHT
#define LCD_HEIGHT         LCD_NATIVE_WIDTH

// ---- QSPI display pins (AXS15231B) ----
#define LCD_CS      9
#define LCD_SCLK    10
#define LCD_SDIO0   11
#define LCD_SDIO1   12
#define LCD_SDIO2   13
#define LCD_SDIO3   14
#define LCD_RESET   21
#define LCD_BL      8

// ---- Touch pins (separate I2C, address 0x3B) ----
#define TOUCH_SDA   17
#define TOUCH_SCL   18
#define TOUCH_ADDR  0x3B

// ---- Sensor I2C pins (QMI8658/RTC bus) ----
#define SENSOR_SDA  47
#define SENSOR_SCL  48

// ---- Battery power hold / PWR key (Waveshare 07_BATT_PWR_Test) ----
#define PWR_KEY_PIN          16
#define TCA9554_ADDR         0x20
#define TCA9554_BAT_EN_PIN   6

#define BOARD_HAS_PMU 0
#define BOARD_HAS_TCA9554_POWER 1
#define BOARD_HAS_TOUCH_IRQ 0
#define BOARD_HAS_AUTO_ROTATION 0
#define BOARD_HAS_SIDE_BUTTONS 0
#define BOARD_HAS_LCD_PWM_BL 1

#else

// ---- Display resolution ----
#define LCD_WIDTH   480
#define LCD_HEIGHT  480

// ---- QSPI display pins (CO5300) ----
#define LCD_CS      12
#define LCD_SCLK    38
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_RESET   2

// ---- Touch pins (CST9220 via I2C) ----
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      11
#define TP_RST      2    // shared with LCD_RESET
#define CST9220_ADDR 0x5A

// ---- Sensor I2C pins (shared with touch + PMU) ----
#define SENSOR_SDA  IIC_SDA
#define SENSOR_SCL  IIC_SCL

// ---- PMU (AXP2101 via same I2C) ----
#define AXP2101_ADDR 0x34

#define BOARD_HAS_PMU 1
#define BOARD_HAS_TCA9554_POWER 0
#define BOARD_HAS_TOUCH_IRQ 1
#define BOARD_HAS_AUTO_ROTATION 1
#define BOARD_HAS_SIDE_BUTTONS 1
#define BOARD_HAS_LCD_PWM_BL 0

#endif

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus *bus;
extern Arduino_GFX *gfx;
#if BOARD_WAVESHARE_AMOLED_216
extern TouchDrvCST92xx touch;
extern XPowersPMU pmu;
#endif
extern SensorQMI8658 imu;
