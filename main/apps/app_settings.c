/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 阅读版设置页，分两页：第一页阅读偏好与睡眠，第二页关于与阅读统计。
 * 阅读：默认字号、行距、首行缩进、左右页边距、晃动翻页；
 * 睡眠：锁屏模式、拿起唤醒；关于：电池、构建时间、存储余量；
 * 统计：累计时长、打开次数、本次开机时长、平均每次。长按「关于」标题进入出厂自检。
 *
 * Reading-build settings in two leaves: reading prefs and sleep first, about and
 * reading stats second. Reading: default size, line height, first-line indent,
 * side margin and shake turn; sleep: lock mode and pickup wake; about: battery,
 * build time and free storage; stats: total, opens, this boot and average.
 * A long press on the About heading opens the factory self-test.
 *
 * 冻结：自检不在菜单里，只由长按「关于」标题进入；字号 36..72、步长 4；
 * 拿起唤醒默认关；行距 110..220 步长 10；缩进 0..4 字；页边距 0..3 档每档 20px。
 * Frozen: the self-test stays out of the menu and opens by holding the About
 * heading; size is 36..72 in steps of 4; pickup wake defaults off; line height is
 * 110..220 in steps of 10; indent is 0..4 characters; margin is 0..3 steps of 20 px.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "app_registry.h"
#include "book_layout.h"
#include "book_stats.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_menu.h"

extern const app_desc_t app_settings;

#define TAG "app_settings"

#define SETTINGS_TITLE "设置 Settings"
#define SETTINGS_POLL_MS 1500
#define SETTINGS_PX_MIN 36
#define SETTINGS_PX_MAX 72
#define SETTINGS_PX_STEP 4
#define SETTINGS_LINE_STEP 10
#define SETTINGS_INDENT_MAX 4
#define SETTINGS_MARGIN_MAX 3
#define SETTINGS_MARGIN_STEP_PX 20
#define SETTINGS_CAP_H 34
#define SETTINGS_HOLD_MS 1500
#define SETTINGS_LEAVES 2

#define SETTINGS_HIT_NONE (-1)
#define SETTINGS_HIT_SIZE_MINUS 0
#define SETTINGS_HIT_SIZE_PLUS 1
#define SETTINGS_HIT_SHAKE 2
#define SETTINGS_HIT_LINE_MINUS 3
#define SETTINGS_HIT_LINE_PLUS 4
#define SETTINGS_HIT_INDENT 5
#define SETTINGS_HIT_MARGIN 6
#define SETTINGS_HIT_MODE_LIGHT 10
#define SETTINGS_HIT_MODE_DEEP 11
#define SETTINGS_HIT_MODE_OFF 12
#define SETTINGS_HIT_PICKUP 13
#define SETTINGS_HIT_PAGE_PREV 20
#define SETTINGS_HIT_PAGE_NEXT 21

typedef struct {
    int read_y;
    int size_cap_y;
    int size_row_y;
    int line_cap_y;
    int line_row_y;
    int indent_y;
    int shake_y;
    int sleep_y;
    int mode_y;
    int pickup_y;
    int about_y;
    int about_rows_y;
    int stats_y;
    int stats_rows_y;
    EpdRect size_minus;
    EpdRect size_value;
    EpdRect size_plus;
    EpdRect line_minus;
    EpdRect line_value;
    EpdRect line_plus;
    EpdRect indent_btn;
    EpdRect margin_btn;
    EpdRect page_prev;
    EpdRect page_next;
    EpdRect page_label;
} settings_geom_t;

typedef struct {
    bool pmu_ok;
    bool pickup_ok;
} settings_ui_t;

static settings_ui_t s_ui;
static int s_leaf;
static int64_t s_last_poll_ms;
static int64_t s_hold_t0_ms;
static bool s_hold_fired;

