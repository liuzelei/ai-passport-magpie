#include "magpie_dashboard.h"

#include <inttypes.h>
#include <stdio.h>

LV_FONT_DECLARE(magpie_cjk_14);
LV_FONT_DECLARE(magpie_cjk_22);

enum {
    BACKGROUND = 0x0B111D,
    PANEL = 0x151D2D,
    LINE = 0x2C384E,
    TEXT = 0xECF0F8,
    SECONDARY = 0xA9B6CA,
    MUTED = 0x8799B5,
    AMBER = 0xFFC38B,
    CORAL = 0xFFAC87,
    MINT = 0x75E4BF,
    BATTERY_BAR_WIDTH = 14,
    CONTENT_WIDTH = MAGPIE_DASHBOARD_WIDTH - 32,
    TOTAL_X = 13,
    TOTAL_WIDTH = CONTENT_WIDTH - TOTAL_X * 2,
    TOTAL_BASELINE = 62,
    DETAIL_VALUE_WIDTH = 150,
};

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

static lv_obj_t *group(lv_obj_t *screen)
{
    lv_obj_t *object = block(screen, 0, 0, MAGPIE_DASHBOARD_WIDTH, MAGPIE_DASHBOARD_HEIGHT, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    return object;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int width, const char *text,
                       const lv_font_t *font, uint32_t color, lv_text_align_t alignment)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, alignment, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_CLIP);
    lv_label_set_text(object, text);
    return object;
}

