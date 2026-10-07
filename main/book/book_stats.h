/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 阅读时长统计：全局累计与单本明细，落在 rp_stats 命名空间。
 * Reading-time statistics: global totals and per-book detail in the rp_stats namespace.
 *
 * 冻结：只从主循环任务调用，不在绘制或预渲染路径里读写；累计值先在内存里攒，
 * 跨过落盘间隔或离页时才写 NVS，避免翻页时反复擦写。损坏或路径不符的记录不覆盖。
 * Frozen: call only from the app-loop task, never from drawing or preparation paths. Increments
 * accumulate in memory and reach NVS on the flush interval or on leaving, so page turns never thrash
 * the flash. Corrupt or foreign records are never overwritten.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    /// 累计阅读秒数。/ Total reading seconds.
    uint32_t total_s;
    /// 累计打开图书次数。/ Total number of book opens.
    uint32_t opens;
    /// 本次开机以来的阅读秒数。/ Reading seconds since this boot.
    uint32_t boot_s;
} book_stats_t;

/// 读取全局统计；首次调用从 NVS 载入。/ Read global totals; the first call loads them from NVS.
void book_stats_global(book_stats_t* out);

/// 打开一本图书：结算上一本、记一次打开并立即落盘。/ Open a book: settle the previous one, count this open, and persist now.
void book_stats_open(const char* path);

/// 累加阅读秒数；只在跨过落盘间隔时写 NVS。/ Accumulate reading seconds; NVS is written only past the flush interval.
void book_stats_accumulate(const char* path, uint32_t seconds);

/// 立刻把内存增量落盘；离页、删除图书或关机前调用。/ Flush pending increments now, before leaving, deleting or powering off.
void book_stats_flush(void);

/// 单本已读秒数与打开次数；内存里未落盘的增量优先。无记录返回 false 且不改输出。
/// One book's seconds and opens; pending in-memory values win. Returns false without touching the outputs when absent.
bool book_stats_book(const char* path, uint32_t* seconds, uint32_t* opens);

/// 删除图书时清掉它的统计；幂等，可重试。/ Drop a book's stats on delete; idempotent and retryable.
esp_err_t book_stats_forget(const char* path);
