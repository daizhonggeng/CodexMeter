#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#if BOARD_CODEX_STRIP
#include <FS.h>
#include <LittleFS.h>
#endif
#if BOARD_WAVESHARE_LCD_349
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "axs15231b/esp_lcd_axs15231b.h"
#endif
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "power.h"
#include "imu.h"
#include "splash.h"
#include "usage_rate.h"
#include "pet_miku.h"

// Physical buttons on the strip board:
//   BTN_BACK   (GPIO 0)  — BOOT short press cycles sessions; long press opens recent sessions
//   BTN_FWD    (GPIO 18) — reserved on this build
//   AXP PWR    (PMU)     — long press shuts the device down
#define BTN_BACK 0
#define BTN_FWD  18

// ---- LVGL draw buffers (PSRAM-backed, partial render) ----
#define BUF_LINES 40

// ---- Hardware objects ----
#if BOARD_WAVESHARE_LCD_349
Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;
#else
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */,
    LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
#endif
#if BOARD_WAVESHARE_AMOLED_216
TouchDrvCST92xx touch;
XPowersPMU pmu;
#endif
SensorQMI8658 imu;

static UsageData usage = {};

#if BOARD_WAVESHARE_LCD_349
static esp_lcd_panel_handle_t lcd_panel = nullptr;
static SemaphoreHandle_t lcd_flush_done = nullptr;
static uint16_t *lcd_dma_buf = nullptr;
static const uint8_t lcd_no_params[] = {0x00};

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, lcd_no_params, 0, 100},
    {0x29, lcd_no_params, 0, 100},
};

static bool lcd_on_color_done(esp_lcd_panel_io_handle_t panel_io,
                              esp_lcd_panel_io_event_data_t *edata,
                              void *user_ctx) {
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    BaseType_t task_woken = pdFALSE;
    if (lcd_flush_done) {
        xSemaphoreGiveFromISR(lcd_flush_done, &task_woken);
    }
    return task_woken == pdTRUE;
}

static uint16_t swap_rgb565_byte(uint16_t pixel) {
    return (pixel << 8) | (pixel >> 8);
}

static void lcd_wait_color_done() {
    if (lcd_flush_done) {
        xSemaphoreTake(lcd_flush_done, pdMS_TO_TICKS(500));
    }
}

static void lcd_draw_native_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                                   const uint16_t *pixels) {
    if (!lcd_panel) return;
    if (lcd_flush_done) {
        xSemaphoreTake(lcd_flush_done, 0);
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(lcd_panel, x, y, x + w, y + h, pixels);
    if (err == ESP_OK) {
        lcd_wait_color_done();
    }
}

static void lcd_clear_native(uint16_t rgb565) {
    if (!lcd_panel || !lcd_dma_buf) return;

    const int32_t rows_per_chunk = BUF_LINES;
    for (int32_t y = 0; y < LCD_NATIVE_HEIGHT; y += rows_per_chunk) {
        int32_t rows = LCD_NATIVE_HEIGHT - y;
        if (rows > rows_per_chunk) rows = rows_per_chunk;
        int32_t count = LCD_NATIVE_WIDTH * rows;
        for (int32_t i = 0; i < count; i++) {
            lcd_dma_buf[i] = rgb565;
        }
        lcd_draw_native_bitmap(0, y, LCD_NATIVE_WIDTH, rows, lcd_dma_buf);
        vTaskDelay(1);
    }
}

static void board_display_init() {
    lcd_flush_done = xSemaphoreCreateBinary();
    if (!lcd_flush_done) {
        Serial.println("LCD semaphore alloc failed");
        return;
    }

    gpio_config_t rst_conf = {};
    rst_conf.intr_type = GPIO_INTR_DISABLE;
    rst_conf.mode = GPIO_MODE_OUTPUT;
    rst_conf.pin_bit_mask = 1ULL << LCD_RESET;
    rst_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    rst_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&rst_conf));

    spi_bus_config_t buscfg = {};
    buscfg.data0_io_num = LCD_SDIO0;
    buscfg.data1_io_num = LCD_SDIO1;
    buscfg.sclk_io_num = LCD_SCLK;
    buscfg.data2_io_num = LCD_SDIO2;
    buscfg.data3_io_num = LCD_SDIO3;
    buscfg.max_transfer_sz = LCD_NATIVE_WIDTH * BUF_LINES * 2;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = LCD_CS;
    io_config.dc_gpio_num = -1;
    io_config.spi_mode = 3;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = lcd_on_color_done;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((spi_host_device_t)SPI3_HOST, &io_config, &panel_io));

    axs15231b_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &lcd_panel));

    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)LCD_RESET, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)LCD_RESET, 0));
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)LCD_RESET, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
}

static void relax_task_watchdog_for_display() {
    esp_task_wdt_config_t config = {};
    config.timeout_ms = 120000;
    config.idle_core_mask = (1 << portNUM_PROCESSORS) - 1;
    config.trigger_panic = false;
    esp_err_t err = esp_task_wdt_reconfigure(&config);
    Serial.printf("Task WDT relaxed: %s\n", esp_err_to_name(err));
}
#endif

// ---- Touch interrupt + shared state ----
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;
static volatile bool     touch_data_ready = false;
static volatile uint16_t touch_raw_x = 0;
static volatile uint16_t touch_raw_y = 0;
static volatile uint32_t touch_event_count = 0;
static volatile uint32_t codex_touch_running_count = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

#if BOARD_WAVESHARE_LCD_349
static TwoWire TouchWire = TwoWire(1);

static void touch_read() {
    static uint32_t last_poll = 0;
    uint32_t now = millis();
    if (now - last_poll < 20) return;
    last_poll = now;

    static const uint8_t read_cmd[11] = {
        0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00
    };
    uint8_t buff[32] = {0};

    TouchWire.beginTransmission(TOUCH_ADDR);
    TouchWire.write(read_cmd, sizeof(read_cmd));
    if (TouchWire.endTransmission(false) != 0) {
        touch_pressed = false;
        return;
    }

    if (TouchWire.requestFrom(TOUCH_ADDR, (uint8_t)sizeof(buff)) != sizeof(buff)) {
        touch_pressed = false;
        return;
    }
    for (uint8_t i = 0; i < sizeof(buff); i++) buff[i] = TouchWire.read();

    if (buff[1] > 0 && buff[1] < 5) {
        uint16_t raw_x = (((uint16_t)buff[2] & 0x0f) << 8) | buff[3];
        uint16_t raw_y = (((uint16_t)buff[4] & 0x0f) << 8) | buff[5];
        if (raw_x > LCD_NATIVE_HEIGHT) raw_x = LCD_NATIVE_HEIGHT;
        if (raw_y > LCD_NATIVE_WIDTH) raw_y = LCD_NATIVE_WIDTH;
        touch_pressed = true;
        touch_raw_x = raw_x;
        touch_raw_y = raw_y;
        touch_x = LCD_NATIVE_HEIGHT - raw_x;
        touch_y = raw_y;
        touch_event_count++;
    } else {
        touch_pressed = false;
    }
}
#else
static void touch_read() {
    if (!touch_data_ready) return;
    touch_data_ready = false;

    int16_t tx[5], ty[5];
    uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
    if (n > 0) {
        touch_pressed = true;
        touch_x = (uint16_t)tx[0];
        touch_y = (uint16_t)ty[0];
    } else {
        touch_pressed = false;
    }
}
#endif

static void board_set_brightness(uint8_t brightness) {
#if BOARD_HAS_LCD_PWM_BL
    ledcWrite(LCD_BL, 255 - brightness);
#else
    static_cast<Arduino_CO5300*>(gfx)->setBrightness(brightness);
#endif
}

static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;
// rot_buf for strip rotation — max size is 480×480 (full invalidation case)
// but typical partial strips are much smaller
static uint16_t *rot_buf = nullptr;

// LVGL tick callback
static uint32_t my_tick(void) {
    return millis();
}

// Rotate a w×h strip and compute destination coordinates on the 480×480 display.
// src pixels are in row-major order for the rectangle (sx, sy, w, h).
// Output goes to rot_buf in row-major order for the destination rectangle.
static void rotate_strip(const uint16_t *src, int32_t w, int32_t h,
                         int32_t sx, int32_t sy, uint8_t r,
                         int32_t *dx, int32_t *dy, int32_t *dw, int32_t *dh) {
    const int S = LCD_WIDTH;  // 480

    switch (r) {
    case 1: { // 90° CW: (x,y) -> (S-1-y, x)
        *dw = h; *dh = w;
        *dx = S - sy - h;
        *dy = sx;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                // src(x,y) -> dst(h-1-y, x)
                rot_buf[x * h + (h - 1 - y)] = src[y * w + x];
            }
        }
        break;
    }
    case 2: { // 180°: (x,y) -> (S-1-x, S-1-y)
        *dw = w; *dh = h;
        *dx = S - sx - w;
        *dy = S - sy - h;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                rot_buf[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
            }
        }
        break;
    }
    case 3: { // 270° CW: (x,y) -> (y, S-1-x)
        *dw = h; *dh = w;
        *dx = sy;
        *dy = S - sx - w;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                // src(x,y) -> dst(y, w-1-x)
                rot_buf[(w - 1 - x) * h + y] = src[y * w + x];
            }
        }
        break;
    }
    default:
        *dx = sx; *dy = sy; *dw = w; *dh = h;
        break;
    }
}

// LVGL flush callback — rotates partial strips and writes to display
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint16_t *src = (uint16_t*)px_map;
#if BOARD_HAS_AUTO_ROTATION
    uint8_t r = imu_get_rotation();

    if (r == 0) {
        gfx->draw16bitRGBBitmap(area->x1, area->y1, src, w, h);
    } else {
        int32_t dx, dy, dw, dh;
        rotate_strip(src, w, h, area->x1, area->y1, r, &dx, &dy, &dw, &dh);
        gfx->draw16bitRGBBitmap(dx, dy, rot_buf, dw, dh);
    }
#else
#if BOARD_WAVESHARE_LCD_349
    if (lcd_panel && lcd_dma_buf) {
        (void)area;
        const int32_t rows_per_chunk = BUF_LINES;
        for (int32_t native_y = 0; native_y < LCD_NATIVE_HEIGHT; native_y += rows_per_chunk) {
            int32_t rows = LCD_NATIVE_HEIGHT - native_y;
            if (rows > rows_per_chunk) rows = rows_per_chunk;

            for (int32_t row = 0; row < rows; row++) {
                int32_t logical_x = native_y + row;
                for (int32_t native_x = 0; native_x < LCD_NATIVE_WIDTH; native_x++) {
                    int32_t logical_y = LCD_NATIVE_WIDTH - 1 - native_x;
                    lcd_dma_buf[row * LCD_NATIVE_WIDTH + native_x] =
                        swap_rgb565_byte(src[logical_y * LCD_WIDTH + logical_x]);
                }
            }

            lcd_draw_native_bitmap(0, native_y, LCD_NATIVE_WIDTH, rows, lcd_dma_buf);
            vTaskDelay(1);
        }
    }
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, src, w, h);
#endif
#endif
    lv_display_flush_ready(disp);
}

// CO5300 requires even-aligned flush regions
static void rounder_cb(lv_event_t* e) {
    lv_area_t *area = (lv_area_t*)lv_event_get_param(e);
    area->x1 = area->x1 & ~1;
    area->y1 = area->y1 & ~1;
    area->x2 = area->x2 | 1;
    area->y2 = area->y2 | 1;
}

// LVGL touch callback
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (touch_pressed) {
        data->point.x = touch_x;
        data->point.y = touch_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

#if BOARD_SELF_TEST || BOARD_CODEX_STRIP
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_cjk_18);