static const char* const k_mode_btn[] = {
    "浅睡 Lightsleep", "深睡 Deepsleep", "关机 Off",
};
static const int k_mode_ids[] = {
    SETTINGS_HIT_MODE_LIGHT, SETTINGS_HIT_MODE_DEEP, SETTINGS_HIT_MODE_OFF,
};

static int clamp_px(int px) {
    if (px < SETTINGS_PX_MIN) return SETTINGS_PX_MIN;
    if (px > SETTINGS_PX_MAX) return SETTINGS_PX_MAX;
    return px;
}

// 两页共用同一套自上而下的顺排：每段高度由各自行数推出，避免写死坐标。
// Both leaves stack top-down with the same rule: each block's height follows its row count.
static settings_geom_t settings_geom(int leaf) {
    int y = UI_CONTENT_TOP;
    settings_geom_t g = { 0 };
    g.page_prev = ui_bar_rect(0, 3);
    g.page_label = ui_bar_rect(1, 3);
    g.page_next = ui_bar_rect(2, 3);

    if (leaf != 1) {
        g.read_y = y;
        y += UI_SEC_HEAD;
        g.size_cap_y = y;
        y += SETTINGS_CAP_H;
        g.size_row_y = y;
        g.size_minus = ui_row_rect(0, 3, y, UI_BTN_H);
        g.size_value = ui_row_rect(1, 3, y, UI_BTN_H);
        g.size_plus = ui_row_rect(2, 3, y, UI_BTN_H);
        y += UI_BTN_H + UI_GAP;
        g.line_cap_y = y;
        y += SETTINGS_CAP_H;
        g.line_row_y = y;
        g.line_minus = ui_row_rect(0, 3, y, UI_BTN_H);
        g.line_value = ui_row_rect(1, 3, y, UI_BTN_H);
        g.line_plus = ui_row_rect(2, 3, y, UI_BTN_H);
        y += UI_BTN_H + UI_GAP;
        g.indent_y = y;
        g.indent_btn = ui_row_rect(0, 2, y, UI_BTN_H);
        g.margin_btn = ui_row_rect(1, 2, y, UI_BTN_H);
        y += UI_BTN_H + UI_GAP;
        g.shake_y = y;
        y += UI_BTN_H + UI_SECTION_GAP;

        g.sleep_y = y;
        y += UI_SEC_HEAD;
        g.mode_y = y;
        y += UI_BTN_H + UI_GAP;
        g.pickup_y = y;
        return g;
    }

    g.about_y = y;
    g.about_rows_y = y + UI_SEC_HEAD;
    y = g.about_rows_y + 3 * UI_ROW_H + UI_SECTION_GAP;
    g.stats_y = y;
    g.stats_rows_y = y + UI_SEC_HEAD;
    return g;
}

static void format_duration(uint32_t seconds, char* out, size_t cap) {
    if (seconds < 60) snprintf(out, cap, "%u 秒", (unsigned)seconds);
    else if (seconds < 3600) snprintf(out, cap, "%u 分", (unsigned)(seconds / 60));
    else snprintf(out, cap, "%u 小时 %u 分", (unsigned)(seconds / 3600), (unsigned)(seconds % 3600 / 60));
}

static const char* charge_text(const pmu_snapshot_t* s) {
    if (!s->status_ok) return "未读到 No data";
    switch (s->charge_state) {
        case PMU_CHARGE_NOT_CHARGING: return "未充电 Idle";
        case PMU_CHARGE_CHARGING: return "充电中 Charging";
        case PMU_CHARGE_FULL_INFERRED: return "已满 Full";
        case PMU_CHARGE_FAULT: return "故障 Fault";
        default: return "未知 Unknown";
    }
}

static void fmt_battery(char* buf, size_t n, const pmu_snapshot_t* s) {
    if (!s->status_ok) {
        snprintf(buf, n, "未读到 No data");
        return;
    }
    snprintf(
        buf, n, "%u.%03u V　%u%%　%s",
        s->battery_mv / 1000, s->battery_mv % 1000, s->soc_permille / 10,
        charge_text(s)
    );
}

