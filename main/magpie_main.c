#include <inttypes.h>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "magpie_ble.h"
#include "nvs_flash.h"

static const char *const TAG = "magpie";
static const uint32_t PAGE_COLOR = 0x09171A;
static const uint32_t CARD_COLOR = 0x132C30;
static const uint32_t TAB_COLOR = 0x25464B;
static const uint32_t INK_COLOR = 0xE9F7EF;
static const uint32_t MUTED_COLOR = 0x91B6B2;
static const uint32_t ACCENT_COLOR = 0x70E7B4;
static const uint32_t WARNING_COLOR = 0xFFD388;

static const char *const ranges[] = {"TODAY", "7 DAYS", "30 DAYS"};
enum {
    RANGE_COUNT = sizeof(ranges) / sizeof(ranges[0]),
    HEADLINE_WIDTH = BSP_LCD_W - 58,
    DETAIL_VALUE_WIDTH = 127,
};

/* All widget operations run under the BSP's LVGL lock. */
typedef struct {
    lv_obj_t *battery;
    lv_obj_t *status;
    lv_obj_t *metric;
    lv_obj_t *last_sync;
    lv_obj_t *headline;
    lv_obj_t *pairing_hint;
    lv_obj_t *footer;
    lv_obj_t *tabs[RANGE_COUNT];
    lv_obj_t *details[3];
} dashboard_t;

static dashboard_t dashboard;

static lv_obj_t *block(lv_obj_t *parent, int x, int y, int width, int height,
                       int radius, uint32_t color)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    return object;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int width, const char *text,
                       const lv_font_t *font, uint32_t color, lv_text_align_t alignment)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, alignment, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_CLIP);
    lv_label_set_text(object, text);
    return object;
}

static void dashboard_create(dashboard_t *view)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(PAGE_COLOR), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    label(screen, 17, 18, 135, "Magpie", &lv_font_montserrat_28, INK_COLOR, LV_TEXT_ALIGN_LEFT);
    view->battery = label(screen, 162, 25, 58, "", &lv_font_montserrat_14,
                          MUTED_COLOR, LV_TEXT_ALIGN_RIGHT);
    label(screen, 18, 56, 118, "TOKEN USAGE", &lv_font_montserrat_14,
          MUTED_COLOR, LV_TEXT_ALIGN_LEFT);
    view->status = label(screen, 129, 56, 91, "", &lv_font_montserrat_14,
                         MUTED_COLOR, LV_TEXT_ALIGN_RIGHT);

    lv_obj_t *card = block(screen, 16, 85, BSP_LCD_W - 32, 112, 14, CARD_COLOR);
    view->metric = label(card, 13, 10, 165, "TOKENS", &lv_font_montserrat_14,
                         MUTED_COLOR, LV_TEXT_ALIGN_LEFT);
    view->last_sync = label(card, 86, 10, 109, "", &lv_font_montserrat_14,
                            MUTED_COLOR, LV_TEXT_ALIGN_RIGHT);
    view->headline = label(card, 13, 30, HEADLINE_WIDTH, "", &lv_font_montserrat_36,
                           INK_COLOR, LV_TEXT_ALIGN_LEFT);
    for (unsigned i = 0; i < RANGE_COUNT; ++i) {
        view->tabs[i] = label(card, 10 + (int)i * 64, 83, 59, ranges[i],
                              &lv_font_montserrat_12, MUTED_COLOR, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_style_pad_top(view->tabs[i], 3, 0);
        lv_obj_set_style_pad_bottom(view->tabs[i], 3, 0);
        lv_obj_set_style_radius(view->tabs[i], 5, 0);
        lv_obj_set_style_bg_color(view->tabs[i], lv_color_hex(TAB_COLOR), 0);
    }

    view->pairing_hint = label(card, 13, 84, HEADLINE_WIDTH, "", &lv_font_montserrat_14,
                               ACCENT_COLOR, LV_TEXT_ALIGN_LEFT);

    block(screen, 18, 206, 204, 1, 0, TAB_COLOR);
    label(screen, 18, 216, 204, "TODAY BREAKDOWN", &lv_font_montserrat_14,
          MUTED_COLOR, LV_TEXT_ALIGN_LEFT);
    const char *const detail_labels[] = {"Input", "Output", "Cache"};
    for (unsigned i = 0; i < 3; ++i) {
        int row_y = 239 + (int)i * 19;
        label(screen, 18, row_y, 78, detail_labels[i], &lv_font_montserrat_12,
              MUTED_COLOR, LV_TEXT_ALIGN_LEFT);
        view->details[i] = label(screen, 95, row_y, DETAIL_VALUE_WIDTH, "", &lv_font_montserrat_14,
                                 INK_COLOR, LV_TEXT_ALIGN_RIGHT);
    }
    view->footer = label(screen, 18, 301, 204, "", &lv_font_montserrat_10,
                         MUTED_COLOR, LV_TEXT_ALIGN_CENTER);
    lv_screen_load(screen);
}

