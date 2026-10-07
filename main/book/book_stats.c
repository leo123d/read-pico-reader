/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 阅读统计的 NVS 读写与内存累加。/ NVS storage and in-memory accumulation for reading stats.
 */
#include "book_stats.h"
#include "book_progress.h"
#include "book_store.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#define PATH_CAP BOOK_STORE_PATH_MAX
#define STATS_HEADER 14
#define STATS_CAP (STATS_HEADER + PATH_CAP)
// 30 秒落一次盘：翻页不会触发写入，意外断电最多丢半分钟。
// Flush every 30 s: page turns never write, and an unexpected power loss costs at most half a minute.
#define STATS_FLUSH_MS 30000

static const char* TAG = "book_stats";
static const char* NS = "rp_stats";

static uint32_t s_total_s;
static uint32_t s_opens;
static uint32_t s_boot_s;
static char s_cur_path[PATH_CAP];
static uint32_t s_cur_s;
static uint32_t s_cur_opens;
static bool s_cur_dirty;
static bool s_global_dirty;
static bool s_loaded;
static int64_t s_flush_ms;

static bool path_valid(const char* path) {
    return path != NULL && path[0] != '\0' && strnlen(path, PATH_CAP) < PATH_CAP;
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static uint16_t get16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put16(uint8_t* p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static esp_err_t warn_error(esp_err_t err) {
    if (err != ESP_OK) ESP_LOGW(TAG, "stats persistence: %s", esp_err_to_name(err));
    return err;
}

static void load_global(void) {
    if (s_loaded) return;
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    uint32_t total = 0, opens = 0;
    if (nvs_get_u32(h, "total", &total) == ESP_OK) s_total_s = total;
    if (nvs_get_u32(h, "opens", &opens) == ESP_OK) s_opens = opens;
    nvs_close(h);
}

// 记录必须版本、长度与完整路径三者自洽，否则视为损坏并不再覆盖。
// Version, length and full path must agree, or the record counts as corrupt and is left alone.
static bool record_valid(const uint8_t* data, size_t len, size_t* path_len) {
    if (len < STATS_HEADER + 2 || len > STATS_CAP || memcmp(data, "RPS", 3) != 0 || data[3] != 1) return false;
    size_t bytes = len - STATS_HEADER;
    if (bytes > PATH_CAP || get16(data + 12) != bytes) return false;
    if (strnlen((const char*)data + STATS_HEADER, bytes) != bytes - 1) return false;
    *path_len = bytes;
    return true;
}

// 不存在返回 ESP_ERR_NVS_NOT_FOUND；损坏或路径不符返回 ESP_ERR_INVALID_STATE。
// Absent yields ESP_ERR_NVS_NOT_FOUND; corrupt or foreign yields ESP_ERR_INVALID_STATE.
static esp_err_t record_read(nvs_handle_t h, const char* key, const char* path,
                             uint32_t* seconds, uint32_t* opens) {
    uint8_t data[STATS_CAP];
    size_t len = sizeof(data);
    esp_err_t err = nvs_get_blob(h, key, data, &len);
    if (err != ESP_OK) return err;
    size_t path_len = 0;
    if (!record_valid(data, len, &path_len) ||
        strcmp((const char*)data + STATS_HEADER, path) != 0) return ESP_ERR_INVALID_STATE;
    if (seconds) *seconds = get32(data + 4);
    if (opens) *opens = get32(data + 8);
    return ESP_OK;
}

static void record_write(nvs_handle_t h, const char* key, const char* path,
                         uint32_t seconds, uint32_t opens) {
    uint8_t data[STATS_CAP] = {'R', 'P', 'S', 1};
    put32(data + 4, seconds);
    put32(data + 8, opens);
    size_t path_len = strlen(path) + 1;
    put16(data + 12, (uint16_t)path_len);
    memcpy(data + STATS_HEADER, path, path_len);
    nvs_set_blob(h, key, data, STATS_HEADER + path_len);
}

// 切换当前书：先把上一本的增量落盘，再载入新书的累计值。
// Switch the current book: flush the previous increment first, then load the new book's totals.
static void switch_book(const char* path) {
    book_stats_flush();
    s_cur_path[0] = '\0';
    s_cur_s = 0;
    s_cur_opens = 0;
    s_cur_dirty = false;
    if (!path_valid(path)) return;
    strcpy(s_cur_path, path);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    char key[11];
    book_progress_key('t', path, key);
    record_read(h, key, path, &s_cur_s, &s_cur_opens);
    nvs_close(h);
}

void book_stats_global(book_stats_t* out) {
    if (out == NULL) return;
    load_global();
    *out = (book_stats_t){ .total_s = s_total_s, .opens = s_opens, .boot_s = s_boot_s };
}

void book_stats_open(const char* path) {
    if (!path_valid(path)) return;
    load_global();
    if (!s_cur_path[0] || strcmp(s_cur_path, path)) switch_book(path);
    ++s_opens;
    ++s_cur_opens;
    s_global_dirty = true;
    s_cur_dirty = true;
    // 打开次数是低频事件，立刻落盘，避免开机只看不读时统计全丢。
    // Opens are rare; persist immediately so a session of browsing still counts.
    book_stats_flush();
}

void book_stats_accumulate(const char* path, uint32_t seconds) {
    if (!seconds || !path_valid(path)) return;
    load_global();
    if (strcmp(s_cur_path, path)) switch_book(path);
    s_cur_s += seconds;
    s_total_s += seconds;
    s_boot_s += seconds;
    s_cur_dirty = true;
    s_global_dirty = true;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_flush_ms >= STATS_FLUSH_MS) book_stats_flush();
}

void book_stats_flush(void) {
    load_global();
    bool global = s_global_dirty;
    bool book = s_cur_dirty && s_cur_path[0];
    if (!global && !book) return;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { warn_error(err); return; }
    if (global) {
        err = nvs_set_u32(h, "total", s_total_s);
        if (err == ESP_OK) err = nvs_set_u32(h, "opens", s_opens);
        if (err == ESP_OK) err = nvs_commit(h);
        if (err == ESP_OK) s_global_dirty = false;
    }
    if (err == ESP_OK && book) {
        char key[11];
        book_progress_key('t', s_cur_path, key);
        record_write(h, key, s_cur_path, s_cur_s, s_cur_opens);
        err = nvs_commit(h);
        if (err == ESP_OK) s_cur_dirty = false;
    }
    nvs_close(h);
    warn_error(err);
    s_flush_ms = esp_timer_get_time() / 1000;
}

bool book_stats_book(const char* path, uint32_t* seconds, uint32_t* opens) {
    if (!path_valid(path)) return false;
    if (s_cur_path[0] && !strcmp(s_cur_path, path)) {
        if (seconds) *seconds = s_cur_s;
        if (opens) *opens = s_cur_opens;
        return true;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char key[11];
    book_progress_key('t', path, key);
    esp_err_t err = record_read(h, key, path, seconds, opens);
    nvs_close(h);
    return err == ESP_OK;
}

esp_err_t book_stats_forget(const char* path) {
    if (!path_valid(path)) return ESP_ERR_INVALID_ARG;
    if (!strcmp(s_cur_path, path)) {
        s_cur_path[0] = '\0';
        s_cur_s = 0;
        s_cur_opens = 0;
        s_cur_dirty = false;
    }
    char key[11];
    book_progress_key('t', path, key);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return warn_error(err);
    uint32_t probe = 0;
    err = record_read(h, key, path, &probe, NULL);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return warn_error(err);
}