static void fmt_storage(char* buf, size_t n) {
    read_pico_sd_info_t info = { 0 };
    esp_err_t err = read_pico_sd_get_info(&info);
    if (err == ESP_ERR_NOT_FINISHED) {
        snprintf(buf, n, "检查中 Checking");
        return;
    }
    if (err != ESP_OK || !info.mounted) {
        snprintf(buf, n, "未挂载 Unmounted");
        return;
    }
    snprintf(
        buf, n, "%llu / %llu MB",
        (unsigned long long)(info.free_bytes / (1024 * 1024)),
        (unsigned long long)(info.capacity_bytes / (1024 * 1024))
    );
}

static void settings_poll(app_ctx_t* ctx) {
    const pmu_snapshot_t* s = read_pico_pmu_get();
    s_ui.pmu_ok = s->present && s->status_ok;
    s_ui.pickup_ok = ctx->sensor_ready && ctx->acc != NULL;
}

// 加减号加中间读数：字号与行距共用这一套控件，只有单位和步长不同。
// Minus, value and plus: size and line height share this control, differing only in unit and step.
static void draw_stepper(
    uint8_t* framebuffer, EpdRect minus, EpdRect value, EpdRect plus, const char* text
) {
    ui_draw_button(framebuffer, minus, "-", false);
    ui_draw_button(framebuffer, plus, "+", false);
    ui_text_vc(
        framebuffer, value.x + value.width / 2, value.y + value.height / 2, UI_PX_LABEL,
        text, EPD_DRAW_ALIGN_CENTER, false
    );
}

static void draw_reading_leaf(uint8_t* framebuffer, const settings_geom_t* g) {
    char value[24];
    const bool pickup = s_ui.pickup_ok && app_settings_pickup_wake();
    const app_sleep_mode_t mode = app_settings_sleep_mode();

    ui_draw_section(framebuffer, g->read_y, "阅读 Reading");
    ui_text(framebuffer, UI_MARGIN, g->size_cap_y, UI_PX_CAPTION,
            "默认字号 Default size", EPD_DRAW_ALIGN_LEFT, false);
    snprintf(value, sizeof(value), "%d px", clamp_px(app_settings_book_px()));
    draw_stepper(framebuffer, g->size_minus, g->size_value, g->size_plus, value);

    ui_text(framebuffer, UI_MARGIN, g->line_cap_y, UI_PX_CAPTION,
            "行距 Line height", EPD_DRAW_ALIGN_LEFT, false);
    snprintf(value, sizeof(value), "%u%%", (unsigned)app_settings_book_line_pct());
    draw_stepper(framebuffer, g->line_minus, g->line_value, g->line_plus, value);

    const unsigned indent = app_settings_book_indent();
    const unsigned margin = app_settings_book_margin();
    snprintf(value, sizeof(value), "缩进 %u 字", indent);
    ui_draw_button(framebuffer, g->indent_btn, value, indent != 0);
    snprintf(value, sizeof(value), "边距 %u px", margin * SETTINGS_MARGIN_STEP_PX);
    ui_draw_button(framebuffer, g->margin_btn, value, margin != 0);

    ui_draw_button(
        framebuffer, (EpdRect){ UI_MARGIN, g->shake_y, ui_content_width(), UI_BTN_H },
        app_settings_book_shake() ? "晃动翻页 Shake：开 On"
                                  : "晃动翻页 Shake：关 Off",
        app_settings_book_shake()
    );

    ui_draw_section(framebuffer, g->sleep_y, "睡眠与唤醒 Sleep & Wake");
    bool mode_on[] = {
        mode == APP_SLEEP_LIGHT, mode == APP_SLEEP_DEEP, mode == APP_SLEEP_OFF,
    };
    for (int i = 0; i < 3; i++) {
        EpdRect r = ui_grid_rect(i, 3, 0, g->mode_y, UI_BTN_H);
        ui_draw_choice_round_rect(framebuffer, r, UI_BTN_RADIUS, mode_on[i]);
        ui_text_vc(
            framebuffer, r.x + r.width / 2, r.y + r.height / 2, UI_PX_LABEL_SM,
            k_mode_btn[i], EPD_DRAW_ALIGN_CENTER, false
        );
    }
    ui_draw_button(
        framebuffer, (EpdRect){ UI_MARGIN, g->pickup_y, ui_content_width(), UI_BTN_H },
        pickup ? "拿起唤醒 Pickup：开 On" : "拿起唤醒 Pickup：关 Off", pickup
    );
}

