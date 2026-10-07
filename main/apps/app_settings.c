/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 阅读版设置页。阅读偏好、睡眠与唤醒、设备信息。
 * 阅读：默认字号、晃动翻页；睡眠：锁屏模式、拿起唤醒；
 * 关于：电池、构建时间、存储余量。长按「关于」标题进入出厂自检。
 *
 * Reading-build settings. Reading prefs, sleep and wake, device info.
 * Reading: default size and shake turn; sleep: lock mode and pickup wake;
 * About: battery, build time and free storage. A long press on the About
 * heading opens the factory self-test.
 *
 * 冻结：底栏只留菜单把手，KEY3 走主循环菜单把手；自检不在菜单里，
 * 只由长按「关于」标题进入；字号 36..72、步长 4；拿起唤醒默认关。
 * Frozen: bar is the menu handle only; KEY3 is the loop menu handle; the
 * self-test stays out of the menu and opens by holding the About heading;
 * size is 36..72 in steps of 4; pickup wake defaults off.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "app_registry.h"
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
#define SETTINGS_CAP_H 34
#define SETTINGS_HOLD_MS 1500

#define SETTINGS_HIT_NONE (-1)
#define SETTINGS_HIT_SIZE_MINUS 0
#define SETTINGS_HIT_SIZE_PLUS 1
#define SETTINGS_HIT_SHAKE 2
#define SETTINGS_HIT_MODE_LIGHT 10
#define SETTINGS_HIT_MODE_DEEP 11
#define SETTINGS_HIT_MODE_OFF 12
#define SETTINGS_HIT_PICKUP 13

typedef struct {
    int read_y;
    int size_cap_y;
    int size_row_y;
    int shake_y;
    int sleep_y;
    int mode_y;
    int pickup_y;
    int about_y;
    int about_rows_y;
    EpdRect size_minus;
    EpdRect size_value;
    EpdRect size_plus;
} settings_geom_t;

typedef struct {
    bool pmu_ok;
    bool pickup_ok;
} settings_ui_t;

static settings_ui_t s_ui;
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

