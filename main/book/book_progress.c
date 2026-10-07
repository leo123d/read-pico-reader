/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 版本化小端进度记录与书签记录，以完整路径校验哈希碰撞。
 * Versioned little-endian progress and bookmark records with full-path collision checks.
 *
 * 冻结：独立 rp_books 命名空间；不擦除分区、不覆盖碰撞书籍。为可靠最近阅读排序，
 * v2 改用持久序号，v1 仍可恢复但排序为 0；遗忘只清指定路径，部分失败允许重试。
 * 书签以 m_ 前缀与进度 b_ 前缀区分，键由同一套路径哈希推出。
 * Frozen: use rp_books; never erase partitions or overwrite collisions. Reliable recent ordering uses
 * persistent v2 sequences; v1 resumes with order zero. Forget only the given path, retrying partial failures.
 * Bookmarks use the m_ prefix against progress's b_, both derived from the same path hash.
 */
#include "book_progress.h"
#include "book_store.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

#define PATH_CAP BOOK_STORE_PATH_MAX
#define HEADER_SIZE 24
#define RECORD_CAP (HEADER_SIZE + PATH_CAP)
static const char* TAG = "book_progress";
static const char* NS = "rp_books";

// 列表拓扑仅在 HTTP 停止并 join 后修改；HTTP 只原子标记已有节点。
// Mutate list topology only after HTTP stops and joins; HTTP only atomically marks existing nodes.
struct book_progress_watch {
    char path[PATH_CAP];
    atomic_bool invalidated;
    struct book_progress_watch* next;
};
static book_progress_watch_t* s_watches;

static bool path_valid(const char* path) {
    return path != NULL && path[0] != '\0' && strnlen(path, PATH_CAP) < PATH_CAP;
}

book_progress_watch_t* book_progress_watch_create(const char* path) {
    if (!path_valid(path)) return NULL;
    book_progress_watch_t* watch = malloc(sizeof(*watch));
    if (!watch) return NULL;
    strcpy(watch->path, path);
    atomic_init(&watch->invalidated, false);
    watch->next = s_watches;
    s_watches = watch;
    return watch;
}

bool book_progress_watch_invalidated(const book_progress_watch_t* watch) {
    return watch && atomic_load(&watch->invalidated);
}

void book_progress_watch_destroy(book_progress_watch_t* watch) {
    book_progress_watch_t** p = &s_watches;
    while (*p && *p != watch) p = &(*p)->next;
    if (*p) { *p = watch->next; free(watch); }
}

void book_progress_key(char prefix, const char* path, char key[11]) {
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p) {
        hash = (hash ^ *p) * UINT32_C(16777619);
    }
    snprintf(key, 11, "%c_%08" PRIx32, prefix, hash);
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static bool record_valid(const uint8_t* data, size_t len) {
    if (len < HEADER_SIZE + 2 || len > RECORD_CAP || memcmp(data, "RPB", 3) != 0 ||
        (data[3] != 1 && data[3] != 2)) return false;
    size_t path_len = len - HEADER_SIZE;
    if (data[3] == 1 && (path_len > 160 || data[23] != 0)) return false;
    return ((size_t)data[22] | ((size_t)data[23] << 8)) == path_len &&
        strnlen((const char*)data + HEADER_SIZE, path_len) == path_len - 1;
}

static esp_err_t warn_error(esp_err_t err) {
    if (err != ESP_OK) ESP_LOGW(TAG, "progress persistence: %s", esp_err_to_name(err));
    return err;
}