static void draw_about_leaf(uint8_t* framebuffer, const settings_geom_t* g) {
    char line[64];
    ui_draw_section(framebuffer, g->about_y, "关于 About");
    int y = g->about_rows_y;
    const pmu_snapshot_t* pmu = read_pico_pmu_get();
    const esp_app_desc_t* desc = esp_app_get_description();
    fmt_battery(line, sizeof(line), pmu);
    y = ui_draw_row(framebuffer, y, "电池 Battery", line);
    snprintf(line, sizeof(line), "%s %s", desc->date, desc->time);
    y = ui_draw_row(framebuffer, y, "构建 UTC Build", line);
    fmt_storage(line, sizeof(line));
    ui_draw_row(framebuffer, y, "存储 Storage", line);

    ui_draw_section(framebuffer, g->stats_y, "阅读统计 Statistics");
    book_stats_t stats = { 0 };
    book_stats_global(&stats);
    char total[48], boot[48], average[48];
    format_duration(stats.total_s, total, sizeof(total));
    format_duration(stats.boot_s, boot, sizeof(boot));
    if (stats.opens) format_duration(stats.total_s / stats.opens, average, sizeof(average));
    else snprintf(average, sizeof(average), "—");
    y = g->stats_rows_y;
    y = ui_draw_row(framebuffer, y, "累计阅读 Total", total);
    snprintf(line, sizeof(line), "%u 次", (unsigned)stats.opens);
    y = ui_draw_row(framebuffer, y, "打开图书 Opens", line);
    y = ui_draw_row(framebuffer, y, "本次开机 This boot", boot);
    ui_draw_row(framebuffer, y, "平均每次 Average", average);
}