static lv_obj_t *touch_label = nullptr;
static lv_obj_t *codex_state_label = nullptr;
static lv_obj_t *codex_activity_label = nullptr;
static lv_obj_t *codex_pulse = nullptr;
static lv_obj_t *codex_session_pct_label = nullptr;
static lv_obj_t *codex_week_pct_label = nullptr;
static lv_obj_t *codex_session_reset_label = nullptr;
static lv_obj_t *codex_week_reset_label = nullptr;
static lv_obj_t *codex_status_label = nullptr;
static lv_obj_t *codex_session_card = nullptr;
static lv_obj_t *codex_week_card = nullptr;
static lv_obj_t *codex_session_arc = nullptr;
static lv_obj_t *codex_week_arc = nullptr;
static lv_obj_t *codex_pet_name_label = nullptr;
static lv_obj_t *codex_pet_head = nullptr;
static lv_obj_t *codex_pet_face = nullptr;
static lv_obj_t *codex_pet_body = nullptr;
static lv_obj_t *codex_pet_tail_l = nullptr;
static lv_obj_t *codex_pet_tail_r = nullptr;
static lv_obj_t *codex_pet_slot = nullptr;
static lv_obj_t *codex_pet_sprite = nullptr;
static uint8_t *codex_pet_sprite_pixels = nullptr;
static lv_image_dsc_t codex_pet_sprite_dsc = {};
#define CODEX_PET_MAX_ANIM_FRAMES 64
#define CODEX_PET_MAX_ATLAS_FRAMES 96
static uint16_t codex_pet_sprite_w = 0;
static uint16_t codex_pet_sprite_h = 0;
static uint8_t codex_pet_atlas_frames = 1;
static uint8_t codex_pet_anim_frames = 1;
static uint8_t codex_pet_anim_index = 0;
static uint8_t codex_pet_anim_loop_start = 0;
static uint32_t codex_pet_frame_size = 0;
static uint32_t codex_pet_anim_last_ms = 0;
static bool codex_pet_anim_paused = false;
static uint16_t codex_pet_frame_durations[CODEX_PET_MAX_ANIM_FRAMES] = {180};
static uint8_t codex_pet_anim_frame_indexes[CODEX_PET_MAX_ANIM_FRAMES] = {0};
static bool codex_pet_uses_atlas = false;
#if BOARD_CODEX_STRIP
static bool codex_pet_cache_ready = false;
static const char *CODEX_PET_CACHE_PATH = "/codex_pet_anim.bin";
static const char *CODEX_PET_ATLAS_CACHE_PATH = "/codex_pet_atlas.bin";
static const uint32_t CODEX_PET_CACHE_MAGIC = 0x54455043; // CPET, little-endian
static const uint32_t CODEX_PET_ATLAS_CACHE_MAGIC = 0x54455041; // APET, little-endian
static const uint16_t CODEX_PET_CACHE_VERSION = 1;

struct CodexPetCacheHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t w;
    uint16_t h;
    uint8_t frames;
    uint8_t loop_start;
    uint32_t size;
    uint16_t durations[CODEX_PET_MAX_ANIM_FRAMES];
};

struct CodexPetAtlasCacheHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t w;
    uint16_t h;
    uint8_t frames;
    uint8_t reserved;
    uint32_t size;
};
#endif
static lv_obj_t *codex_project_label = nullptr;
static lv_obj_t *codex_output_status_label = nullptr;
static lv_obj_t *codex_output_bubble = nullptr;
static lv_obj_t *codex_output_body_label = nullptr;
#define CODEX_OUTPUT_MAX_LINES 8
#define CODEX_OUTPUT_VISIBLE_LINES 4
#define CODEX_SESSION_MAX_ITEMS 8
static lv_obj_t *codex_output_line_labels[CODEX_OUTPUT_MAX_LINES] = {};
static lv_obj_t *codex_output_footer_label = nullptr;
static lv_obj_t *codex_context_label = nullptr;
static lv_obj_t *codex_context_value_label = nullptr;
static lv_obj_t *codex_context_bar = nullptr;
static lv_obj_t *codex_output_image = nullptr;
static uint8_t *codex_output_image_pixels = nullptr;
static lv_image_dsc_t codex_output_image_dsc = {};
static char codex_output_target_lines[CODEX_OUTPUT_MAX_LINES][128] = {{0}};
static char codex_output_visible_lines[CODEX_OUTPUT_MAX_LINES][128] = {{0}};
static bool codex_output_typing_active = false;
static uint8_t codex_output_typing_line = 0;
static size_t codex_output_typing_byte = 0;
static uint32_t codex_output_typing_last_ms = 0;
static uint8_t codex_output_scroll_offset = 0;
static uint8_t codex_output_line_count = 0;
static bool codex_session_list_open = false;
static uint8_t codex_session_count = 0;
static uint8_t codex_session_selected = 0;
static uint8_t codex_session_scroll_offset = 0;
static char codex_session_titles[CODEX_SESSION_MAX_ITEMS][72] = {{0}};
static int32_t codex_pet_base_x = 0;
static int32_t codex_pet_base_y = 0;
static char codex_pet_motion_state[16] = "idle";
static uint32_t codex_pet_motion_last_ms = 0;
static uint32_t codex_touch_feedback_until_ms = 0;
static bool codex_quota_refresh_pending = false;
static uint32_t codex_quota_feedback_until_ms = 0;
static bool codex_touch_user_stopped = false;

static void render_codex_session_list();

static void panel_style(lv_obj_t *obj, uint32_t color) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_panel(lv_obj_t *parent, int32_t x, int32_t y,
                            int32_t w, int32_t h, uint32_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    panel_style(obj, color);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            int32_t x, int32_t y, int32_t w,
                            const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, w);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_set_style_text_line_space(label, 2, 0);
    return label;
}

static lv_obj_t *make_bar(lv_obj_t *parent, int32_t x, int32_t y, int32_t w,
                          int32_t value, uint32_t color) {
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, w, 7);
    lv_obj_set_pos(bar, x, y);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x273031), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(bar, 0, 0);
    return bar;
}

static lv_obj_t *make_usage_card(lv_obj_t *parent, int32_t x, int32_t y,
                                 uint32_t accent, const char *title,
                                 lv_obj_t **arc_out, lv_obj_t **pct_out,
                                 lv_obj_t **reset_out) {
    lv_obj_t *card = make_panel(parent, x, y, 136, 72, 0x0a1115);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(accent), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_60, 0);

    lv_obj_t *arc = lv_arc_create(card);
    lv_obj_set_size(arc, 48, 48);
    lv_obj_set_pos(arc, 6, 12);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_arc_set_bg_angles(arc, 125, 55);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x243141), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);

    lv_obj_t *pct = make_label(card, "--%", 6, 29, 48, &font_styrene_14, 0xffffff);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, 0);

    const lv_font_t *title_font = strlen(title) > 2 ? &font_styrene_12 : &font_styrene_14;
    lv_obj_t *title_label = make_label(card, title, 64, 17, 64, title_font, 0xffffff);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_CLIP);
    lv_obj_t *reset = make_label(card, "--", 65, 43, 62, &font_styrene_12, accent);
    lv_label_set_long_mode(reset, LV_LABEL_LONG_CLIP);

    if (arc_out) *arc_out = arc;
    if (pct_out) *pct_out = pct;
    if (reset_out) *reset_out = reset;
    return card;
}

static void set_obj_color(lv_obj_t *obj, uint32_t color) {
    if (obj) lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
}