/* Token labels contain only ASCII digits, commas and the placeholder dash. */
static int token_text_width(const char *text, const lv_font_t *font)
{
    int width = 0;
    for (const char *character = text; *character != '\0'; ++character) {
        width += lv_font_get_glyph_width(font, (uint8_t)character[0], (uint8_t)character[1]);
    }
    return width;
}

static void token_label(lv_obj_t *object, bool has_usage, uint64_t value, bool large)
{
    char text[MAGPIE_TOKEN_TEXT_CAPACITY] = "--";
    if (has_usage) {
        magpie_format_tokens(value, text);
    }
    const lv_font_t *const headline_fonts[] = {
        &lv_font_montserrat_36, &lv_font_montserrat_28,
        &lv_font_montserrat_20, &lv_font_montserrat_14, &lv_font_montserrat_10,
    };
    const lv_font_t *font = &lv_font_montserrat_14;
    if (large) {
        for (size_t i = 0; i < sizeof(headline_fonts) / sizeof(headline_fonts[0]); ++i) {
            font = headline_fonts[i];
            if (token_text_width(text, font) <= HEADLINE_WIDTH) {
                break;
            }
        }
    } else if (token_text_width(text, font) > DETAIL_VALUE_WIDTH) {
        font = &lv_font_montserrat_10;
        if (token_text_width(text, font) > DETAIL_VALUE_WIDTH) {
            font = &lv_font_montserrat_8;
        }
    }
    lv_obj_set_style_text_font(object, font, 0);
    lv_label_set_text(object, text);
}

