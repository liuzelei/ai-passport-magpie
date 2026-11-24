#pragma once

#include "lvgl.h"
#include "magpie_protocol.h"

enum {
    MAGPIE_DASHBOARD_WIDTH = 240,
    MAGPIE_DASHBOARD_HEIGHT = 320,
    MAGPIE_RANGE_COUNT = 3,
};

typedef struct {
    magpie_link_t link;
    magpie_status_t status;
    unsigned selected_range;
    int battery;
    bool bluetooth_available;
    bool ready;
    bool pairing;
    bool has_bond;
    bool showing_passkey;
    uint32_t passkey;
} magpie_dashboard_view_t;

typedef struct {
    lv_obj_t *battery_level;
    lv_obj_t *battery_text;
    lv_obj_t *usage_group;
    lv_obj_t *pairing_group;
    lv_obj_t *tabs[MAGPIE_RANGE_COUNT];
    lv_obj_t *total;
    lv_obj_t *unit;
    lv_obj_t *exact;
    lv_obj_t *details[3];
    lv_obj_t *status_dot;
    lv_obj_t *status_text;
    lv_obj_t *sync;
    lv_obj_t *passkey;
    lv_obj_t *pairing_hint;
} magpie_dashboard_t;

/* Invoke in LVGL's context or under its lock. The caller creates and loads the screen. */
void magpie_dashboard_create(magpie_dashboard_t *dashboard, lv_obj_t *screen);
void magpie_dashboard_render(magpie_dashboard_t *dashboard, const magpie_dashboard_view_t *view);