// 阅读、睡眠、关于三段自上而下顺排，每段高度由各自的行数推出，避免写死坐标。
// Reading, sleep and about stack top-down; each block height follows its row count instead of fixed coordinates.
static settings_geom_t settings_geom(void) {
    int y = UI_CONTENT_TOP;
    settings_geom_t g = { 0 };

    g.read_y = y;
    y += UI_SEC_HEAD;
    g.size_cap_y = y;
    y += SETTINGS_CAP_H;
    g.size_row_y = y;
    g.size_minus = ui_row_rect(0, 3, y, UI_BTN_H);
    g.size_value = ui_row_rect(1, 3, y, UI_BTN_H);
    g.size_plus = ui_row_rect(2, 3, y, UI_BTN_H);
    y += UI_BTN_H + UI_GAP;
    g.shake_y = y;
    y += UI_BTN_H + UI_SECTION_GAP;

    g.sleep_y = y;
    y += UI_SEC_HEAD;
    g.mode_y = y;
    y += UI_BTN_H + UI_GAP;
    g.pickup_y = y;
    y += UI_BTN_H + UI_SECTION_GAP;

    g.about_y = y;
    g.about_rows_y = y + UI_SEC_HEAD;
    return g;
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

static void draw_size_row(uint8_t* framebuffer, const settings_geom_t* g) {
    char value[24];
    const int px = clamp_px(app_settings_book_px());
    snprintf(value, sizeof(value), "%d px", px);
    ui_draw_button(framebuffer, g->size_minus, "-", false);
    ui_draw_button(framebuffer, g->size_plus, "+", false);
    ui_text_vc(
        framebuffer, g->size_value.x + g->size_value.width / 2,
        g->size_value.y + g->size_value.height / 2, UI_PX_LABEL,
        value, EPD_DRAW_ALIGN_CENTER, false
    );
}

static void draw_settings_page(uint8_t* framebuffer) {
    const settings_geom_t g = settings_geom();
    const app_sleep_mode_t mode = app_settings_sleep_mode();
    const bool pickup = s_ui.pickup_ok && app_settings_pickup_wake();
    char line[64];

    ui_clear_page(framebuffer);
    ui_draw_header(framebuffer, SETTINGS_TITLE, app_settings.detail);

    ui_draw_section(framebuffer, g.read_y, "阅读 Reading");
    ui_text(
        framebuffer, UI_MARGIN, g.size_cap_y, UI_PX_CAPTION,
        "默认字号 Default size", EPD_DRAW_ALIGN_LEFT, false
    );
    draw_size_row(framebuffer, &g);
    ui_draw_button(
        framebuffer, (EpdRect){ UI_MARGIN, g.shake_y, ui_content_width(), UI_BTN_H },
        app_settings_book_shake() ? "晃动翻页 Shake：开 On"
                                  : "晃动翻页 Shake：关 Off",
        app_settings_book_shake()
    );

    ui_draw_section(framebuffer, g.sleep_y, "睡眠与唤醒 Sleep & Wake");
    bool mode_on[] = {
        mode == APP_SLEEP_LIGHT, mode == APP_SLEEP_DEEP, mode == APP_SLEEP_OFF,
    };
    for (int i = 0; i < 3; i++) {
        EpdRect r = ui_grid_rect(i, 3, 0, g.mode_y, UI_BTN_H);
        ui_draw_choice_round_rect(framebuffer, r, UI_BTN_RADIUS, mode_on[i]);
        ui_text_vc(
            framebuffer, r.x + r.width / 2, r.y + r.height / 2, UI_PX_LABEL_SM,
            k_mode_btn[i], EPD_DRAW_ALIGN_CENTER, false
        );
    }
    ui_draw_button(
        framebuffer, (EpdRect){ UI_MARGIN, g.pickup_y, ui_content_width(), UI_BTN_H },
        pickup ? "拿起唤醒 Pickup：开 On" : "拿起唤醒 Pickup：关 Off", pickup
    );

    ui_draw_section(framebuffer, g.about_y, "关于 About");
    int y = g.about_rows_y;
    const pmu_snapshot_t* pmu = read_pico_pmu_get();
    const esp_app_desc_t* desc = esp_app_get_description();
    fmt_battery(line, sizeof(line), pmu);
    y = ui_draw_row(framebuffer, y, "电池 Battery", line);
    snprintf(line, sizeof(line), "%s %s", desc->date, desc->time);
    y = ui_draw_row(framebuffer, y, "构建 UTC Build", line);
    fmt_storage(line, sizeof(line));
    ui_draw_row(framebuffer, y, "存储 Storage", line);

    ui_draw_menu_handle(framebuffer, false);
}

static int settings_hit_test(const settings_geom_t* g, uint16_t x, uint16_t y) {
    if (ui_rect_hit(g->size_minus, x, y)) return SETTINGS_HIT_SIZE_MINUS;
    if (ui_rect_hit(g->size_plus, x, y)) return SETTINGS_HIT_SIZE_PLUS;
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
    draw_settings_page(fb);
}

static void on_enter(app_ctx_t* ctx) {
    read_pico_pmu_refresh();
    settings_poll(ctx);
    s_last_poll_ms = ctx->now_ms;
    s_hold_t0_ms = 0;
    s_hold_fired = false;
}

// 长按「关于」标题进入出厂自检；松手或滑出即取消。
// Hold the About heading to open the factory self-test; release or slide off cancels.
static app_redraw_t settings_hold_check(app_ctx_t* ctx) {
    const settings_geom_t g = settings_geom();
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
    const settings_geom_t g = settings_geom();
    const int hit = settings_hit_test(&g, touch->x, touch->y);
    if (hit >= SETTINGS_HIT_MODE_LIGHT && hit <= SETTINGS_HIT_MODE_OFF) {
        const app_sleep_mode_t mode = (app_sleep_mode_t)(hit - SETTINGS_HIT_MODE_LIGHT);
        app_settings_set_sleep_mode(mode);
        ESP_LOGI(TAG, "lock mode %s", app_sleep_mode_name(mode));
        return APP_REDRAW_PAGE;
    }
    switch (hit) {
        case SETTINGS_HIT_SIZE_MINUS:
            app_settings_set_book_px(
                (uint8_t)clamp_px(app_settings_book_px() - SETTINGS_PX_STEP)
            );
            return APP_REDRAW_PAGE;
        case SETTINGS_HIT_SIZE_PLUS:
            app_settings_set_book_px(
                (uint8_t)clamp_px(app_settings_book_px() + SETTINGS_PX_STEP)
            );
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
            return APP_REDRAW_NONE;
    }
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
    .detail = "阅读偏好 · 睡眠 · 关于 Reading · Sleep · About",
    .enter_full = true,
    .render = render,
    .on_enter = on_enter,
    .on_touch = on_touch,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