static void dashboard_render(dashboard_t *widgets, const magpie_ble_view_t *view,
                             magpie_status_t status, unsigned selected_range, int battery,
                             bool bluetooth_available)
{
    static const char *const status_labels[] = {
        [MAGPIE_WAITING] = "WAITING",
        [MAGPIE_LIVE] = "BLE LIVE",
        [MAGPIE_PAUSED] = "PAUSED",
        [MAGPIE_STALE] = "STALE",
    };
    const magpie_link_t *link = &view->link;
    const char *status_text = status_labels[status];
    uint32_t status_color = MUTED_COLOR;
    if (!bluetooth_available) {
        status_text = "BLE ERROR";
        status_color = WARNING_COLOR;
    } else if (view->pairing) {
        status_text = "PAIRING";
        status_color = WARNING_COLOR;
    } else if (status == MAGPIE_LIVE && view->ready) {
        status_color = ACCENT_COLOR;
    } else if (status == MAGPIE_LIVE) {
        status_text = "OFFLINE";
        status_color = WARNING_COLOR;
    } else if (status == MAGPIE_PAUSED || status == MAGPIE_STALE) {
        status_color = WARNING_COLOR;
    }
    lv_label_set_text(widgets->status, status_text);
    lv_obj_set_style_text_color(widgets->status, lv_color_hex(status_color), 0);
    if (battery >= 0 && battery <= 100) {
        lv_label_set_text_fmt(widgets->battery, "%d%%", battery);
    } else {
        lv_label_set_text(widgets->battery, "");
    }

    if (view->pairing) {
        lv_label_set_text(widgets->metric, view->showing_passkey ? "PAIRING CODE" : "PAIRING WINDOW");
        lv_label_set_text(widgets->last_sync, "");
        lv_obj_set_style_text_font(widgets->headline, &lv_font_montserrat_36, 0);
        if (view->showing_passkey) {
            lv_label_set_text_fmt(widgets->headline, "%06" PRIu32, view->passkey);
        } else {
            lv_label_set_text(widgets->headline, "--");
        }
        lv_label_set_text(widgets->pairing_hint, view->showing_passkey ? "Enter on your Mac" : "Start the Mac bridge");
        lv_obj_remove_flag(widgets->pairing_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(widgets->metric, "TOKENS");
        if (link->has_usage) {
            lv_label_set_text_fmt(widgets->last_sync, "SYNC %s", link->usage.synced_at);
        } else {
            lv_label_set_text(widgets->last_sync, "SYNC --:--");
        }
        const uint64_t totals[] = {
            link->usage.today_total, link->usage.last_7_days, link->usage.last_30_days,
        };
        token_label(widgets->headline, link->has_usage, totals[selected_range], true);
        lv_obj_add_flag(widgets->pairing_hint, LV_OBJ_FLAG_HIDDEN);
    }

    const uint64_t details[] = {
        link->usage.today_input, link->usage.today_output, link->usage.today_cache,
    };
    for (unsigned i = 0; i < 3; ++i) {
        token_label(widgets->details[i], link->has_usage, details[i], false);
    }
    for (unsigned i = 0; i < RANGE_COUNT; ++i) {
        bool active = i == selected_range;
        lv_obj_set_style_text_color(widgets->tabs[i], lv_color_hex(active ? ACCENT_COLOR : MUTED_COLOR), 0);
        lv_obj_set_style_bg_opa(widgets->tabs[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (view->pairing) {
            lv_obj_add_flag(widgets->tabs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(widgets->tabs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (view->pairing) {
        lv_label_set_text(widgets->footer, "");
    } else {
        lv_label_set_text(widgets->footer, bluetooth_available && !view->has_bond ?
                          "HOLD OK TO PAIR" : "UP / DOWN  CHANGE RANGE");
    }
}

static uint64_t uptime_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

typedef struct {
    bsp_btn_t button;
    bsp_btn_ev_t type;
} button_input_t;

typedef struct {
    QueueHandle_t buttons;
    QueueHandle_t updates;
    bool bluetooth_available;
} display_task_config_t;

static display_task_config_t task_config;

static void on_button(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    if (event == BSP_BTN_CLICK || event == BSP_BTN_LONG || event == BSP_BTN_DOUBLE) {
        const button_input_t input = {.button = button, .type = event};
        xQueueSend((QueueHandle_t)user, &input, 0);
    }
}

static void display_task(void *user)
{
    const display_task_config_t *config = user;
    magpie_ble_view_t view = {0};
    unsigned selected_range = 0;
    magpie_status_t drawn_status = MAGPIE_WAITING;
    int battery = -1;
    uint64_t next_battery_ms = 0;
    uint64_t last_interaction_ms = uptime_ms();
    uint8_t backlight = 45;
    bool dirty = true;

    for (;;) {
        button_input_t input;
        bool button_available = xQueueReceive(config->buttons, &input, pdMS_TO_TICKS(200)) == pdTRUE;
        uint64_t now = uptime_ms();
        if (xQueueReceive(config->updates, &view, 0) == pdTRUE) {
            dirty = true;
        }
        if (button_available) {
            last_interaction_ms = now;
            if (backlight != 0 && input.type == BSP_BTN_LONG && input.button == BSP_BTN_OK &&
                config->bluetooth_available) {
                magpie_ble_request_pairing();
            } else if (backlight != 0 && input.type == BSP_BTN_CLICK) {
                switch (input.button) {
                case BSP_BTN_UP:
                    selected_range = (selected_range + RANGE_COUNT - 1) % RANGE_COUNT;
                    break;
                case BSP_BTN_DOWN:
                    selected_range = (selected_range + 1) % RANGE_COUNT;
                    break;
                case BSP_BTN_OK:
                    selected_range = 0;
                    break;
                }
                dirty = true;
            }
        }

        if (now >= next_battery_ms) {
            int current_battery = bsp_battery_soc();
            dirty |= current_battery != battery;
            battery = current_battery;
            next_battery_ms = now + 30000;
        }

        magpie_status_t status = magpie_status(&view.link, now);
        if ((dirty || status != drawn_status) && bsp_lvgl_lock(25)) {
            dashboard_render(&dashboard, &view, status, selected_range, battery,
                             config->bluetooth_available);
            bsp_lvgl_unlock();
            drawn_status = status;
            dirty = false;
        }

        uint64_t idle_ms = now - last_interaction_ms;
        uint8_t next_brightness = 45;
        if (!view.pairing && idle_ms >= 120000) {
            next_brightness = 0;
        } else if (!view.pairing && idle_ms >= 45000) {
            next_brightness = 15;
        }
        if (next_brightness != backlight) {
            bsp_display_backlight(next_brightness);
            backlight = next_brightness;
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_init());
    if (bsp_lvgl_init() == NULL || !bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "Cannot initialize the dashboard display");
        return;
    }
    dashboard_create(&dashboard);
    const magpie_ble_view_t initial = {0};
    dashboard_render(&dashboard, &initial, MAGPIE_WAITING, 0, -1, true);
    bsp_lvgl_unlock();
    bsp_display_backlight(45);

    task_config.buttons = xQueueCreate(8, sizeof(button_input_t));
    task_config.updates = xQueueCreate(1, sizeof(magpie_ble_view_t));
    if (task_config.buttons == NULL || task_config.updates == NULL) {
        ESP_LOGE(TAG, "Cannot allocate dashboard queues");
        return;
    }
    if (bsp_button_init(on_button, task_config.buttons) != ESP_OK) {
        ESP_LOGW(TAG, "Buttons unavailable");
    }
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "Battery reading unavailable");
    }

    esp_err_t bluetooth_result = nvs_flash_init();
    if (bluetooth_result == ESP_OK) {
        bluetooth_result = magpie_ble_init(task_config.updates);
    }
    task_config.bluetooth_available = bluetooth_result == ESP_OK;
    if (!task_config.bluetooth_available) {
        ESP_LOGE(TAG, "Bluetooth unavailable (%s)", esp_err_to_name(bluetooth_result));
    }
    if (xTaskCreate(display_task, "magpie_display", 4096, &task_config, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the display task");
    }
}
