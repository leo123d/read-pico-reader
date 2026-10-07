/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 全部页面的清单。菜单顺序。加页只改这张表。
 * 本版为纯阅读固件：菜单只保留阅读相关页面，演示与诊断页已下线。
 *
 * Page table. Menu order lives here; adding a page only changes this list.
 * This build is reading-only: the menu keeps reading pages only; demo and
 * diagnostic pages are removed.
 */

#include "app_registry.h"

extern const app_desc_t app_book;
extern const app_desc_t app_transfer;
extern const app_desc_t app_font_pick;
extern const app_desc_t app_storage;
extern const app_desc_t app_settings;
// 出厂自检页：不在菜单里，只由开机断电续跑与设置页隐藏入口进入。
// Factory self-test page: not in the menu; reached by the boot power-cut resume and a hidden entry in Settings.
extern const app_desc_t app_selftest;

// 阅读菜单：相近的页挨着排，由 ui_menu 按页翻。/ Reading menu; ui_menu pages through it.
static const app_desc_t* const s_apps[] = {
    &app_book,
    &app_transfer,
    &app_font_pick,
    &app_storage,
    &app_settings,
};

#define APP_COUNT ((int)(sizeof(s_apps) / sizeof(s_apps[0])))

int app_count(void) {
    return APP_COUNT;
}

const app_desc_t* app_at(int index) {
    if (index < 0 || index >= APP_COUNT) return NULL;
    return s_apps[index];
}

int app_index_of(const app_desc_t* app) {
    for (int i = 0; i < APP_COUNT; i++) {
        if (s_apps[i] == app) return i;
    }
    return -1;
}

// 开机首页：书架。app_loop 在 first_app 为空时也回退到这里。
// Boot page: the shelf. app_loop also falls back here when first_app is empty.
const app_desc_t* app_home_page(void) {
    return &app_book;
}

// 自检页只由开机断电续跑进入，不进菜单。/ The self-test page is boot-resume only and stays out of the menu.
const app_desc_t* app_selftest_page(void) {
    return &app_selftest;
}