static void panel_border(lv_obj_t *object)
{
    lv_obj_set_style_border_color(object, lv_color_hex(LINE), 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_border_opa(object, LV_OPA_COVER, 0);
}

static void show(lv_obj_t *object, bool visible)
{
    if (visible) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

void magpie_dashboard_create(magpie_dashboard_t *dashboard, lv_obj_t *screen)
{
    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    label(screen, 17, 19, 144, "Token 用量", &magpie_cjk_22, TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *battery = block(screen, 162, 28, 18, 10, 2, BACKGROUND);
    panel_border(battery);
    lv_obj_set_style_border_color(battery, lv_color_hex(MUTED), 0);
    block(screen, 181, 31, 2, 4, 1, MUTED);
    dashboard->battery_level = block(battery, 1, 1, BATTERY_BAR_WIDTH, 6, 0, MINT);
    dashboard->battery_text = label(screen, 185, 25, 38, "", &lv_font_montserrat_12,
                                    SECONDARY, LV_TEXT_ALIGN_RIGHT);

    dashboard->usage_group = group(screen);
    lv_obj_t *usage = dashboard->usage_group;
    lv_obj_t *tabs = block(usage, 16, 58, CONTENT_WIDTH, 31, 8, PANEL);
    static const char *const periods[MAGPIE_RANGE_COUNT] = {"今日", "近7日", "近30日"};
    for (unsigned i = 0; i < MAGPIE_RANGE_COUNT; ++i) {
        lv_obj_t *tab = label(tabs, 3 + (int)i * 68, 3, 66, periods[i],
                              &magpie_cjk_14, SECONDARY, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_style_pad_top(tab, 4, 0);
        lv_obj_set_height(tab, 25);
        lv_obj_set_style_radius(tab, 6, 0);
        lv_obj_set_style_bg_color(tab, lv_color_hex(AMBER), 0);
        dashboard->tabs[i] = tab;
    }

    lv_obj_t *primary = block(usage, 16, 98, CONTENT_WIDTH, 101, 10, PANEL);
    panel_border(primary);
    dashboard->total = label(primary, TOTAL_X, 10, TOTAL_WIDTH, "", &lv_font_montserrat_48,
                             AMBER, LV_TEXT_ALIGN_LEFT);
    dashboard->unit = label(primary, TOTAL_X, 32, 49, "", &magpie_cjk_22,
                            CORAL, LV_TEXT_ALIGN_LEFT);
    dashboard->exact = label(primary, TOTAL_X, 73, TOTAL_WIDTH, "", &lv_font_montserrat_14,
                             SECONDARY, LV_TEXT_ALIGN_LEFT);

    label(usage, 17, 210, CONTENT_WIDTH, "今日明细", &magpie_cjk_14, MUTED, LV_TEXT_ALIGN_LEFT);
    static const char *const detail_labels[] = {"输入", "输出", "缓存"};
    for (unsigned i = 0; i < 3; ++i) {
        int y = 234 + (int)i * 19;
        label(usage, 17, y, 52, detail_labels[i], &magpie_cjk_14, SECONDARY, LV_TEXT_ALIGN_LEFT);
        dashboard->details[i] = label(usage, 73, y, DETAIL_VALUE_WIDTH, "", &lv_font_montserrat_14,
                                      TEXT, LV_TEXT_ALIGN_RIGHT);
    }

    block(usage, 17, 292, CONTENT_WIDTH, 1, 0, LINE);
    dashboard->status_dot = block(usage, 18, 302, 6, 6, 3, MUTED);
    dashboard->status_text = label(usage, 30, 296, 99, "", &magpie_cjk_14, SECONDARY, LV_TEXT_ALIGN_LEFT);
    dashboard->sync = label(usage, 94, 296, 129, "", &magpie_cjk_14, MUTED, LV_TEXT_ALIGN_RIGHT);

    dashboard->pairing_group = group(screen);
    lv_obj_t *pair = block(dashboard->pairing_group, 16, 76, CONTENT_WIDTH, 171, 10, PANEL);
    panel_border(pair);
    label(pair, 8, 18, CONTENT_WIDTH - 16, "蓝牙配对", &magpie_cjk_22, TEXT, LV_TEXT_ALIGN_CENTER);
    dashboard->passkey = label(pair, 8, 61, CONTENT_WIDTH - 16, "", &lv_font_montserrat_36,
                               AMBER, LV_TEXT_ALIGN_CENTER);
    dashboard->pairing_hint = label(pair, 8, 123, CONTENT_WIDTH - 16, "", &magpie_cjk_14,
                                    SECONDARY, LV_TEXT_ALIGN_CENTER);
    show(dashboard->pairing_group, false);
}

/* Numeric labels use ASCII digits, separators and placeholder dashes. */
static int number_width(const char *text, const lv_font_t *font)
{
    int width = 0;
    for (const char *digit = text; *digit; ++digit) {
        width += lv_font_get_glyph_width(font, (uint8_t)digit[0], (uint8_t)digit[1]);
    }
    return width;
}

static const lv_font_t *fit_number(const char *text, int width, const lv_font_t *const *fonts,
                                   size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (number_width(text, fonts[i]) <= width) {
            return fonts[i];
        }
    }
    return fonts[count - 1];
}

static const char *format_primary(uint64_t value, char number[MAGPIE_TOKEN_TEXT_CAPACITY])
{
    static const struct {
        uint64_t scale;
        const char *label;
    } units[] = {
        {UINT64_C(10000), "万"}, {UINT64_C(100000000), "亿"},
        {UINT64_C(1000000000000), "万亿"}, {UINT64_C(10000000000000000), "京"},
    };
    if (value < units[0].scale) {
        magpie_format_tokens(value, number);
        return "";
    }

    /* Round a decimal digit without multiplying the full value. Promote at the rounding boundary. */
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); ++i) {
        uint64_t step = units[i].scale / 10;
        uint64_t tenths = value / step + (value % step >= step / 2);
        if (i + 1 == sizeof(units) / sizeof(units[0]) || tenths < units[i + 1].scale / step) {
            snprintf(number, MAGPIE_TOKEN_TEXT_CAPACITY, "%" PRIu64 ".%" PRIu64, tenths / 10, tenths % 10);
            return units[i].label;
        }
    }
    return "";
}

static void align_baseline(lv_obj_t *label_object, const lv_font_t *font)
{
    lv_obj_set_style_text_font(label_object, font, 0);
    lv_obj_set_y(label_object, TOTAL_BASELINE - (font->line_height - font->base_line));
}

static void render_usage(magpie_dashboard_t *dashboard, const magpie_dashboard_view_t *view)
{
    const magpie_usage_t *usage = &view->link.usage;
    const uint64_t totals[MAGPIE_RANGE_COUNT] = {usage->today_total, usage->last_7_days, usage->last_30_days};
    const uint64_t detail_values[] = {usage->today_input, usage->today_output, usage->today_cache};
    const unsigned range = view->selected_range < MAGPIE_RANGE_COUNT ? view->selected_range : 0;
    for (unsigned i = 0; i < MAGPIE_RANGE_COUNT; ++i) {
        lv_obj_set_style_bg_opa(dashboard->tabs[i], i == range ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(dashboard->tabs[i], lv_color_hex(i == range ? BACKGROUND : SECONDARY), 0);
    }

    char value[MAGPIE_TOKEN_TEXT_CAPACITY] = "--";
    char exact[MAGPIE_TOKEN_TEXT_CAPACITY] = "";
    const char *unit = view->link.has_usage ? format_primary(totals[range], value) : "";
    const bool scaled = *unit != '\0';
    if (scaled) {
        magpie_format_tokens(totals[range], exact);
    }
    const lv_font_t *const primary_fonts[] = {
        &lv_font_montserrat_48, &lv_font_montserrat_36,
        &lv_font_montserrat_28, &lv_font_montserrat_20,
    };
    const lv_font_t *const detail_fonts[] = {
        &lv_font_montserrat_14, &lv_font_montserrat_12,
        &lv_font_montserrat_10, &lv_font_montserrat_8,
    };
    const int unit_space = scaled ? (unit[3] ? 50 : 28) : 0;
    const lv_font_t *primary_font = fit_number(value, TOTAL_WIDTH - unit_space, primary_fonts,
                                               sizeof(primary_fonts) / sizeof(primary_fonts[0]));
    align_baseline(dashboard->total, primary_font);
    lv_label_set_text(dashboard->total, value);
    align_baseline(dashboard->unit, &magpie_cjk_22);
    lv_obj_set_x(dashboard->unit, TOTAL_X + number_width(value, primary_font) + 6);
    lv_label_set_text(dashboard->unit, unit);
    show(dashboard->unit, scaled);
    lv_obj_set_style_text_font(dashboard->exact,
        fit_number(exact, TOTAL_WIDTH, detail_fonts, sizeof(detail_fonts) / sizeof(detail_fonts[0])), 0);
    lv_label_set_text(dashboard->exact, exact);

    for (unsigned i = 0; i < 3; ++i) {
        char text[MAGPIE_TOKEN_TEXT_CAPACITY] = "--";
        if (view->link.has_usage) {
            magpie_format_tokens(detail_values[i], text);
        }
        const lv_font_t *font = fit_number(text, DETAIL_VALUE_WIDTH, detail_fonts,
                                            sizeof(detail_fonts) / sizeof(detail_fonts[0]));
        lv_obj_set_style_text_font(dashboard->details[i], font, 0);
        lv_label_set_text(dashboard->details[i], text);
    }
}

static void render_status(magpie_dashboard_t *dashboard, const magpie_dashboard_view_t *view)
{
    const char *status = "等待连接";
    uint32_t color = MUTED;
    if (!view->bluetooth_available) {
        status = "蓝牙不可用";
        color = CORAL;
    } else if (view->status == MAGPIE_STALE) {
        status = "数据过期";
        color = CORAL;
    } else if (view->status == MAGPIE_PAUSED) {
        status = "同步暂停";
        color = CORAL;
    } else if (view->status == MAGPIE_LIVE && !view->ready) {
        status = "连接断开";
        color = CORAL;
    } else if (view->ready) {
        status = "已连接";
        color = MINT;
    }
    lv_obj_set_style_bg_color(dashboard->status_dot, lv_color_hex(color), 0);
    lv_label_set_text(dashboard->status_text, status);
    if (view->bluetooth_available && !view->has_bond && !view->ready) {
        lv_label_set_text(dashboard->sync, "长按确定键配对");
    } else if (view->link.has_usage) {
        lv_label_set_text_fmt(dashboard->sync, "同步 %s", view->link.usage.synced_at);
    } else {
        lv_label_set_text(dashboard->sync, "同步 --:--");
    }
}

void magpie_dashboard_render(magpie_dashboard_t *dashboard, const magpie_dashboard_view_t *view)
{
    bool battery_known = view->battery >= 0 && view->battery <= 100;
    show(dashboard->battery_level, battery_known && view->battery > 0);
    if (battery_known) {
        lv_label_set_text_fmt(dashboard->battery_text, "%d%%", view->battery);
        lv_obj_set_width(dashboard->battery_level, (view->battery * BATTERY_BAR_WIDTH + 99) / 100);
        lv_obj_set_style_bg_color(dashboard->battery_level,
            lv_color_hex(view->battery <= 20 ? CORAL : MINT), 0);
    } else {
        lv_label_set_text(dashboard->battery_text, "--");
    }

    show(dashboard->usage_group, !view->pairing);
    show(dashboard->pairing_group, view->pairing);
    if (view->pairing) {
        if (view->showing_passkey) {
            lv_label_set_text_fmt(dashboard->passkey, "%06" PRIu32, view->passkey);
            lv_label_set_text(dashboard->pairing_hint, "在 Mac 输入配对码");
        } else {
            lv_label_set_text(dashboard->passkey, "------");
            lv_label_set_text(dashboard->pairing_hint, "启动 Mac 桥接程序");
        }
    } else {
        render_usage(dashboard, view);
        render_status(dashboard, view);
    }
}