static void draw_settings_page(uint8_t* framebuffer, int leaf) {
    const settings_geom_t g = settings_geom(leaf);
    ui_clear_page(framebuffer);
    ui_draw_header(
        framebuffer, SETTINGS_TITLE,
        leaf == 1 ? "关于 · 阅读统计 About · Statistics"
                  : "阅读偏好 · 睡眠 Reading · Sleep"
    );
    if (leaf == 1) draw_about_leaf(framebuffer, &g);
    else draw_reading_leaf(framebuffer, &g);

    ui_draw_button(framebuffer, g.page_prev, "上一页", false);
    char page[24];
    snprintf(page, sizeof(page), "第 %d/%d 页", leaf + 1, SETTINGS_LEAVES);
    ui_text_vc(framebuffer, g.page_label.x + g.page_label.width / 2,
               g.page_label.y + g.page_label.height / 2, UI_PX_CAPTION, page,
               EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_button(framebuffer, g.page_next, "下一页", false);
    ui_draw_menu_handle(framebuffer, false);
}

static int settings_hit_test(const settings_geom_t* g, int leaf, uint16_t x, uint16_t y) {
    if (ui_rect_hit(g->page_prev, x, y)) return SETTINGS_HIT_PAGE_PREV;
    if (ui_rect_hit(g->page_next, x, y)) return SETTINGS_HIT_PAGE_NEXT;
    if (leaf == 1) return SETTINGS_HIT_NONE;
    if (ui_rect_hit(g->size_minus, x, y)) return SETTINGS_HIT_SIZE_MINUS;
    if (ui_rect_hit(g->size_plus, x, y)) return SETTINGS_HIT_SIZE_PLUS;
    if (ui_rect_hit(g->line_minus, x, y)) return SETTINGS_HIT_LINE_MINUS;
    if (ui_rect_hit(g->line_plus, x, y)) return SETTINGS_HIT_LINE_PLUS;
    if (ui_rect_hit(g->indent_btn, x, y)) return SETTINGS_HIT_INDENT;
    if (ui_rect_hit(g->margin_btn, x, y)) return SETTINGS_HIT_MARGIN;
    if (ui_rect_hit(
            (EpdRect){ UI_MARGIN, g->shake_y, ui_content_width(), UI_BTN_H }, x, y
        )) {
        return SETTINGS_HIT_SHAKE;
    }
    int hit = ui_grid_hit(x, y, 3, 1, g->mode_y, UI_BTN_H, k_mode_ids);
    if (hit >= 0) return hit;
    if (ui_rect_hit(
            (EpdRect){ UI_MARGIN, g->pickup_y, ui_content_width(), UI_BTN_H }, x, y
        )) {
        return SETTINGS_HIT_PICKUP;
    }
    return SETTINGS_HIT_NONE;
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    if (!ttf_font_ready()) {
        read_pico_sd_info_t sd = { 0 };
        read_pico_sd_get_info(&sd);
        ui_draw_no_font_page(fb, sd.present, false);
        return;
    }
    draw_settings_page(fb, s_leaf);
}

static void on_enter(app_ctx_t* ctx) {
    read_pico_pmu_refresh();
    settings_poll(ctx);
    s_leaf = 0;
    s_last_poll_ms = ctx->now_ms;
    s_hold_t0_ms = 0;
    s_hold_fired = false;
}

// 长按「关于」标题进入出厂自检；松手或滑出即取消，只在第二页生效。
// Hold the About heading to open the factory self-test; release or slide off cancels. Second leaf only.
static app_redraw_t settings_hold_check(app_ctx_t* ctx) {
    if (s_leaf != 1) {
        s_hold_t0_ms = 0;
        s_hold_fired = false;
        return APP_REDRAW_NONE;
    }
    const settings_geom_t g = settings_geom(s_leaf);
    const bool on_about = ctx->touch != NULL && ctx->touch->touched
        && ctx->touch->y >= g.about_y && ctx->touch->y < g.about_rows_y;
    if (!on_about) {
        s_hold_t0_ms = 0;
        s_hold_fired = false;
        return APP_REDRAW_NONE;
    }
    if (s_hold_t0_ms == 0) {
        s_hold_t0_ms = ctx->now_ms;
        return APP_REDRAW_NONE;
    }
    if (!s_hold_fired && ctx->now_ms - s_hold_t0_ms >= SETTINGS_HOLD_MS) {
        s_hold_fired = true;
        ctx->request_app = app_selftest_page();
        ESP_LOGI(TAG, "open device self-test");
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t settings_apply(int hit) {
    switch (hit) {
        case SETTINGS_HIT_PAGE_PREV:
            s_leaf = (s_leaf + SETTINGS_LEAVES - 1) % SETTINGS_LEAVES;
            return APP_REDRAW_PAGE;
        case SETTINGS_HIT_PAGE_NEXT:
            s_leaf = (s_leaf + 1) % SETTINGS_LEAVES;
            return APP_REDRAW_PAGE;
        case SETTINGS_HIT_SIZE_MINUS:
            app_settings_set_book_px((uint8_t)clamp_px(app_settings_book_px() - SETTINGS_PX_STEP));
            return APP_REDRAW_PAGE;
        case SETTINGS_HIT_SIZE_PLUS:
            app_settings_set_book_px((uint8_t)clamp_px(app_settings_book_px() + SETTINGS_PX_STEP));
            return APP_REDRAW_PAGE;
        case SETTINGS_HIT_LINE_MINUS:
            if (app_settings_book_line_pct() <= BOOK_LAYOUT_LINE_PCT_MIN) return APP_REDRAW_NONE;
            app_settings_set_book_line_pct((uint8_t)(app_settings_book_line_pct() - SETTINGS_LINE_STEP));
            return APP_REDRAW_PAGE;
        case SETTINGS_HIT_LINE_PLUS:
            if (app_settings_book_line_pct() >= BOOK_LAYOUT_LINE_PCT_MAX) return APP_REDRAW_NONE;
            app_settings_set_book_line_pct((uint8_t)(app_settings_book_line_pct() + SETTINGS_LINE_STEP));
            return APP_REDRAW_PAGE;
        case SETTINGS_HIT_INDENT:
            app_settings_set_book_indent((uint8_t)((app_settings_book_indent() + 1) % (SETTINGS_INDENT_MAX + 1)));
            return APP_REDRAW_PAGE;
        case SETTINGS_HIT_MARGIN:
            app_settings_set_book_margin((uint8_t)((app_settings_book_margin() + 1) % (SETTINGS_MARGIN_MAX + 1)));
            return APP_REDRAW_PAGE;
        case SETTINGS_HIT_SHAKE:
            app_settings_set_book_shake(!app_settings_book_shake());
            ESP_LOGI(TAG, "shake turn %s", app_settings_book_shake() ? "on" : "off");
            return APP_REDRAW_PAGE;
        case SETTINGS_HIT_PICKUP:
            if (!s_ui.pickup_ok) return APP_REDRAW_NONE;
            app_settings_set_pickup_wake(!app_settings_pickup_wake());
            return APP_REDRAW_PAGE;
        default:
            break;
    }
    if (hit >= SETTINGS_HIT_MODE_LIGHT && hit <= SETTINGS_HIT_MODE_OFF) {
        const app_sleep_mode_t mode = (app_sleep_mode_t)(hit - SETTINGS_HIT_MODE_LIGHT);
        app_settings_set_sleep_mode(mode);
        ESP_LOGI(TAG, "lock mode %s", app_sleep_mode_name(mode));
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    if (!ttf_font_ready()) {
        switch (ui_no_font_hit_test(touch->x, touch->y)) {
            case UI_NO_FONT_HIT_REMOUNT:
                ttf_font_unload();
                ttf_font_open_builtin();
                read_pico_sd_remount();
                ttf_font_init();
                return APP_REDRAW_FULL;
            default:
                return APP_REDRAW_NONE;
        }
    }
    const settings_geom_t g = settings_geom(s_leaf);
    return settings_apply(settings_hit_test(&g, s_leaf, touch->x, touch->y));
}

// 非接管页只收 KEY1；这里用它顺序翻页，和底栏按钮等价。
// Non-owning pages only receive KEY1; cycle the leaves here, matching the bar buttons.
static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    (void)ctx;
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    s_leaf = (s_leaf + 1) % SETTINGS_LEAVES;
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    app_redraw_t redraw = settings_hold_check(ctx);
    if (redraw != APP_REDRAW_NONE) return redraw;
    if (ctx->now_ms - s_last_poll_ms < SETTINGS_POLL_MS) return APP_REDRAW_NONE;
    s_last_poll_ms = ctx->now_ms;
    if (read_pico_pmu_poll() != ESP_OK) return APP_REDRAW_NONE;
    settings_ui_t prev = s_ui;
    settings_poll(ctx);
    if (prev.pmu_ok == s_ui.pmu_ok && prev.pickup_ok == s_ui.pickup_ok) {
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_PAGE;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    return ui_content_refresh_area();
}

const app_desc_t app_settings = {
    .title = SETTINGS_TITLE,
    .detail = "阅读偏好 · 睡眠 · 关于与统计 Reading · Sleep · About",
    .enter_full = true,
    .render = render,
    .on_enter = on_enter,
    .on_touch = on_touch,
    .on_key = on_key,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