bool book_progress_load(const char* path, uint32_t size, book_progress_t* out) {
    if (!path_valid(path) || out == NULL) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char key[11];
    book_progress_key('b', path, key);
    uint8_t data[RECORD_CAP];
    size_t len = sizeof(data);
    esp_err_t err = nvs_get_blob(h, key, data, &len);
    nvs_close(h);
    if (err != ESP_OK || !record_valid(data, len) ||
        strcmp((const char*)data + HEADER_SIZE, path) != 0 || get32(data + 4) != size ||
        data[14] < 36 || data[14] > 72 || (data[14] - 36) % 4 != 0 || data[15] > 100) return false;
    *out = (book_progress_t){
        .file_size = get32(data + 4), .chapter = (uint16_t)(data[8] | (data[9] << 8)),
        .byte_off = get32(data + 10), .px = data[14], .pct = data[15],
        .last_open_s = data[3] == 2 ? get32(data + 16) : 0,
    };
    return true;
}

// 旧记录必须可识别且属于同一路径；损坏或未知版本也不盲目覆盖。
// Existing records must be recognized and owned by this path; preserve corrupt or unknown versions too.
static esp_err_t check_owner(nvs_handle_t h, const char* key, const char* path) {
    uint8_t data[RECORD_CAP];
    size_t len = sizeof(data);
    esp_err_t err = nvs_get_blob(h, key, data, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    return record_valid(data, len) && strcmp((const char*)data + HEADER_SIZE, path) == 0
        ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t book_progress_save(const char* path, const book_progress_t* progress) {
    if (!path_valid(path) || progress == NULL || progress->px < 36 || progress->px > 72 ||
        (progress->px - 36) % 4 != 0 || progress->pct > 100) return ESP_ERR_INVALID_ARG;
    uint8_t data[RECORD_CAP] = {'R', 'P', 'B', 2};
    put32(data + 4, progress->file_size);
    data[8] = (uint8_t)progress->chapter;
    data[9] = (uint8_t)(progress->chapter >> 8);
    put32(data + 10, progress->byte_off);
    data[14] = progress->px;
    data[15] = progress->pct;
    size_t path_len = strlen(path) + 1;
    data[22] = (uint8_t)path_len;
    data[23] = (uint8_t)(path_len >> 8);
    memcpy(data + HEADER_SIZE, path, path_len);
    char key[11];
    book_progress_key('b', path, key);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return warn_error(err);
    err = check_owner(h, key, path);
    uint32_t sequence = 0;
    if (err == ESP_OK) {
        err = nvs_get_u32(h, "seq", &sequence);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK && sequence == UINT32_MAX) err = ESP_ERR_INVALID_STATE;
    // 先提交序号预留，再写进度；失败可留空洞，但重启后不能倒序。
    // Commit the sequence reservation before progress; failures may leave gaps, never reverse order after restart.
    if (err == ESP_OK) err = nvs_set_u32(h, "seq", ++sequence);
    if (err == ESP_OK) err = nvs_commit(h);
    put32(data + 16, sequence);
    if (err == ESP_OK) err = nvs_set_blob(h, key, data, HEADER_SIZE + path_len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return warn_error(err);
}

esp_err_t book_progress_clear(const char* path) {
    if (!path_valid(path)) return ESP_ERR_INVALID_ARG;
    char key[11];
    book_progress_key('b', path, key);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return warn_error(err);
    err = check_owner(h, key, path);
    if (err == ESP_OK) err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return warn_error(err);
}

esp_err_t book_progress_forget(const char* path) {
    if (!path_valid(path)) return ESP_ERR_INVALID_ARG;
    for (book_progress_watch_t* watch = s_watches; watch; watch = watch->next)
        if (!strcmp(watch->path, path)) atomic_store(&watch->invalidated, true);
    char key[11];
    book_progress_key('b', path, key);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return warn_error(err);
    err = check_owner(h, key, path);
    char last[PATH_CAP];
    size_t len = sizeof(last);
    bool matching_last = false;
    if (err == ESP_OK) {
        err = nvs_get_str(h, "last", last, &len);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        else if (err == ESP_OK) matching_last = strcmp(last, path) == 0;
    }
    // 先移除自动恢复入口；失败不继续清进度，重试也不会波及其他书。
    // Remove automatic resume first; on failure retain progress, and retries never touch other books.
    if (err == ESP_OK && matching_last) {
        err = nvs_erase_key(h, "last");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(h);
    }
    if (err == ESP_OK) err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return warn_error(err);
}

bool book_progress_last_path(char* out, size_t cap) {
    if (out == NULL || cap == 0) return false;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, "last", out, &len);
    nvs_close(h);
    if (err != ESP_OK || !path_valid(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

esp_err_t book_progress_set_last_path(const char* path) {
    if (path == NULL || strnlen(path, PATH_CAP) >= PATH_CAP) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return warn_error(err);
    err = nvs_set_str(h, "last", path);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return warn_error(err);
}

/* ---- 书签 / Bookmarks ---- */

// 记录布局：8 字节头 + 完整路径 + 每条 7 字节（章节 u16、章节内偏移 u32、百分比 u8）。
// Record layout: an 8-byte header, the full path, then 7 bytes per mark (chapter u16, offset u32, percent u8).
#define MARK_HEADER 8
#define MARK_ENTRY 7
#define MARK_CAP (MARK_HEADER + PATH_CAP + BOOK_MARK_MAX * MARK_ENTRY)

static uint16_t get16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put16(uint8_t* p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

// 完整路径必须自洽，数量与长度必须和实际字节数吻合，否则整条记录作废。
// The full path must be self-consistent and the count must match the actual byte count, or the whole record is void.
static bool mark_record_valid(const uint8_t* data, size_t len, size_t* path_len, size_t* count) {
    if (len < MARK_HEADER + 2 + MARK_ENTRY || len > MARK_CAP ||
        memcmp(data, "RPM", 3) != 0 || data[3] != 1) return false;
    size_t marks = get16(data + 4);
    if (marks > BOOK_MARK_MAX) return false;
    size_t bytes = len - MARK_HEADER - marks * MARK_ENTRY;
    if (bytes < 2 || bytes > PATH_CAP || get16(data + 6) != bytes) return false;
    if (strnlen((const char*)data + MARK_HEADER, bytes) != bytes - 1) return false;
    *path_len = bytes;
    *count = marks;
    return true;
}

// 读取已存在的记录；不存在返回 ESP_ERR_NVS_NOT_FOUND，损坏或路径不符返回 ESP_ERR_INVALID_STATE。
// Read an existing record; absent yields ESP_ERR_NVS_NOT_FOUND, corrupt or foreign yields ESP_ERR_INVALID_STATE.
static esp_err_t mark_read(nvs_handle_t h, const char* key, const char* path,
                           book_mark_t* out, size_t cap, size_t* count) {
    uint8_t data[MARK_CAP];
    size_t len = sizeof(data);
    esp_err_t err = nvs_get_blob(h, key, data, &len);
    if (err != ESP_OK) return err;
    size_t path_len = 0, marks = 0;
    if (!mark_record_valid(data, len, &path_len, &marks) ||
        strcmp((const char*)data + MARK_HEADER, path) != 0) return ESP_ERR_INVALID_STATE;
    if (out) {
        for (size_t i = 0; i < marks && i < cap; ++i) {
            const uint8_t* entry = data + MARK_HEADER + path_len + i * MARK_ENTRY;
            out[i] = (book_mark_t){ .chapter = get16(entry), .byte_off = get32(entry + 2), .pct = entry[6] };
        }
    }
    if (count) *count = marks;
    return ESP_OK;
}

static esp_err_t mark_write(nvs_handle_t h, const char* key, const char* path,
                            const book_mark_t* marks, size_t count) {
    uint8_t data[MARK_CAP] = {'R', 'P', 'M', 1};
    put16(data + 4, (uint16_t)count);
    size_t path_len = strlen(path) + 1;
    put16(data + 6, (uint16_t)path_len);
    memcpy(data + MARK_HEADER, path, path_len);
    for (size_t i = 0; i < count; ++i) {
        uint8_t* entry = data + MARK_HEADER + path_len + i * MARK_ENTRY;
        put16(entry, marks[i].chapter);
        put32(entry + 2, marks[i].byte_off);
        entry[6] = marks[i].pct > 100 ? 100 : marks[i].pct;
    }
    esp_err_t err = nvs_set_blob(h, key, data, MARK_HEADER + path_len + count * MARK_ENTRY);
    if (err == ESP_OK) err = nvs_commit(h);
    return err;
}

static bool mark_same(const book_mark_t* a, const book_mark_t* b) {
    return a->chapter == b->chapter && a->byte_off == b->byte_off;
}

esp_err_t book_marks_load(const char* path, book_mark_t* out, size_t cap, size_t* count) {
    if (count) *count = 0;
    if (!path_valid(path)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    char key[11];
    book_progress_key('m', path, key);
    err = mark_read(h, key, path, out, cap, count);
    nvs_close(h);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t book_marks_add(const char* path, const book_mark_t* mark, size_t* count) {
    if (count) *count = 0;
    if (!path_valid(path) || mark == NULL) return ESP_ERR_INVALID_ARG;
    char key[11];
    book_progress_key('m', path, key);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return warn_error(err);
    book_mark_t marks[BOOK_MARK_MAX];
    size_t existing = 0;
    err = mark_read(h, key, path, marks, BOOK_MARK_MAX, &existing);
    if (err == ESP_ERR_NVS_NOT_FOUND) { err = ESP_OK; existing = 0; }
    if (err == ESP_OK) {
        for (size_t i = 0; i < existing; ++i) if (mark_same(&marks[i], mark)) {
            if (count) *count = existing;
            nvs_close(h);
            return ESP_OK;
        }
        if (existing == BOOK_MARK_MAX) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        marks[existing++] = *mark;
        err = mark_write(h, key, path, marks, existing);
        if (err == ESP_OK && count) *count = existing;
    }
    nvs_close(h);
    return warn_error(err);
}

esp_err_t book_marks_remove(const char* path, const book_mark_t* mark, size_t* count) {
    if (count) *count = 0;
    if (!path_valid(path) || mark == NULL) return ESP_ERR_INVALID_ARG;
    char key[11];
    book_progress_key('m', path, key);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return warn_error(err);
    book_mark_t marks[BOOK_MARK_MAX];
    size_t existing = 0;
    err = mark_read(h, key, path, marks, BOOK_MARK_MAX, &existing);
    if (err == ESP_ERR_NVS_NOT_FOUND) { err = ESP_OK; existing = 0; }
    if (err == ESP_OK) {
        size_t kept = 0;
        for (size_t i = 0; i < existing; ++i) if (!mark_same(&marks[i], mark)) marks[kept++] = marks[i];
        if (kept == existing) {
            if (count) *count = existing;
            nvs_close(h);
            return ESP_OK;
        }
        // 最后一条删掉就整条抹除，避免留下空记录占键位。
        // Erase the whole record once the last mark goes, so no empty record keeps the key.
        err = kept ? mark_write(h, key, path, marks, kept) : nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(h);
        if (err == ESP_OK && count) *count = kept;
    }
    nvs_close(h);
    return warn_error(err);
}

// 先确认记录属于这本书，再删除；碰撞或损坏的记录原样保留。
// Confirm the record belongs to this book before erasing; colliding or corrupt records stay untouched.
static esp_err_t mark_erase(const char* path) {
    if (!path_valid(path)) return ESP_ERR_INVALID_ARG;
    char key[11];
    book_progress_key('m', path, key);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return warn_error(err);
    book_mark_t probe;
    err = mark_read(h, key, path, &probe, 1, NULL);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return warn_error(err);
}

esp_err_t book_marks_clear(const char* path) {
    return mark_erase(path);
}

esp_err_t book_marks_forget(const char* path) {
    return mark_erase(path);
}