static void apply_pet_theme(const char *pet_id, const char *name) {
    const bool is_miku = pet_id && strstr(pet_id, "miku");
    const bool is_mikoto = pet_id && strstr(pet_id, "mikoto");
    const uint32_t main_color = is_mikoto ? 0xe7a73b : 0x1fbfae;
    const uint32_t dark_color = is_mikoto ? 0x8a6129 : 0x08746d;
    const uint32_t face_color = is_mikoto ? 0xffefd0 : 0xdcfff7;
    const uint32_t pulse_color = is_mikoto ? 0xffd58a : 0x92fff0;

    if (codex_pet_name_label) {
        lv_label_set_text(codex_pet_name_label, "");
        lv_obj_set_style_text_color(codex_pet_name_label,
                                    lv_color_hex(is_mikoto ? 0xf5c579 : 0x9ff4e8), 0);
    }
    set_obj_color(codex_pet_head, main_color);
    set_obj_color(codex_pet_body, dark_color);
    set_obj_color(codex_pet_tail_l, dark_color);
    set_obj_color(codex_pet_tail_r, dark_color);
    set_obj_color(codex_pet_face, face_color);
    set_obj_color(codex_pulse, pulse_color);
    if (codex_pet_sprite && codex_pet_sprite_pixels) {
        lv_obj_clear_flag(codex_pet_sprite, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_pet_sprite_frame(uint8_t frame_index) {
    if (!codex_pet_sprite || !codex_pet_sprite_pixels || codex_pet_frame_size == 0) {
        return;
    }
    if (frame_index >= codex_pet_anim_frames) frame_index = 0;
    codex_pet_anim_index = frame_index;
    uint8_t source_index = frame_index;
    if (codex_pet_uses_atlas) {
        source_index = codex_pet_anim_frame_indexes[frame_index];
        if (source_index >= codex_pet_atlas_frames) source_index = 0;
    }
    codex_pet_sprite_dsc.data = codex_pet_sprite_pixels + (uint32_t)source_index * codex_pet_frame_size;
    lv_image_set_src(codex_pet_sprite, &codex_pet_sprite_dsc);
    lv_obj_invalidate(codex_pet_sprite);
}

static uint8_t utf8_char_len(char lead) {
    uint8_t ch = (uint8_t)lead;
    if ((ch & 0x80) == 0) return 1;
    if ((ch & 0xe0) == 0xc0) return 2;
    if ((ch & 0xf0) == 0xe0) return 3;
    if ((ch & 0xf8) == 0xf0) return 4;
    return 1;
}

static void position_pet_sprite(uint16_t w, uint16_t h) {
    if (!codex_pet_sprite) return;
    int32_t x = (168 - (int32_t)w) / 2;
    int32_t y = (148 - (int32_t)h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    codex_pet_base_x = x;
    codex_pet_base_y = y;
    lv_obj_set_pos(codex_pet_sprite, codex_pet_base_x, codex_pet_base_y);
    lv_obj_clear_flag(codex_pet_sprite, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(codex_pet_slot);
}

static bool update_pet_animation_from_rgb565(uint16_t w, uint16_t h, uint8_t frames,
                                             uint8_t *pixels, uint32_t size,
                                             const uint16_t *durations,
                                             uint8_t loop_start) {
    const uint32_t frame_size = (uint32_t)w * h * 2;
    if (!codex_pet_slot || !pixels || w == 0 || h == 0 || frames == 0 ||
        frames > CODEX_PET_MAX_ANIM_FRAMES || frame_size == 0 ||
        size != frame_size * frames) {
        return false;
    }

    uint8_t *old_pixels = nullptr;
    uint8_t *incoming_pixels = pixels;
    uint32_t old_size = (uint32_t)codex_pet_sprite_w * codex_pet_sprite_h * 2 * codex_pet_anim_frames;
    if (codex_pet_sprite_pixels && old_size == size) {
        memcpy(codex_pet_sprite_pixels, pixels, size);
        pixels = codex_pet_sprite_pixels;
        if (incoming_pixels != codex_pet_sprite_pixels) {
            heap_caps_free(incoming_pixels);
        }
    } else {
        old_pixels = codex_pet_sprite_pixels;
        codex_pet_sprite_pixels = pixels;
    }
    codex_pet_sprite_w = w;
    codex_pet_sprite_h = h;
    codex_pet_atlas_frames = frames;
    codex_pet_anim_frames = frames;
    codex_pet_anim_index = 0;
    codex_pet_anim_loop_start = loop_start < frames ? loop_start : 0;
    codex_pet_frame_size = frame_size;
    codex_pet_anim_last_ms = millis();
    codex_pet_uses_atlas = false;
    for (uint8_t i = 0; i < CODEX_PET_MAX_ANIM_FRAMES; i++) {
        codex_pet_frame_durations[i] = i < frames && durations && durations[i] > 0
            ? durations[i]
            : 180;
        codex_pet_anim_frame_indexes[i] = i;
    }

    memset(&codex_pet_sprite_dsc, 0, sizeof(codex_pet_sprite_dsc));
    codex_pet_sprite_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    codex_pet_sprite_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    codex_pet_sprite_dsc.header.w = w;
    codex_pet_sprite_dsc.header.h = h;
    codex_pet_sprite_dsc.header.stride = w * 2;
    codex_pet_sprite_dsc.data_size = frame_size;
    codex_pet_sprite_dsc.data = codex_pet_sprite_pixels;

    if (!codex_pet_sprite) {
        codex_pet_sprite = lv_image_create(codex_pet_slot);
    }
    lv_image_set_src(codex_pet_sprite, &codex_pet_sprite_dsc);
    position_pet_sprite(w, h);
    if (old_pixels) {
        heap_caps_free(old_pixels);
    }
    return true;
}

static bool update_pet_atlas_from_rgb565(uint16_t w, uint16_t h, uint8_t frames,
                                         uint8_t *pixels, uint32_t size) {
    const uint32_t frame_size = (uint32_t)w * h * 2;
    if (!codex_pet_slot || !pixels || w == 0 || h == 0 || frames == 0 ||
        frames > CODEX_PET_MAX_ATLAS_FRAMES || frame_size == 0 ||
        size != frame_size * frames) {
        return false;
    }

    uint8_t *old_pixels = codex_pet_sprite_pixels;
    codex_pet_sprite_pixels = pixels;
    codex_pet_sprite_w = w;
    codex_pet_sprite_h = h;
    codex_pet_atlas_frames = frames;
    codex_pet_anim_frames = 1;
    codex_pet_anim_index = 0;
    codex_pet_anim_loop_start = 0;
    codex_pet_frame_size = frame_size;
    codex_pet_anim_last_ms = millis();
    codex_pet_uses_atlas = true;
    codex_pet_frame_durations[0] = 180;
    codex_pet_anim_frame_indexes[0] = 0;

    memset(&codex_pet_sprite_dsc, 0, sizeof(codex_pet_sprite_dsc));
    codex_pet_sprite_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    codex_pet_sprite_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    codex_pet_sprite_dsc.header.w = w;
    codex_pet_sprite_dsc.header.h = h;
    codex_pet_sprite_dsc.header.stride = w * 2;
    codex_pet_sprite_dsc.data_size = frame_size;
    codex_pet_sprite_dsc.data = codex_pet_sprite_pixels;

    if (!codex_pet_sprite) {
        codex_pet_sprite = lv_image_create(codex_pet_slot);
    }
    lv_image_set_src(codex_pet_sprite, &codex_pet_sprite_dsc);
    position_pet_sprite(w, h);
    if (old_pixels) {
        heap_caps_free(old_pixels);
    }
    return true;
}

static bool select_pet_animation_from_atlas(uint8_t frames, uint8_t loop_start,
                                            const uint8_t *indexes,
                                            const uint16_t *durations) {
    if (!codex_pet_uses_atlas || !codex_pet_sprite_pixels || frames == 0 ||
        frames > CODEX_PET_MAX_ANIM_FRAMES) {
        return false;
    }
    for (uint8_t i = 0; i < frames; i++) {
        if (!indexes || indexes[i] >= codex_pet_atlas_frames) {
            return false;
        }
    }
    codex_pet_anim_frames = frames;
    codex_pet_anim_index = 0;
    codex_pet_anim_loop_start = loop_start < frames ? loop_start : 0;
    codex_pet_anim_last_ms = millis();
    for (uint8_t i = 0; i < CODEX_PET_MAX_ANIM_FRAMES; i++) {
        codex_pet_frame_durations[i] = i < frames && durations && durations[i] > 0
            ? durations[i]
            : 180;
        codex_pet_anim_frame_indexes[i] = i < frames ? indexes[i] : 0;
    }
    apply_pet_sprite_frame(0);
    return true;
}

static bool select_codex_touch_running_animation() {
    if (!codex_pet_uses_atlas || codex_pet_atlas_frames < 14) {
        return false;
    }
    static const uint8_t indexes[] = {6, 7, 8, 9, 10, 11, 12, 13};
    static const uint16_t durations[] = {240, 240, 240, 240, 240, 240, 240, 440};
    return select_pet_animation_from_atlas(8, 0, indexes, durations);
}

static bool select_codex_idle_animation() {
    if (!codex_pet_uses_atlas || codex_pet_atlas_frames < 6) {
        return false;
    }
    static const uint8_t indexes[] = {0, 1, 2, 3, 4, 5};
    static const uint16_t durations[] = {1680, 660, 660, 840, 840, 1920};
    return select_pet_animation_from_atlas(6, 0, indexes, durations);
}

static bool update_pet_sprite_from_rgb565(uint16_t w, uint16_t h, uint8_t *pixels,
                                          uint32_t size) {
    uint16_t duration = 180;
    return update_pet_animation_from_rgb565(w, h, 1, pixels, size, &duration, 0);
}

#if BOARD_CODEX_STRIP
static bool init_pet_cache_fs() {
    if (codex_pet_cache_ready) {
        return true;
    }
    codex_pet_cache_ready = LittleFS.begin(true);
    Serial.printf("Pet cache FS %s\n", codex_pet_cache_ready ? "ready" : "failed");
    return codex_pet_cache_ready;
}

static bool save_pet_animation_cache(uint16_t w, uint16_t h, uint8_t frames,
                                     uint32_t size, uint8_t loop_start,
                                     const uint16_t *durations,
                                     const uint8_t *pixels) {
    const uint32_t frame_size = (uint32_t)w * h * 2;
    if (!pixels || w == 0 || h == 0 || frames == 0 ||
        frames > CODEX_PET_MAX_ANIM_FRAMES || frame_size == 0 ||
        size != frame_size * frames) {
        return false;
    }
    if (!init_pet_cache_fs()) {
        return false;
    }

    CodexPetCacheHeader header = {};
    header.magic = CODEX_PET_CACHE_MAGIC;
    header.version = CODEX_PET_CACHE_VERSION;
    header.w = w;
    header.h = h;
    header.frames = frames;
    header.loop_start = loop_start < frames ? loop_start : 0;
    header.size = size;
    for (uint8_t i = 0; i < CODEX_PET_MAX_ANIM_FRAMES; i++) {
        header.durations[i] = i < frames && durations && durations[i] > 0
            ? durations[i]
            : 180;
    }

    File file = LittleFS.open(CODEX_PET_CACHE_PATH, "w");
    if (!file) {
        return false;
    }
    bool ok = file.write((const uint8_t *)&header, sizeof(header)) == sizeof(header);
    ok = ok && file.write(pixels, size) == size;
    file.close();
    if (!ok) {
        LittleFS.remove(CODEX_PET_CACHE_PATH);
    }
    return ok;
}

static bool save_pet_atlas_cache(uint16_t w, uint16_t h, uint8_t frames,
                                 uint32_t size, const uint8_t *pixels) {
    const uint32_t frame_size = (uint32_t)w * h * 2;
    if (!pixels || w == 0 || h == 0 || frames == 0 ||
        frames > CODEX_PET_MAX_ATLAS_FRAMES || frame_size == 0 ||
        size != frame_size * frames) {
        return false;
    }
    if (!init_pet_cache_fs()) {
        return false;
    }

    CodexPetAtlasCacheHeader header = {};
    header.magic = CODEX_PET_ATLAS_CACHE_MAGIC;
    header.version = CODEX_PET_CACHE_VERSION;
    header.w = w;
    header.h = h;
    header.frames = frames;
    header.size = size;

    File file = LittleFS.open(CODEX_PET_ATLAS_CACHE_PATH, "w");
    if (!file) {
        return false;
    }
    bool ok = file.write((const uint8_t *)&header, sizeof(header)) == sizeof(header);
    ok = ok && file.write(pixels, size) == size;
    file.close();
    if (!ok) {
        LittleFS.remove(CODEX_PET_ATLAS_CACHE_PATH);
    }
    return ok;
}

static bool load_pet_atlas_cache() {
    if (!init_pet_cache_fs()) {
        return false;
    }
    File file = LittleFS.open(CODEX_PET_ATLAS_CACHE_PATH, "r");
    if (!file) {
        return false;
    }

    CodexPetAtlasCacheHeader header = {};
    if (file.read((uint8_t *)&header, sizeof(header)) != sizeof(header)) {
        file.close();
        Serial.println("Pet atlas cache header read failed");
        return false;
    }
    const uint32_t frame_size = (uint32_t)header.w * header.h * 2;
    if (header.magic != CODEX_PET_ATLAS_CACHE_MAGIC ||
        header.version != CODEX_PET_CACHE_VERSION ||
        header.w == 0 || header.h == 0 || header.frames == 0 ||
        header.frames > CODEX_PET_MAX_ATLAS_FRAMES ||
        frame_size == 0 || header.size != frame_size * header.frames ||
        file.size() != (size_t)sizeof(header) + header.size) {
        file.close();
        LittleFS.remove(CODEX_PET_ATLAS_CACHE_PATH);
        Serial.println("Pet atlas cache invalid");
        return false;
    }

    uint8_t *pixels = (uint8_t *)heap_caps_malloc(header.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        pixels = (uint8_t *)heap_caps_malloc(header.size, MALLOC_CAP_8BIT);
    }
    if (!pixels) {
        file.close();
        Serial.println("Pet atlas cache alloc failed");
        return false;
    }
    if (file.read(pixels, header.size) != (int)header.size) {
        file.close();
        heap_caps_free(pixels);
        Serial.println("Pet atlas cache pixel read failed");
        return false;
    }
    file.close();

    if (!update_pet_atlas_from_rgb565(header.w, header.h, header.frames,
                                      pixels, header.size)) {
        heap_caps_free(pixels);
        Serial.println("Pet atlas cache apply failed");
        return false;
    }
    Serial.printf("Pet atlas cache loaded %u %u %u %lu\n", header.w, header.h,
                  header.frames, (unsigned long)header.size);
    return true;
}

static bool load_pet_animation_cache() {
    if (load_pet_atlas_cache()) {
        return true;
    }
    if (!init_pet_cache_fs()) {
        return false;
    }
    File file = LittleFS.open(CODEX_PET_CACHE_PATH, "r");
    if (!file) {
        Serial.println("Pet cache empty");
        return false;
    }

    CodexPetCacheHeader header = {};
    if (file.read((uint8_t *)&header, sizeof(header)) != sizeof(header)) {
        file.close();
        Serial.println("Pet cache header read failed");
        return false;
    }
    const uint32_t frame_size = (uint32_t)header.w * header.h * 2;
    if (header.magic != CODEX_PET_CACHE_MAGIC ||
        header.version != CODEX_PET_CACHE_VERSION ||
        header.w == 0 || header.h == 0 || header.frames == 0 ||
        header.frames > CODEX_PET_MAX_ANIM_FRAMES ||
        frame_size == 0 || header.size != frame_size * header.frames ||
        file.size() != (size_t)sizeof(header) + header.size) {
        file.close();
        LittleFS.remove(CODEX_PET_CACHE_PATH);
        Serial.println("Pet cache invalid");
        return false;
    }

    uint8_t *pixels = (uint8_t *)heap_caps_malloc(header.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        pixels = (uint8_t *)heap_caps_malloc(header.size, MALLOC_CAP_8BIT);
    }
    if (!pixels) {
        file.close();
        Serial.println("Pet cache alloc failed");
        return false;
    }
    if (file.read(pixels, header.size) != (int)header.size) {
        file.close();
        heap_caps_free(pixels);
        Serial.println("Pet cache pixel read failed");
        return false;
    }
    file.close();

    if (!update_pet_animation_from_rgb565(header.w, header.h, header.frames,
                                          pixels, header.size, header.durations,
                                          header.loop_start)) {
        heap_caps_free(pixels);
        Serial.println("Pet cache apply failed");
        return false;
    }
    Serial.printf("Pet cache loaded %u %u %u %lu\n", header.w, header.h,
                  header.frames, (unsigned long)header.size);
    return true;
}
#endif

static bool update_codex_output_image_from_rgb565(uint16_t w, uint16_t h,
                                                  uint8_t *pixels, uint32_t size) {
    const uint32_t image_size = (uint32_t)w * h * 2;
    if (!pixels || w == 0 || h == 0 || image_size == 0 || size != image_size ||
        w > (LCD_WIDTH - 320) || h > LCD_HEIGHT) {
        return false;
    }

    uint32_t old_image_size = codex_output_image_dsc.data_size;
    if (codex_output_image && codex_output_image_pixels && old_image_size == size) {
        memcpy(codex_output_image_pixels, pixels, size);
        heap_caps_free(pixels);
        lv_obj_move_foreground(codex_output_image);
        lv_obj_clear_flag(codex_output_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(codex_output_image);
        return true;
    } else {
        codex_output_image_pixels = pixels;
    }

    memset(&codex_output_image_dsc, 0, sizeof(codex_output_image_dsc));
    codex_output_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    codex_output_image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    codex_output_image_dsc.header.w = w;
    codex_output_image_dsc.header.h = h;
    codex_output_image_dsc.header.stride = w * 2;
    codex_output_image_dsc.data_size = image_size;
    codex_output_image_dsc.data = codex_output_image_pixels;

    if (!codex_output_image) {
        codex_output_image = lv_image_create(lv_screen_active());
        lv_obj_set_pos(codex_output_image, 320, 0);
    }
    lv_image_set_src(codex_output_image, &codex_output_image_dsc);
    lv_obj_move_foreground(codex_output_image);
    lv_obj_clear_flag(codex_output_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(codex_output_image);
    return true;
}

static void update_codex_output_screen(const char *project, const char *status,
                                       const char *body, const char *footer) {
    codex_output_typing_active = false;
    if (codex_output_image) {
        lv_obj_add_flag(codex_output_image, LV_OBJ_FLAG_HIDDEN);
    }
    for (uint8_t i = 0; i < CODEX_OUTPUT_MAX_LINES; i++) {
        if (codex_output_line_labels[i]) {
            lv_obj_add_flag(codex_output_line_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (codex_output_body_label) {
        lv_obj_clear_flag(codex_output_body_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (codex_project_label) {
        lv_label_set_text(codex_project_label,
                          project && project[0] ? project : "Current Project");
    }
    if (codex_output_status_label) {
        lv_label_set_text(codex_output_status_label,
                          status && status[0] ? status : "LIVE");
    }
    if (codex_session_list_open) {
        render_codex_session_list();
        return;
    }
    if (codex_output_body_label) {
        const char *display_body = body && body[0] ? body : "Waiting for Codex output...";
        lv_label_set_text(codex_output_body_label, display_body);
        size_t len = strlen(display_body);
        if (len <= 4) {
            lv_obj_set_style_text_font(codex_output_body_label, &font_styrene_24, 0);
            lv_obj_set_style_text_align(codex_output_body_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(codex_output_body_label, 10, 28);
            lv_obj_set_size(codex_output_body_label, 260, 70);
            lv_label_set_long_mode(codex_output_body_label, LV_LABEL_LONG_WRAP);
        } else {
            lv_obj_set_style_text_font(codex_output_body_label, &font_cjk_18, 0);
            lv_obj_set_style_text_align(codex_output_body_label, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_pos(codex_output_body_label, 12, 10);
            lv_obj_set_size(codex_output_body_label, 268, 98);
            lv_label_set_long_mode(codex_output_body_label, LV_LABEL_LONG_WRAP);
        }
    }
    if (codex_output_footer_label) {
        lv_label_set_text(codex_output_footer_label,
                          footer && footer[0] ? footer : "source: current thread");
    }
}

static void render_codex_output_window() {
    if (codex_output_line_count <= CODEX_OUTPUT_VISIBLE_LINES) {
        codex_output_scroll_offset = 0;
    } else if (codex_output_scroll_offset > codex_output_line_count - CODEX_OUTPUT_VISIBLE_LINES) {
        codex_output_scroll_offset = codex_output_line_count - CODEX_OUTPUT_VISIBLE_LINES;
    }
    for (uint8_t i = 0; i < CODEX_OUTPUT_VISIBLE_LINES; i++) {
        if (!codex_output_line_labels[i]) continue;
        uint8_t source = codex_output_scroll_offset + i;
        const char *text = source < CODEX_OUTPUT_MAX_LINES ? codex_output_target_lines[source] : "";
        lv_label_set_text(codex_output_line_labels[i], text);
        lv_obj_clear_flag(codex_output_line_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (uint8_t i = CODEX_OUTPUT_VISIBLE_LINES; i < CODEX_OUTPUT_MAX_LINES; i++) {
        if (codex_output_line_labels[i]) {
            lv_obj_add_flag(codex_output_line_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void scroll_codex_output_window(int8_t delta) {
    if (codex_output_line_count <= CODEX_OUTPUT_VISIBLE_LINES) return;
    int16_t next = (int16_t)codex_output_scroll_offset + delta;
    if (next < 0) next = 0;
    uint8_t max_offset = codex_output_line_count - CODEX_OUTPUT_VISIBLE_LINES;
    if (next > max_offset) next = max_offset;
    if ((uint8_t)next == codex_output_scroll_offset) return;
    codex_output_scroll_offset = (uint8_t)next;
    render_codex_output_window();
}

static void render_codex_session_list() {
    if (codex_output_image) {
        lv_obj_add_flag(codex_output_image, LV_OBJ_FLAG_HIDDEN);
    }
    if (codex_output_body_label) {
        lv_obj_add_flag(codex_output_body_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (codex_output_status_label) {
        lv_label_set_text(codex_output_status_label, "SESS");
    }
    if (codex_session_count <= CODEX_OUTPUT_VISIBLE_LINES) {
        codex_session_scroll_offset = 0;
    } else if (codex_session_selected < codex_session_scroll_offset) {
        codex_session_scroll_offset = codex_session_selected;
    } else if (codex_session_selected >= codex_session_scroll_offset + CODEX_OUTPUT_VISIBLE_LINES) {
        codex_session_scroll_offset = codex_session_selected - CODEX_OUTPUT_VISIBLE_LINES + 1;
    }
    for (uint8_t i = 0; i < CODEX_OUTPUT_VISIBLE_LINES; i++) {
        if (!codex_output_line_labels[i]) continue;
        char line[96];
        uint8_t source = codex_session_scroll_offset + i;
        if (source < codex_session_count) {
            snprintf(line, sizeof(line), "%c %u %s",
                     source == codex_session_selected ? '>' : ' ',
                     (unsigned)(source + 1),
                     codex_session_titles[source][0] ? codex_session_titles[source] : "Untitled");
        } else if (i == 0) {
            strlcpy(line, "Loading sessions...", sizeof(line));
        } else {
            line[0] = '\0';
        }
        lv_label_set_text(codex_output_line_labels[i], line);
        lv_obj_clear_flag(codex_output_line_labels[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(codex_output_line_labels[i],
                                    lv_color_hex(source == codex_session_selected ? 0x111820 : 0x52616a), 0);
    }
}

static void open_codex_session_list() {
    codex_session_list_open = true;
    if (codex_session_selected >= codex_session_count) {
        codex_session_selected = 0;
    }
    codex_session_scroll_offset = 0;
    render_codex_session_list();
    ble_request_session_list();
}

static void close_codex_session_list() {
    codex_session_list_open = false;
    if (codex_output_status_label) {
        lv_label_set_text(codex_output_status_label, "SYNC");
    }
    for (uint8_t i = 0; i < CODEX_OUTPUT_VISIBLE_LINES; i++) {
        if (codex_output_line_labels[i]) {
            lv_obj_set_style_text_color(codex_output_line_labels[i], lv_color_hex(0x111820), 0);
        }
    }
    render_codex_output_window();
}

static void move_codex_session_selection(int8_t delta) {
    if (codex_session_count == 0) return;
    int16_t next = (int16_t)codex_session_selected + delta;
    if (next < 0) next = 0;
    if (next >= codex_session_count) next = codex_session_count - 1;
    if ((uint8_t)next == codex_session_selected) return;
    codex_session_selected = (uint8_t)next;
    render_codex_session_list();
}

static void select_codex_session_from_touch() {
    if (!codex_session_list_open || codex_session_count == 0) return;
    ble_request_session_select(codex_session_selected);
    close_codex_session_list();
}

static void update_codex_session_list(const JsonDocument &doc) {
    uint8_t count = doc["n"] | 0;
    if (count > CODEX_SESSION_MAX_ITEMS) count = CODEX_SESSION_MAX_ITEMS;
    uint8_t selected = doc["i"] | 0;
    codex_session_count = count;
    codex_session_selected = selected < count ? selected : 0;
    codex_session_scroll_offset = 0;
    for (uint8_t i = 0; i < CODEX_SESSION_MAX_ITEMS; i++) {
        char key[4];
        snprintf(key, sizeof(key), "t%u", (unsigned)(i + 1));
        const char *title = doc[key] | "";
        strlcpy(codex_session_titles[i], title, sizeof(codex_session_titles[i]));
    }
    if (codex_session_list_open) {
        render_codex_session_list();
    }
}

static void update_codex_output_lines(const char *project, const char *status,
                                      const char *line1, const char *line2,
                                      const char *line3, const char *line4,
                                      const char *line5, const char *line6,
                                      const char *line7, const char *line8) {
    if (codex_output_image) {
        lv_obj_add_flag(codex_output_image, LV_OBJ_FLAG_HIDDEN);
    }
    if (codex_project_label) {
        lv_label_set_text(codex_project_label,
                          project && project[0] ? project : "Current Project");
    }
    if (codex_output_status_label) {
        lv_label_set_text(codex_output_status_label,
                          status && status[0] ? status : "LIVE");
    }
    const char *lines[CODEX_OUTPUT_MAX_LINES] = {
        line1, line2, line3, line4, line5, line6, line7, line8
    };
    bool has_any_line = false;
    for (uint8_t i = 0; i < CODEX_OUTPUT_MAX_LINES; i++) {
        if (lines[i] && lines[i][0]) {
            has_any_line = true;
            break;
        }
    }
    if (codex_output_body_label) {
        if (has_any_line) {
            lv_obj_add_flag(codex_output_body_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(codex_output_body_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(codex_output_body_label, "Waiting for Codex output...");
        }
    }
    codex_output_line_count = 0;
    for (uint8_t i = 0; i < CODEX_OUTPUT_MAX_LINES; i++) {
        const char *src = lines[i] ? lines[i] : "";
        strlcpy(codex_output_target_lines[i], src, sizeof(codex_output_target_lines[i]));
        if (src[0]) codex_output_line_count = i + 1;
    }
    codex_output_typing_active = false;
    codex_output_scroll_offset = 0;
    if (codex_session_list_open) {
        render_codex_session_list();
    } else {
        render_codex_output_window();
    }
}

static void format_token_count(char *out, size_t len, uint32_t tokens) {
    if (tokens >= 1000000) {
        snprintf(out, len, "%lu.%luM",
                 (unsigned long)(tokens / 1000000),
                 (unsigned long)((tokens % 1000000) / 100000));
    } else if (tokens >= 1000) {
        snprintf(out, len, "%luk", (unsigned long)((tokens + 500) / 1000));
    } else {
        snprintf(out, len, "%lu", (unsigned long)tokens);
    }
}

static void update_codex_context_screen(uint32_t input_tokens, uint32_t window_tokens) {
    if (!codex_context_bar || window_tokens == 0) return;
    uint32_t pct = (input_tokens * 100UL + window_tokens / 2) / window_tokens;
    if (pct > 100) pct = 100;
    lv_bar_set_value(codex_context_bar, (int32_t)pct, LV_ANIM_OFF);

    uint32_t color = pct >= 85 ? 0xff6f5f : (pct >= 70 ? 0xffbd4a : 0x35d49a);
    lv_obj_set_style_bg_color(codex_context_bar, lv_color_hex(color), LV_PART_INDICATOR);
    if (codex_context_label) {
        lv_label_set_text_fmt(codex_context_label, "CTX %lu%%", (unsigned long)pct);
        lv_obj_set_style_text_color(codex_context_label, lv_color_hex(color), 0);
    }
    if (codex_context_value_label) {
        char input_buf[16];
        char window_buf[16];
        format_token_count(input_buf, sizeof(input_buf), input_tokens);
        format_token_count(window_buf, sizeof(window_buf), window_tokens);
        lv_label_set_text_fmt(codex_context_value_label, "%s / %s", input_buf, window_buf);
    }
}

static void update_codex_output_typing() {
    codex_output_typing_active = false;
}

static void format_reset(char *out, size_t len, int mins) {
    if (mins < 0) {
        strlcpy(out, "--", len);
    } else if (mins < 60) {
        snprintf(out, len, "%dm", mins);
    } else if (mins < 24 * 60) {
        snprintf(out, len, "%dh%02d", mins / 60, mins % 60);
    } else {
        snprintf(out, len, "%dd%02dh", mins / (24 * 60), (mins / 60) % 24);
    }
}

static uint32_t quota_color_for_pct(int pct, uint32_t normal) {
    if (pct >= 95) return 0xff5f57;
    if (pct >= 80) return 0xffbd4a;
    if (pct >= 65) return 0x7bd88f;
    return normal;
}

static void set_codex_quota_card_feedback(uint8_t opa) {
    if (codex_session_card) {
        lv_obj_set_style_border_opa(codex_session_card, opa, 0);
    }
    if (codex_week_card) {
        lv_obj_set_style_border_opa(codex_week_card, opa, 0);
    }
}

static void update_codex_usage_screen(const UsageData *data) {
    if (!codex_session_pct_label || !codex_week_pct_label || !data) return;
    bool was_refresh_pending = codex_quota_refresh_pending;
    if (codex_quota_refresh_pending) {
        codex_quota_refresh_pending = false;
        codex_quota_feedback_until_ms = millis() + 900;
    }

    int session = (int)(data->session_pct + 0.5f);
    int weekly = (int)(data->weekly_pct + 0.5f);
    if (session < 0) session = 0;
    if (session > 100) session = 100;
    if (weekly < 0) weekly = 0;
    if (weekly > 100) weekly = 100;

    lv_label_set_text_fmt(codex_session_pct_label, "%d%%", session);
    lv_label_set_text_fmt(codex_week_pct_label, "%d%%", weekly);
    if (codex_session_arc) lv_arc_set_value(codex_session_arc, session);
    if (codex_week_arc) lv_arc_set_value(codex_week_arc, weekly);
    uint32_t session_color = quota_color_for_pct(session, 0x28dff0);
    uint32_t weekly_color = quota_color_for_pct(weekly, 0xffbd4a);
    if (codex_session_arc) {
        lv_obj_set_style_arc_color(codex_session_arc, lv_color_hex(session_color), LV_PART_INDICATOR);
    }
    if (codex_week_arc) {
        lv_obj_set_style_arc_color(codex_week_arc, lv_color_hex(weekly_color), LV_PART_INDICATOR);
    }
    lv_obj_set_style_text_color(codex_session_pct_label, lv_color_hex(session_color), 0);
    lv_obj_set_style_text_color(codex_week_pct_label, lv_color_hex(weekly_color), 0);

    char reset_buf[24];
    if (data->session_reset_label[0]) {
        lv_label_set_text(codex_session_reset_label, data->session_reset_label);
    } else {
        format_reset(reset_buf, sizeof(reset_buf), data->session_reset_mins);
        lv_label_set_text(codex_session_reset_label, reset_buf);
    }
    if (data->weekly_reset_label[0]) {
        lv_label_set_text(codex_week_reset_label, data->weekly_reset_label);
    } else {
        format_reset(reset_buf, sizeof(reset_buf), data->weekly_reset_mins);
        lv_label_set_text(codex_week_reset_label, reset_buf);
    }
    if (was_refresh_pending) {
        lv_label_set_text(codex_status_label, "updated");
    } else if (strcmp(data->status, "limited") == 0) {
        lv_label_set_text(codex_status_label, "LIMITED");
    } else if (session >= 90) {
        lv_label_set_text(codex_status_label, "5h nearly full");
    } else if (weekly >= 90) {
        lv_label_set_text(codex_status_label, "week nearly full");
    } else {
        lv_label_set_text(codex_status_label, "");
    }

    uint32_t status_color = was_refresh_pending ? 0x35d49a :
                            (strcmp(data->status, "limited") == 0 ? 0xff6f5f : 0x75e2a3);
    lv_obj_set_style_text_color(codex_status_label, lv_color_hex(status_color), 0);
    set_codex_quota_card_feedback(was_refresh_pending ? LV_OPA_COVER : LV_OPA_60);
}

static void build_codex_pet_screen() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    panel_style(scr, 0x070908);

    lv_obj_t *usage_panel = make_panel(scr, 0, 0, 152, LCD_HEIGHT, 0x070908);
    lv_obj_t *pet_panel = make_panel(scr, 152, 0, 168, LCD_HEIGHT, 0x070908);
    lv_obj_t *reply_panel = make_panel(scr, 320, 0, LCD_WIDTH - 320, LCD_HEIGHT, 0x070908);

    make_panel(scr, 151, 0, 1, LCD_HEIGHT, 0x273232);
    make_panel(scr, 319, 0, 1, LCD_HEIGHT, 0x273232);

    codex_session_card = make_usage_card(usage_panel, 8, 10, 0x28dff0, "5h",
                                         &codex_session_arc, &codex_session_pct_label,
                                         &codex_session_reset_label);
    codex_week_card = make_usage_card(usage_panel, 8, 88, 0xffbd4a, "WEEK",
                                      &codex_week_arc, &codex_week_pct_label,
                                      &codex_week_reset_label);
    codex_status_label = make_label(usage_panel, "", 10, 156, 132, &font_styrene_12, 0x75e2a3);

    codex_pet_name_label = make_label(pet_panel, "", 48, 8, 80, &font_styrene_12, 0x9fdac8);
    lv_obj_set_style_text_align(codex_pet_name_label, LV_TEXT_ALIGN_CENTER, 0);

    codex_pet_slot = make_panel(pet_panel, 0, 20, 168, 148, 0x070908);

    codex_pet_sprite = lv_image_create(codex_pet_slot);
    lv_image_set_src(codex_pet_sprite, &pet_miku_idle_dsc);
    lv_obj_set_pos(codex_pet_sprite, 20, 4);

    codex_output_status_label = make_label(reply_panel, "LIVE", 224, 138, 84,
                                           &font_styrene_12, 0x8ee8c1);
    lv_obj_set_style_text_align(codex_output_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(codex_output_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_bg_color(codex_output_status_label, lv_color_hex(0x0d1919), 0);
    lv_obj_set_style_bg_opa(codex_output_status_label, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(codex_output_status_label, 9, 0);
    lv_obj_set_style_pad_top(codex_output_status_label, 3, 0);
    lv_obj_set_style_pad_bottom(codex_output_status_label, 2, 0);
    lv_obj_set_style_border_width(codex_output_status_label, 1, 0);
    lv_obj_set_style_border_color(codex_output_status_label, lv_color_hex(0x35d49a), 0);
    codex_project_label = nullptr;
    codex_output_bubble = make_panel(reply_panel, 14, 18, 296, 118, 0xf5f6f7);
    lv_obj_set_style_radius(codex_output_bubble, 16, 0);
    lv_obj_set_style_border_width(codex_output_bubble, 1, 0);
    lv_obj_set_style_border_color(codex_output_bubble, lv_color_hex(0xcdd5dc), 0);
    lv_obj_set_style_pad_all(codex_output_bubble, 0, 0);

    codex_output_body_label = make_label(codex_output_bubble,
                                         "Waiting for Codex output...",
                                         12, 12, 268, &font_cjk_18, 0x111820);
    lv_obj_set_size(codex_output_body_label, 268, 98);
    lv_label_set_long_mode(codex_output_body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(codex_output_body_label, 2, 0);
    for (uint8_t i = 0; i < CODEX_OUTPUT_VISIBLE_LINES; i++) {
        codex_output_line_labels[i] = make_label(codex_output_bubble, "",
                                                 12, 10 + (int32_t)i * 22, 268,
                                                 &font_cjk_18, 0x111820);
        lv_label_set_long_mode(codex_output_line_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_size(codex_output_line_labels[i], 268, 20);
        lv_obj_add_flag(codex_output_line_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
    codex_context_label = make_label(reply_panel, "CTX --", 22, 140, 74,
                                     &font_styrene_12, 0x35d49a);
    codex_context_value_label = make_label(reply_panel, "-- / --", 96, 140, 118,
                                           &font_styrene_12, 0x7f9290);
    lv_obj_set_style_text_align(codex_context_value_label, LV_TEXT_ALIGN_RIGHT, 0);
    codex_context_bar = make_bar(reply_panel, 22, 160, 276, 0, 0x35d49a);
    codex_output_footer_label = nullptr;
    codex_output_image = nullptr;
    lv_obj_add_flag(codex_pet_sprite, LV_OBJ_FLAG_HIDDEN);
    apply_pet_theme("miku", "Miku");
}

static void update_codex_pet_motion(uint32_t now) {
    (void)now;
    if (!codex_pet_sprite || !codex_pet_sprite_pixels) return;
    if (lv_obj_get_x(codex_pet_sprite) != codex_pet_base_x ||
        lv_obj_get_y(codex_pet_sprite) != codex_pet_base_y) {
        lv_obj_set_pos(codex_pet_sprite, codex_pet_base_x, codex_pet_base_y);
    }
}

static void toggle_codex_pet_running_from_touch() {
    static uint32_t last_run_touch_ms = 0;
    uint32_t now = millis();
    if (now - last_run_touch_ms < 350) return;
    last_run_touch_ms = now;

    bool should_run = strcmp(codex_pet_motion_state, "running") != 0;
    if (should_run) {
        strlcpy(codex_pet_motion_state, "running", sizeof(codex_pet_motion_state));
        codex_touch_user_stopped = false;
        codex_pet_anim_paused = false;
        codex_pet_anim_last_ms = now;
        if (!select_codex_touch_running_animation()) {
            apply_pet_sprite_frame(0);
        }
    } else {
        strlcpy(codex_pet_motion_state, "idle", sizeof(codex_pet_motion_state));
        codex_touch_user_stopped = true;
        codex_pet_anim_paused = false;
        codex_pet_anim_last_ms = now;
        if (!select_codex_idle_animation()) {
            apply_pet_sprite_frame(0);
        }
    }

    codex_pet_motion_last_ms = now;
    codex_touch_feedback_until_ms = now;
    codex_touch_running_count++;
    if (codex_state_label) {
        lv_label_set_text(codex_state_label, should_run ? "running" : "idle");
    }
    if (codex_activity_label) {
        lv_label_set_text(codex_activity_label, should_run ? "touch: running" : "touch: idle");
    }
}

static bool codex_touch_hits_pet_area(uint16_t logical_x, uint16_t logical_y) {
    const uint16_t pet_left = 152;
    const uint16_t pet_right = 320;
    const uint16_t pet_top = 20;
    const uint16_t pet_bottom = 168;
    return logical_x >= pet_left && logical_x < pet_right &&
           logical_y >= pet_top && logical_y < pet_bottom;
}

static bool codex_touch_hits_quota_area(uint16_t logical_x, uint16_t logical_y) {
    return logical_x >= 8 && logical_x < 144 &&
           logical_y >= 10 && logical_y < 160;
}

static bool codex_touch_hits_message_area(uint16_t logical_x, uint16_t logical_y) {
    return logical_x >= 320 && logical_x < LCD_WIDTH &&
           logical_y >= 18 && logical_y < 136;
}

static void request_codex_quota_refresh_from_touch() {
    static uint32_t last_refresh_touch_ms = 0;
    uint32_t now = millis();
    if (now - last_refresh_touch_ms < 1000) return;
    last_refresh_touch_ms = now;
    codex_quota_refresh_pending = true;
    codex_quota_feedback_until_ms = now + 5000;
    if (codex_status_label) {
        lv_label_set_text(codex_status_label, "refreshing...");
        lv_obj_set_style_text_color(codex_status_label, lv_color_hex(0x28dff0), 0);
    }
    set_codex_quota_card_feedback(LV_OPA_COVER);
    ble_request_refresh();
    Serial.println("REFRESH_REQUEST touch=quota");
}

static void request_codex_next_session_from_boot() {
    static bool stable_down = false;
    static bool last_raw_down = false;
    static uint32_t raw_changed_ms = 0;
    static uint32_t stable_down_ms = 0;
    static uint32_t last_boot_action_ms = 0;
    static bool long_press_handled = false;

    uint32_t now = millis();
    bool raw_down = digitalRead(BTN_BACK) == LOW;
    if (raw_down != last_raw_down) {
        last_raw_down = raw_down;
        raw_changed_ms = now;
        return;
    }
    if (now - raw_changed_ms < 45 || raw_down == stable_down) return;

    bool was_down = stable_down;
    stable_down = raw_down;
    if (stable_down) {
        stable_down_ms = now;
        long_press_handled = false;
        return;
    }

    if (was_down) {
        uint32_t held_ms = now - stable_down_ms;
        if (held_ms >= 1200 && now - last_boot_action_ms >= 700) {
            last_boot_action_ms = now;
            long_press_handled = true;
            if (!codex_session_list_open) {
                open_codex_session_list();
            } else {
                render_codex_session_list();
                ble_request_session_list();
            }
            if (codex_output_status_label) {
                lv_label_set_text(codex_output_status_label, "LIST");
            }
            Serial.println("SESSION_LIST_REQUEST boot");
            return;
        }
        if (!long_press_handled && held_ms >= 120 && held_ms < 1200 &&
            now - last_boot_action_ms >= 700) {
            last_boot_action_ms = now;
            close_codex_session_list();
            if (codex_output_status_label) {
                lv_label_set_text(codex_output_status_label, "NEXT");
            }
            ble_request_session_next();
            Serial.println("SESSION_NEXT_REQUEST boot");
        }
        long_press_handled = false;
    }
}

static void handle_codex_touch_actions() {
    static bool was_pressed = false;
    static uint16_t start_x = 0;
    static uint16_t start_y = 0;
    static uint32_t start_ms = 0;
    static bool start_in_message = false;
    static bool start_in_quota = false;

    if (touch_pressed && !was_pressed) {
        start_x = touch_x;
        start_y = touch_y;
        start_ms = millis();
        start_in_message = codex_touch_hits_message_area(start_x, start_y);
        start_in_quota = codex_touch_hits_quota_area(start_x, start_y);
        if (codex_touch_hits_pet_area(start_x, start_y)) {
            toggle_codex_pet_running_from_touch();
        }
    } else if (!touch_pressed && was_pressed) {
        uint32_t elapsed = millis() - start_ms;
        int32_t dx = (int32_t)touch_x - (int32_t)start_x;
        int32_t dy = (int32_t)touch_y - (int32_t)start_y;
        if (start_in_message && elapsed < 1500 &&
            (dy > 14 || dy < -14) && dx > -48 && dx < 48) {
            if (codex_session_list_open) {
                move_codex_session_selection(dy < 0 ? 1 : -1);
            } else {
                scroll_codex_output_window(dy < 0 ? 1 : -1);
            }
        } else if (start_in_message && elapsed < 500 &&
                   dx > -10 && dx < 10 && dy > -10 && dy < 10 &&
                   codex_touch_hits_message_area(touch_x, touch_y)) {
            if (codex_session_list_open) {
                select_codex_session_from_touch();
            } else {
                open_codex_session_list();
            }
        } else if (start_in_quota && elapsed < 900 &&
                   dx > -24 && dx < 24 && dy > -24 && dy < 24 &&
                   codex_touch_hits_quota_area(touch_x, touch_y)) {
            request_codex_quota_refresh_from_touch();
        }
        start_in_message = false;
        start_in_quota = false;
    }
    was_pressed = touch_pressed;
}

static void update_codex_touch_feedback(uint32_t now) {
    if (codex_quota_refresh_pending) {
        if (codex_quota_feedback_until_ms && now > codex_quota_feedback_until_ms) {
            codex_quota_refresh_pending = false;
            codex_quota_feedback_until_ms = now + 900;
            if (codex_status_label) {
                lv_label_set_text(codex_status_label, "try again");
                lv_obj_set_style_text_color(codex_status_label, lv_color_hex(0xffbd4a), 0);
            }
            set_codex_quota_card_feedback(LV_OPA_COVER);
            return;
        }
        uint8_t opa = ((now / 180) % 2) ? LV_OPA_COVER : LV_OPA_60;
        set_codex_quota_card_feedback(opa);
    } else if (codex_quota_feedback_until_ms && now > codex_quota_feedback_until_ms) {
        codex_quota_feedback_until_ms = 0;
        update_codex_usage_screen(&usage);
    }
}

#if BOARD_SELF_TEST
static void build_self_test_screen() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x080807), 0);

#if BOARD_WAVESHARE_LCD_349
    build_codex_pet_screen();
    return;
#else
    lv_obj_t *left = lv_obj_create(scr);
    lv_obj_set_size(left, 112, LCD_HEIGHT);
    lv_obj_set_pos(left, 0, 0);
    lv_obj_set_style_bg_color(left, lv_color_hex(0x20263a), 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_radius(left, 0, 0);

    lv_obj_t *mid = lv_obj_create(scr);
    lv_obj_set_size(mid, 168, LCD_HEIGHT);
    lv_obj_set_pos(mid, 112, 0);
    lv_obj_set_style_bg_color(mid, lv_color_hex(0x24382f), 0);
    lv_obj_set_style_border_width(mid, 0, 0);
    lv_obj_set_style_radius(mid, 0, 0);

    lv_obj_t *right = lv_obj_create(scr);
    lv_obj_set_size(right, LCD_WIDTH - 280, LCD_HEIGHT);
    lv_obj_set_pos(right, 280, 0);
    lv_obj_set_style_bg_color(right, lv_color_hex(0x3a2924), 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_radius(right, 0, 0);
#endif

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP32-S3-Touch-LCD-3.49");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, LCD_WIDTH - 16);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 14);

    lv_obj_t *layout = lv_label_create(scr);
#if BOARD_WAVESHARE_LCD_349
    lv_label_set_text(layout, "172 x 640");
#else
    lv_label_set_text(layout, "112 | 168 | 360");
#endif
    lv_obj_set_style_text_color(layout, lv_color_hex(0xd6d0c4), 0);
    lv_obj_align(layout, LV_ALIGN_LEFT_MID, 8, 18);

    touch_label = lv_label_create(scr);
    lv_label_set_text(touch_label, "touch: --, --");
    lv_obj_set_style_text_color(touch_label, lv_color_hex(0xffffff), 0);
    lv_obj_align(touch_label, LV_ALIGN_BOTTOM_LEFT, 8, -14);
}
#endif

static void update_codex_strip_anim() {
#if BOARD_WAVESHARE_LCD_349
    static uint32_t last = 0;
    static uint8_t phase = 0;
    uint32_t now = millis();
    handle_codex_touch_actions();
    update_codex_touch_feedback(now);
    if (codex_pet_anim_frames > 1 && codex_pet_frame_size > 0 &&
        !codex_pet_anim_paused &&
        now - codex_pet_anim_last_ms >= codex_pet_frame_durations[codex_pet_anim_index]) {
        codex_pet_anim_last_ms = now;
        uint8_t next = codex_pet_anim_index + 1;
        if (next >= codex_pet_anim_frames) {
            next = codex_pet_anim_loop_start;
        }
        apply_pet_sprite_frame(next);
    }
    update_codex_output_typing();
    update_codex_pet_motion(now);
    if (now - last < 450) return;
    last = now;
    phase = (phase + 1) % 4;

    static const char *states[] = {
        "thinking", "thinking.", "thinking..", "thinking..."
    };
    if (codex_state_label) lv_label_set_text(codex_state_label, states[phase]);
    if (codex_pulse) {
        lv_obj_set_style_bg_opa(codex_pulse, phase % 2 ? LV_OPA_90 : LV_OPA_50, 0);
    }
    if (codex_activity_label && touch_pressed) {
        lv_label_set_text_fmt(codex_activity_label, "touch %u,%u", touch_x, touch_y);
    } else if (codex_activity_label) {
        lv_label_set_text(codex_activity_label, "live bridge pending");
    }
#else
    if (!touch_label) return;
    static bool last_pressed = false;
    static uint16_t last_x = UINT16_MAX;
    static uint16_t last_y = UINT16_MAX;
    if (touch_pressed == last_pressed && touch_x == last_x && touch_y == last_y) {
        return;
    }
    last_pressed = touch_pressed;
    last_x = touch_x;
    last_y = touch_y;
    if (touch_pressed) {
        lv_label_set_text_fmt(touch_label, "touch: %u, %u", touch_x, touch_y);
    } else {
        lv_label_set_text(touch_label, "touch: released");
    }
#endif
}
#endif

// Parse a JSON line into UsageData
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    JsonVariant session_pct = doc["p"];
    JsonVariant session_reset = doc["pr"];
    out->session_pct = session_pct.isNull() ? (doc["s"] | 0.0f) : (session_pct | 0.0f);
    out->session_reset_mins = session_reset.isNull() ? (doc["sr"] | -1) : (session_reset | -1);
    strlcpy(out->session_reset_label, doc["prl"] | "", sizeof(out->session_reset_label));
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->weekly_reset_label, doc["wrl"] | "", sizeof(out->weekly_reset_label));
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    strlcpy(out->plan, doc["plan"] | "", sizeof(out->plan));
    strlcpy(out->limit, doc["limit"] | "", sizeof(out->limit));
    out->ok = doc["ok"] | false;
    out->valid = true;
    return true;
}

#if BOARD_CODEX_STRIP
static char codex_chunk_id[24] = "";
static char codex_chunk_buf[4096] = "";
static uint8_t codex_chunk_next = 0;
static uint8_t codex_chunk_total = 0;

static bool handle_codex_strip_payload(const char *json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    const char *kind = doc["kind"] | "codex_usage";
    if (strcmp(kind, "chunk") == 0) {
        const char *id = doc["id"] | "";
        const char *data = doc["d"] | "";
        uint8_t index = doc["i"] | 0;
        uint8_t total = doc["n"] | 0;
        if (!id[0] || !data || total == 0 || total > 16) {
            return false;
        }
        if (index == 0) {
            strlcpy(codex_chunk_id, id, sizeof(codex_chunk_id));
            codex_chunk_buf[0] = '\0';
            codex_chunk_next = 0;
            codex_chunk_total = total;
        }
        if (strcmp(codex_chunk_id, id) != 0 || index != codex_chunk_next ||
            total != codex_chunk_total) {
            codex_chunk_id[0] = '\0';
            codex_chunk_buf[0] = '\0';
            codex_chunk_next = 0;
            codex_chunk_total = 0;
            return false;
        }
        size_t used = strlen(codex_chunk_buf);
        size_t incoming = strlen(data);
        if (used + incoming >= sizeof(codex_chunk_buf)) {
            codex_chunk_id[0] = '\0';
            codex_chunk_buf[0] = '\0';
            codex_chunk_next = 0;
            codex_chunk_total = 0;
            return false;
        }
        memcpy(codex_chunk_buf + used, data, incoming + 1);
        codex_chunk_next++;
        if (codex_chunk_next < codex_chunk_total) {
            return true;
        }
        char assembled[sizeof(codex_chunk_buf)];
        strlcpy(assembled, codex_chunk_buf, sizeof(assembled));
        codex_chunk_id[0] = '\0';
        codex_chunk_buf[0] = '\0';
        codex_chunk_next = 0;
        codex_chunk_total = 0;
        return handle_codex_strip_payload(assembled);
    }

    if (strcmp(kind, "pet_state") == 0) {
        const char *state = doc["state"] | "thinking";
        const char *pet = doc["pet"] | "";
        const char *name = doc["name"] | "Pet";
        bool wants_running = strcmp(state, "running") == 0 || strcmp(state, "thinking") == 0;
        if (wants_running && codex_touch_user_stopped) {
            strlcpy(codex_pet_motion_state, "idle", sizeof(codex_pet_motion_state));
            select_codex_idle_animation();
        } else {
            strlcpy(codex_pet_motion_state, state, sizeof(codex_pet_motion_state));
            if (wants_running) {
                select_codex_touch_running_animation();
            } else {
                codex_touch_user_stopped = false;
            }
        }
        apply_pet_theme(pet, name);
        if (codex_state_label) lv_label_set_text(codex_state_label, state);
        if (codex_activity_label) {
            lv_label_set_text_fmt(codex_activity_label, "%s / %s", name, state);
        }
        Serial.printf("pet_state: %s %s\n", name, state);
        return true;
    }

    if (strcmp(kind, "session_list") == 0) {
        update_codex_session_list(doc);
        Serial.printf("session_list: %u selected=%u\n",
                      (unsigned)codex_session_count,
                      (unsigned)codex_session_selected);
        return true;
    }

    if (strcmp(kind, "codex_output") == 0) {
        const char *body = doc["raw_text"] | "";
        if (!body || !body[0]) {
            body = doc["text"] | "";
        }
        if (!body || !body[0]) {
            body = doc["t"] | "";
        }
        const char *project = doc["project"] | "";
        if (!project || !project[0]) project = doc["p"] | "";
        const char *status = doc["status"] | "";
        if (!status || !status[0]) status = doc["s"] | "";
        const char *footer = doc["footer"] | "";
        if (!footer || !footer[0]) footer = doc["f"] | "";
        const char *line1 = doc["l1"] | "";
        const char *line2 = doc["l2"] | "";
        const char *line3 = doc["l3"] | "";
        const char *line4 = doc["l4"] | "";
        const char *line5 = doc["l5"] | "";
        const char *line6 = doc["l6"] | "";
        const char *line7 = doc["l7"] | "";
        const char *line8 = doc["l8"] | "";
        if (line1 && line1[0]) {
            update_codex_output_lines(project, status, line1, line2, line3, line4,
                                      line5, line6, line7, line8);
        } else {
            update_codex_output_screen(project,
                                       status,
                                       body,
                                       footer);
        }
        uint32_t context_input = doc["cti"] | 0;
        if (context_input == 0) context_input = doc["ci"] | 0;
        uint32_t context_window = doc["ctw"] | 0;
        if (context_window == 0) context_window = doc["cw"] | 0;
        if (context_input > 0 && context_window > 0) {
            update_codex_context_screen(context_input, context_window);
        }
        Serial.printf("codex_output: %s\n", project);
        return true;
    }

    if (!parse_json(json, &usage)) {
        return false;
    }
    usage_rate_sample(usage.session_pct);
    update_codex_usage_screen(&usage);
    Serial.printf("codex_usage: plan=%s limit=%s p=%.0f w=%.0f st=%s\n",
                  usage.plan, usage.limit, usage.session_pct,
                  usage.weekly_pct, usage.status);
    return true;
}
#endif

// Serial command buffer
#define CMD_BUF_SIZE 1024
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n", (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");

    heap_caps_free(sbuf);
}

static bool receive_pet_sprite(uint16_t w, uint16_t h, uint32_t size) {
    if (w == 0 || h == 0 || size != (uint32_t)w * h * 2 || size > 120000) {
        Serial.println("PET_SPRITE_NACK");
        return false;
    }

    uint8_t *pixels = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        pixels = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    if (!pixels) {
        Serial.println("PET_SPRITE_NACK");
        return false;
    }

    uint32_t got = 0;
    uint32_t deadline = millis() + 30000;
    while (got < size && (int32_t)(millis() - deadline) < 0) {
        int available = Serial.available();
        if (available <= 0) {
            delay(1);
            continue;
        }
        uint32_t chunk = size - got;
        if (chunk > (uint32_t)available) chunk = available;
        got += Serial.readBytes(pixels + got, chunk);
    }

    if (got != size || !update_pet_sprite_from_rgb565(w, h, pixels, size)) {
        heap_caps_free(pixels);
        Serial.println("PET_SPRITE_NACK");
        return false;
    }

    Serial.printf("PET_SPRITE_ACK %u %u %lu\n", w, h, (unsigned long)size);
    return true;
}

static bool receive_output_image(uint16_t w, uint16_t h, uint32_t size) {
    if (w == 0 || h == 0 || size != (uint32_t)w * h * 2 ||
        w > (LCD_WIDTH - 320) || h > LCD_HEIGHT || size > 120000) {
        Serial.println("OUTPUT_IMAGE_NACK");
        return false;
    }

    if (codex_output_image && codex_output_image_pixels &&
        codex_output_image_dsc.data_size == size &&
        codex_output_image_dsc.header.w == w &&
        codex_output_image_dsc.header.h == h) {
        uint32_t got = 0;
        uint32_t deadline = millis() + 30000;
        while (got < size && (int32_t)(millis() - deadline) < 0) {
            int available = Serial.available();
            if (available <= 0) {
                delay(1);
                continue;
            }
            uint32_t chunk = size - got;
            if (chunk > (uint32_t)available) chunk = available;
            got += Serial.readBytes(codex_output_image_pixels + got, chunk);
        }
        if (got != size) {
            Serial.println("OUTPUT_IMAGE_NACK");
            return false;
        }
        lv_obj_move_foreground(codex_output_image);
        lv_obj_clear_flag(codex_output_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(codex_output_image);
        Serial.printf("OUTPUT_IMAGE_ACK %u %u %lu\n", w, h, (unsigned long)size);
        return true;
    }

    uint8_t *pixels = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        pixels = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    if (!pixels) {
        Serial.println("OUTPUT_IMAGE_NACK");
        return false;
    }

    uint32_t got = 0;
    uint32_t deadline = millis() + 30000;
    while (got < size && (int32_t)(millis() - deadline) < 0) {
        int available = Serial.available();
        if (available <= 0) {
            delay(1);
            continue;
        }
        uint32_t chunk = size - got;
        if (chunk > (uint32_t)available) chunk = available;
        got += Serial.readBytes(pixels + got, chunk);
    }

    if (got != size || !update_codex_output_image_from_rgb565(w, h, pixels, size)) {
        heap_caps_free(pixels);
        Serial.println("OUTPUT_IMAGE_NACK");
        return false;
    }

    Serial.printf("OUTPUT_IMAGE_ACK %u %u %lu\n", w, h, (unsigned long)size);
    return true;
}

static void parse_pet_anim_durations(const char *text, uint8_t frames,
                                     uint16_t *durations_out) {
    for (uint8_t i = 0; i < CODEX_PET_MAX_ANIM_FRAMES; i++) {
        durations_out[i] = 180;
    }
    if (!text || !text[0]) {
        return;
    }

    const char *p = text;
    for (uint8_t i = 0; i < frames && i < CODEX_PET_MAX_ANIM_FRAMES; i++) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char *end = nullptr;
        unsigned long value = strtoul(p, &end, 10);
        if (end == p) break;
        if (value > 0 && value <= 10000) {
            durations_out[i] = (uint16_t)value;
        }
        p = end;
    }
}

static void parse_pet_anim_indexes(const char *text, uint8_t frames,
                                   uint8_t *indexes_out) {
    for (uint8_t i = 0; i < CODEX_PET_MAX_ANIM_FRAMES; i++) {
        indexes_out[i] = 0;
    }
    if (!text || !text[0]) {
        return;
    }

    const char *p = text;
    for (uint8_t i = 0; i < frames && i < CODEX_PET_MAX_ANIM_FRAMES; i++) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char *end = nullptr;
        unsigned long value = strtoul(p, &end, 10);
        if (end == p) break;
        if (value < CODEX_PET_MAX_ATLAS_FRAMES) {
            indexes_out[i] = (uint8_t)value;
        }
        p = end;
    }
}

static bool receive_pet_atlas(uint16_t w, uint16_t h, uint8_t frames, uint32_t size) {
    const uint32_t frame_size = (uint32_t)w * h * 2;
    if (w == 0 || h == 0 || frames == 0 || frames > CODEX_PET_MAX_ATLAS_FRAMES ||
        frame_size == 0 || size != frame_size * frames || size > 2600000) {
        Serial.println("PET_ATLAS_NACK");
        return false;
    }

    uint8_t *pixels = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        pixels = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    if (!pixels) {
        Serial.println("PET_ATLAS_NACK");
        return false;
    }

    uint32_t got = 0;
    uint32_t deadline = millis() + 180000;
    while (got < size && (int32_t)(millis() - deadline) < 0) {
        int available = Serial.available();
        if (available <= 0) {
            delay(1);
            continue;
        }
        uint32_t chunk = size - got;
        if (chunk > (uint32_t)available) chunk = available;
        got += Serial.readBytes(pixels + got, chunk);
    }

    if (got != size) {
        heap_caps_free(pixels);
        Serial.println("PET_ATLAS_NACK");
        return false;
    }

#if BOARD_CODEX_STRIP
    bool cache_ok = save_pet_atlas_cache(w, h, frames, size, pixels);
#endif

    if (!update_pet_atlas_from_rgb565(w, h, frames, pixels, size)) {
        heap_caps_free(pixels);
        Serial.println("PET_ATLAS_NACK");
        return false;
    }

    Serial.printf("PET_ATLAS_ACK %u %u %u %lu\n", w, h, frames, (unsigned long)size);
#if BOARD_CODEX_STRIP
    Serial.printf("PET_ATLAS_CACHE_%s\n", cache_ok ? "ACK" : "NACK");
#endif
    return true;
}

static bool receive_pet_animation(uint16_t w, uint16_t h, uint8_t frames, uint32_t size,
                                  uint8_t loop_start, const uint16_t *durations) {
    const uint32_t frame_size = (uint32_t)w * h * 2;
    if (w == 0 || h == 0 || frames == 0 || frames > CODEX_PET_MAX_ANIM_FRAMES ||
        frame_size == 0 || size != frame_size * frames || size > 950000) {
        Serial.println("PET_ANIM_NACK");
        return false;
    }

    uint8_t *pixels = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        pixels = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    if (!pixels) {
        Serial.println("PET_ANIM_NACK");
        return false;
    }

    uint32_t got = 0;
    uint32_t deadline = millis() + 120000;
    while (got < size && (int32_t)(millis() - deadline) < 0) {
        int available = Serial.available();
        if (available <= 0) {
            delay(1);
            continue;
        }
        uint32_t chunk = size - got;
        if (chunk > (uint32_t)available) chunk = available;
        got += Serial.readBytes(pixels + got, chunk);
    }

    if (got != size) {
        heap_caps_free(pixels);
        Serial.println("PET_ANIM_NACK");
        return false;
    }

#if BOARD_CODEX_STRIP
    bool cache_ok = save_pet_animation_cache(w, h, frames, size, loop_start, durations, pixels);
#endif

    if (!update_pet_animation_from_rgb565(w, h, frames, pixels, size, durations, loop_start)) {
        heap_caps_free(pixels);
        Serial.println("PET_ANIM_NACK");
        return false;
    }

    Serial.printf("PET_ANIM_ACK %u %u %u %lu\n", w, h, frames, (unsigned long)size);
#if BOARD_CODEX_STRIP
    Serial.printf("PET_CACHE_%s\n", cache_ok ? "ACK" : "NACK");
#endif
    return true;
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) {
                send_screenshot();
#if BOARD_CODEX_STRIP
            } else if (strcmp(cmd_buf, "BLE_STATUS") == 0) {
                Serial.printf("BLE_STATUS state=%d adv=%d name=%s mac=%s\n",
                              (int)ble_get_state(),
                              ble_advertising_started() ? 1 : 0,
                              ble_get_device_name(),
                              ble_get_mac_address());
            } else if (strcmp(cmd_buf, "BLE_CLEAR") == 0) {
                ble_clear_bonds();
                Serial.println("BLE_CLEAR_ACK");
            } else if (strcmp(cmd_buf, "PET_ANIM_STATUS") == 0) {
                Serial.printf("PET_ANIM_STATUS frames=%u atlas=%u index=%u loop=%u state=%s feedback=%lu durations=",
                              codex_pet_anim_frames,
                              codex_pet_uses_atlas ? 1 : 0,
                              codex_pet_anim_index,
                              codex_pet_anim_loop_start,
                              codex_pet_motion_state,
                              (unsigned long)codex_touch_feedback_until_ms);
                for (uint8_t i = 0; i < codex_pet_anim_frames && i < CODEX_PET_MAX_ANIM_FRAMES; i++) {
                    if (i) Serial.print(",");
                    Serial.print(codex_pet_frame_durations[i]);
                }
                Serial.printf(" pos=%ld,%ld\n",
                              (long)codex_pet_base_x,
                              (long)codex_pet_base_y);
            } else if (strcmp(cmd_buf, "TOUCH_STATUS") == 0) {
                Serial.printf("TOUCH_STATUS pressed=%d raw=%u,%u logical=%u,%u events=%lu running=%lu state=%s\n",
                              touch_pressed ? 1 : 0,
                              (unsigned)touch_raw_x,
                              (unsigned)touch_raw_y,
                              (unsigned)touch_x,
                              (unsigned)touch_y,
                              (unsigned long)touch_event_count,
                              (unsigned long)codex_touch_running_count,
                              codex_pet_motion_state);
            } else if (strncmp(cmd_buf, "PET_SPRITE_START ", 17) == 0) {
                unsigned int w = 0;
                unsigned int h = 0;
                unsigned long size = 0;
                if (sscanf(cmd_buf + 17, "%u %u %lu", &w, &h, &size) == 3) {
                    receive_pet_sprite((uint16_t)w, (uint16_t)h, (uint32_t)size);
                } else {
                    Serial.println("PET_SPRITE_NACK");
                }
            } else if (strncmp(cmd_buf, "PET_ANIM_START ", 15) == 0) {
                unsigned int w = 0;
                unsigned int h = 0;
                unsigned int frames = 0;
                unsigned int loop_start = 0;
                unsigned long size = 0;
                int consumed = 0;
                if (sscanf(cmd_buf + 15, "%u %u %u %lu %u %n",
                           &w, &h, &frames, &size, &loop_start, &consumed) >= 4) {
                    uint16_t durations[CODEX_PET_MAX_ANIM_FRAMES];
                    const char *duration_text = consumed > 0 ? cmd_buf + 15 + consumed : "";
                    parse_pet_anim_durations(duration_text, (uint8_t)frames, durations);
                    receive_pet_animation((uint16_t)w, (uint16_t)h, (uint8_t)frames,
                                          (uint32_t)size, (uint8_t)loop_start,
                                          durations);
                } else {
                    Serial.println("PET_ANIM_NACK");
                }
            } else if (strncmp(cmd_buf, "PET_ATLAS_START ", 16) == 0) {
                unsigned int w = 0;
                unsigned int h = 0;
                unsigned int frames = 0;
                unsigned long size = 0;
                if (sscanf(cmd_buf + 16, "%u %u %u %lu", &w, &h, &frames, &size) == 4) {
                    receive_pet_atlas((uint16_t)w, (uint16_t)h, (uint8_t)frames, (uint32_t)size);
                } else {
                    Serial.println("PET_ATLAS_NACK");
                }
            } else if (strncmp(cmd_buf, "PET_ANIM_SELECT ", 16) == 0) {
                unsigned int frames = 0;
                unsigned int loop_start = 0;
                char indexes_text[256] = {0};
                char durations_text[256] = {0};
                if (sscanf(cmd_buf + 16, "%u %u %255s %255s",
                           &frames, &loop_start, indexes_text, durations_text) >= 3) {
                    uint8_t indexes[CODEX_PET_MAX_ANIM_FRAMES];
                    uint16_t durations[CODEX_PET_MAX_ANIM_FRAMES];
                    parse_pet_anim_indexes(indexes_text, (uint8_t)frames, indexes);
                    parse_pet_anim_durations(durations_text, (uint8_t)frames, durations);
                    if (select_pet_animation_from_atlas((uint8_t)frames, (uint8_t)loop_start,
                                                        indexes, durations)) {
                        Serial.printf("PET_ANIM_SELECT_ACK %u\n", frames);
                    } else {
                        Serial.println("PET_ANIM_SELECT_NACK");
                    }
                } else {
                    Serial.println("PET_ANIM_SELECT_NACK");
                }
            } else if (strncmp(cmd_buf, "OUTPUT_IMAGE_START ", 19) == 0) {
                unsigned int w = 0;
                unsigned int h = 0;
                unsigned long size = 0;
                if (sscanf(cmd_buf + 19, "%u %u %lu", &w, &h, &size) == 3) {
                    receive_output_image((uint16_t)w, (uint16_t)h, (uint32_t)size);
                } else {
                    Serial.println("OUTPUT_IMAGE_NACK");
                }
            } else if (cmd_buf[0] == '{') {
                Serial.println(handle_codex_strip_payload(cmd_buf) ? "JSON_ACK" : "JSON_NACK");
#endif
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

void setup() {
    Serial.setRxBufferSize(4096);
    Serial.begin(921600);
    delay(300);
    Serial.println("{\"ready\":true}");
#if BOARD_WAVESHARE_LCD_349
    relax_task_watchdog_for_display();
#endif

    // Init I2C (shared by touch + PMU)
#if BOARD_WAVESHARE_LCD_349
    Wire.begin(SENSOR_SDA, SENSOR_SCL);
    TouchWire.begin(TOUCH_SDA, TOUCH_SCL);
#else
    Wire.begin(IIC_SDA, IIC_SCL);
#endif

    // Init display
#if BOARD_HAS_LCD_PWM_BL
    ledcAttach(LCD_BL, 50000, 8);
#endif
#if BOARD_WAVESHARE_LCD_349
    board_display_init();
#else
    gfx->begin();
    gfx->fillScreen(0x0000);
#endif
    board_set_brightness(200);

    // Init PMU
    power_init();

    // Init IMU (accelerometer for auto-rotation)
    imu_init();

    // Init touch
#if BOARD_WAVESHARE_LCD_349
    Serial.println("Touch polling init OK");
#else
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
    } else {
        touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
        touch.setSwapXY(true);
        touch.setMirrorXY(true, false);
        attachInterrupt(TP_INT, touch_isr, FALLING);
        Serial.println("Touch init OK");
    }
#endif

    // Init LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    // Allocate partial render buffers
#if BOARD_WAVESHARE_LCD_349
    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    lcd_dma_buf = (uint16_t*)heap_caps_malloc(LCD_NATIVE_WIDTH * BUF_LINES * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    rot_buf = nullptr;
#else
    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    // rot_buf needs to hold the largest possible strip after rotation
    // A 480×40 strip rotated 90° becomes 40×480, same pixel count
    rot_buf = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
#endif
    if (!buf1 || !buf2
#if BOARD_WAVESHARE_LCD_349
        || !lcd_dma_buf
#endif
    ) {
        Serial.println("LVGL buffer alloc failed");
        while (true) delay(1000);
    }
#if BOARD_WAVESHARE_LCD_349
    lcd_clear_native(0x0000);
#endif

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
#if BOARD_WAVESHARE_LCD_349
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * LCD_HEIGHT * 2,
                           LV_DISPLAY_RENDER_MODE_FULL);
#else
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
#endif

    // CO5300 even-alignment rounder
#if BOARD_WAVESHARE_AMOLED_216
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
#endif

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

#if BOARD_SELF_TEST
    build_self_test_screen();
#elif BOARD_CODEX_STRIP
    // Init BLE data channel and build the 640x172 Codex pet strip.
    pinMode(BTN_BACK, INPUT_PULLUP);
    ble_init();
    build_codex_pet_screen();
    load_pet_animation_cache();
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    Serial.println("Codex strip ready, waiting for codex_usage payloads...");
#else
    // Init BLE data channel
    ble_init();

    // Physical buttons: back (GPIO 0) and forward (GPIO 18)
#if BOARD_HAS_SIDE_BUTTONS
    pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BTN_FWD,  INPUT_PULLUP);
#endif

    // Build dashboard
    ui_init();

    // Show initial BLE status on Bluetooth screen
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());

    // Show initial battery status
    ui_update_battery(power_battery_pct(), power_is_charging());

    ui_show_screen(SCREEN_SPLASH);

    Serial.println("Dashboard ready, waiting for data on BLE...");
#endif
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Brightness ramp state for rotation transition
// On rotation change we blank the panel, force a full LVGL redraw at the
// new orientation, then ramp brightness back up over ~125ms so the
// transition reads as deliberate instead of as a glitch.
static void handle_rotation_change(void) {
    static uint8_t last_rotation = 0;
    static uint8_t  ramp_step = 0;  // 0=idle, 1-4=ramping
    static uint32_t ramp_last = 0;

    uint8_t rot = imu_get_rotation();
    if (rot != last_rotation) {
        board_set_brightness(0);
        last_rotation = rot;
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
        return;
    }

    if (ramp_step == 0) return;
    uint32_t now = millis();
    if (now - ramp_last < 25) return;
    ramp_last = now;

    static const uint8_t levels[] = {60, 120, 170, 200};
    board_set_brightness(levels[ramp_step - 1]);
    if (ramp_step >= 4) ramp_step = 0;
    else                ramp_step++;
}

static void shutdown_device() {
    Serial.println("PWR long press: shutting down");

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    panel_style(scr, 0x000000);
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "OFF");
    lv_obj_set_style_text_color(label, lv_color_hex(0xf4efe7), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_center(label);
    lv_timer_handler();
    delay(450);

    board_set_brightness(0);
    lv_timer_handler();
    delay(50);
    power_shutdown_now();
    delay(250);

    Serial.println("Entering deep sleep fallback");
    Serial.flush();
    esp_deep_sleep_start();
}

void loop() {
    touch_read();
    lv_timer_handler();
    power_tick();
    if (power_shutdown_requested()) {
        shutdown_device();
    }
#if BOARD_SELF_TEST
    update_codex_strip_anim();
#elif BOARD_CODEX_STRIP
    update_codex_strip_anim();
    request_codex_next_session_from_boot();
    ble_tick();
    check_serial_cmd();

    if (ble_has_data()) {
        if (handle_codex_strip_payload(ble_get_data())) {
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }
#else
    ui_tick_anim();
    ble_tick();
    imu_tick();
    splash_tick();

    // Legacy round-display input handling.
    {
#if BOARD_HAS_SIDE_BUTTONS
        static bool back_was = false, fwd_was = false;
        bool back_now = (digitalRead(BTN_BACK) == LOW);
        bool fwd_now  = (digitalRead(BTN_FWD)  == LOW);

        if (back_now != back_was) {
            if (back_now) ble_keyboard_press(0x2C, 0);  // HID Space, no mods
            else          ble_keyboard_release();
            back_was = back_now;
        }
        if (fwd_now != fwd_was) {
            if (fwd_now) ble_keyboard_press(0x2B, 0x02);  // HID Tab + LEFT_SHIFT
            else         ble_keyboard_release();
            fwd_was = fwd_now;
        }
#endif

        if (power_pwr_pressed()) {
            if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
            else                                          ui_cycle_screen();
        }
    }

    handle_rotation_change();

    // Update BLE status on screen when state changes
    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    // Update battery indicator
    static int last_pct = -2;
    static bool last_charging = false;
    int pct = power_battery_pct();
    bool charging = power_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    // Check for serial commands (screenshot, etc.)
    check_serial_cmd();

    // Process incoming BLE data
    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }
#endif

    delay(5);
}
