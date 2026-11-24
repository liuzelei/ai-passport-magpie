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
#include "magpie_dashboard.h"
#include "nvs_flash.h"

static const char *const TAG = "magpie";
_Static_assert(BSP_LCD_W == MAGPIE_DASHBOARD_WIDTH && BSP_LCD_H == MAGPIE_DASHBOARD_HEIGHT,
               "Dashboard layout requires the 240 by 320 Passport display");

static magpie_dashboard_t dashboard;

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
                    selected_range = (selected_range + MAGPIE_RANGE_COUNT - 1) % MAGPIE_RANGE_COUNT;
                    break;
                case BSP_BTN_DOWN:
                    selected_range = (selected_range + 1) % MAGPIE_RANGE_COUNT;
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
            const magpie_dashboard_view_t presentation = {
                .link = view.link,
                .status = status,
                .selected_range = selected_range,
                .battery = battery,
                .bluetooth_available = config->bluetooth_available,
                .ready = view.ready,
                .pairing = view.pairing,
                .has_bond = view.has_bond,
                .showing_passkey = view.showing_passkey,
                .passkey = view.passkey,
            };
            magpie_dashboard_render(&dashboard, &presentation);
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
    lv_obj_t *screen = lv_obj_create(NULL);
    magpie_dashboard_create(&dashboard, screen);
    const magpie_dashboard_view_t initial = {.battery = -1, .bluetooth_available = true};
    magpie_dashboard_render(&dashboard, &initial);
    lv_screen_load(screen);
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
