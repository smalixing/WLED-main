/**
 * @file wled_c_segment.c
 * @brief 段结构与段管理实现(任务 6)。
 *
 * 槽位池:s_segments[WLEDC_MAX_SEGMENTS](ID 即下标,active 占用)。
 * 运行时效果数据:lazy 分配(request_data,容量只增不减)+
 * 全局用量记账 s_used_segment_data(clamp ≥ 0,对齐 WLED
 * addUsedSegmentData)+ 效果切换统一重置(mark_for_reset /
 * reset_if_required)。
 */
#include <stdlib.h>
#include <string.h>

#include "wled_c_segment.h"

/* ------------------------------------------------------------------ */
/* 内部状态                                                             */
/* ------------------------------------------------------------------ */

/** 段槽位池(ID 即下标;active=false 为空槽)。 */
static wledc_segment_t s_segments[WLEDC_MAX_SEGMENTS];

/** 全部段效果数据总用量记账(字节,对齐 WLED _usedSegmentData)。 */
static unsigned s_used_segment_data;

/* ------------------------------------------------------------------ */
/* 内部工具                                                             */
/* ------------------------------------------------------------------ */

/** 越界安全取活跃段;ID 非法或槽位空闲时返回 NULL。 */
static wledc_segment_t *seg_find(wledc_segment_id_t id)
{
    if (id >= WLEDC_MAX_SEGMENTS) {
        return NULL;
    }
    wledc_segment_t *seg = &s_segments[id];
    return seg->active ? seg : NULL;
}

/**
 * 段物理像素数:1D 为 stop-start,2D(startY<stopY)为宽×高。
 * 返回 uint32_t 以避免 uint16 乘法溢出。
 */
static uint32_t seg_phys_len(const wledc_segment_t *seg)
{
    uint32_t len = (uint32_t)(seg->stop - seg->start);
    if (seg->stopY > seg->startY) {
        len *= (uint32_t)(seg->stopY - seg->startY);
    }
    return len;
}

/**
 * 按当前范围重分配段像素缓冲(容量变化时整体清零)。
 * 调用方负责失败时回滚范围字段。
 */
static esp_err_t seg_realloc_pixels(wledc_segment_t *seg)
{
    uint32_t need = seg_phys_len(seg);
    if (need == 0 || need > 0xFFFFu) {
        return ESP_ERR_INVALID_ARG;
    }
    if (seg->pixels && seg->pixels_len == (uint16_t)need) {
        return ESP_OK; /* 范围未变化,保留已渲染内容 */
    }

    uint32_t *p = realloc(seg->pixels, (size_t)need * sizeof(uint32_t));
    if (p == NULL) {
        return ESP_ERR_NO_MEM;
    }
    seg->pixels = p;
    seg->pixels_len = (uint16_t)need;
    memset(p, 0, (size_t)need * sizeof(uint32_t));
    return ESP_OK;
}

/**
 * 释放段效果数据并扣减全局用量(向下 clamp 到 0,对齐 WLED
 * addUsedSegmentData 的防御语义)。
 */
static void seg_release_data(wledc_segment_t *seg)
{
    if (seg->data != NULL) {
        if (seg->data_len <= s_used_segment_data) {
            s_used_segment_data -= seg->data_len;
        } else {
            s_used_segment_data = 0; /* 记账失衡防御 */
        }
        free(seg->data);
        seg->data = NULL;
        seg->data_len = 0;
    }
}

/* ------------------------------------------------------------------ */
/* 段生命周期                                                           */
/* ------------------------------------------------------------------ */

esp_err_t wledc_segment_add(uint16_t start, uint16_t stop,
                            wledc_segment_id_t *out_id)
{
    if (start >= stop) {
        return ESP_ERR_INVALID_ARG;
    }

    wledc_segment_id_t id;
    for (id = 0; id < WLEDC_MAX_SEGMENTS; id++) {
        if (!s_segments[id].active) {
            break;
        }
    }
    if (id == WLEDC_MAX_SEGMENTS) {
        return WLEDC_ERR_NO_SEGMENT;
    }

    wledc_segment_t *seg = &s_segments[id];
    memset(seg, 0, sizeof(*seg));
    seg->start = start;
    seg->stop = stop;
    seg->colors[0] = WLEDC_SEGMENT_DEFAULT_COLOR;
    seg->options = SEG_OPT_ON;
    seg->grouping = 1;
    seg->opacity = 255;
    seg->cct = 255;
    seg->speed = WLEDC_SEGMENT_DEFAULT_SPEED;
    seg->intensity = WLEDC_SEGMENT_DEFAULT_INTENSITY;

    esp_err_t err = seg_realloc_pixels(seg);
    if (err != ESP_OK) {
        memset(seg, 0, sizeof(*seg)); /* 回滚槽位 */
        return err;
    }

    seg->active = true;
    if (out_id != NULL) {
        *out_id = id;
    }
    return ESP_OK;
}

esp_err_t wledc_segment_remove(wledc_segment_id_t id)
{
    wledc_segment_t *seg = seg_find(id);
    if (seg == NULL) {
        return WLEDC_ERR_NO_SEGMENT;
    }

    seg_release_data(seg);
    free(seg->pixels);
    memset(seg, 0, sizeof(*seg));
    return ESP_OK;
}

wledc_segment_t *wledc_segment_get(wledc_segment_id_t id)
{
    return seg_find(id);
}

wledc_segment_t *wledc_segment_get_all(uint8_t *count)
{
    if (count != NULL) {
        *count = WLEDC_MAX_SEGMENTS;
    }
    return s_segments;
}

uint8_t wledc_segment_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < WLEDC_MAX_SEGMENTS; i++) {
        if (s_segments[i].active) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 段修改                                                               */
/* ------------------------------------------------------------------ */

esp_err_t wledc_segment_set_range(wledc_segment_id_t id,
                                   uint16_t start, uint16_t stop)
{
    wledc_segment_t *seg = seg_find(id);
    if (seg == NULL) {
        return WLEDC_ERR_NO_SEGMENT;
    }
    if (start >= stop) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t old_start = seg->start;
    uint16_t old_stop = seg->stop;
    seg->start = start;
    seg->stop = stop;

    esp_err_t err = seg_realloc_pixels(seg);
    if (err != ESP_OK) {
        seg->start = old_start; /* 分配失败回滚范围 */
        seg->stop = old_stop;
        return err;
    }

    wledc_segment_mark_for_reset(seg);
    return ESP_OK;
}

esp_err_t wledc_segment_set_dimensions(wledc_segment_id_t id,
                                        uint16_t width, uint16_t height)
{
    wledc_segment_t *seg = seg_find(id);
    if (seg == NULL) {
        return WLEDC_ERR_NO_SEGMENT;
    }
    if (width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 2D 物理像素数(宽×高)与 stop 溢出防御 */
    if ((uint32_t)width * height > 0xFFFFu ||
        (uint32_t)seg->start + width > 0xFFFFu ||
        (uint32_t)seg->startY + height > 0xFFFFu) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t old_stop = seg->stop;
    uint16_t old_stopY = seg->stopY;
    seg->stop = (uint16_t)(seg->start + width);
    seg->stopY = (uint16_t)(seg->startY + height);

    esp_err_t err = seg_realloc_pixels(seg);
    if (err != ESP_OK) {
        seg->stop = old_stop;
        seg->stopY = old_stopY;
        return err;
    }

    wledc_segment_mark_for_reset(seg);
    return ESP_OK;
}

esp_err_t wledc_segment_set_mode(wledc_segment_id_t id, wledc_fx_id_t mode)
{
    wledc_segment_t *seg = seg_find(id);
    if (seg == NULL) {
        return WLEDC_ERR_NO_SEGMENT;
    }
    if (seg->mode != mode) {
        seg->mode = mode;
        wledc_segment_mark_for_reset(seg); /* 效果切换统一重置 */
    }
    return ESP_OK;
}

esp_err_t wledc_segment_set_palette(wledc_segment_id_t id,
                                    wledc_palette_id_t palette)
{
    wledc_segment_t *seg = seg_find(id);
    if (seg == NULL) {
        return WLEDC_ERR_NO_SEGMENT;
    }
    seg->palette = palette; /* 与 WLED 一致:换调色板不重置运行时 */
    return ESP_OK;
}

esp_err_t wledc_segment_set_options(wledc_segment_id_t id, uint16_t options)
{
    wledc_segment_t *seg = seg_find(id);
    if (seg == NULL) {
        return WLEDC_ERR_NO_SEGMENT;
    }
    seg->options = options;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 运行时数据管理(效果引擎使用)                                        */
/* ------------------------------------------------------------------ */

uint8_t *wledc_segment_request_data(wledc_segment_t *seg, uint16_t len)
{
    if (seg == NULL || !seg->active || len == 0) {
        return NULL;
    }
    if (seg->data != NULL && seg->data_len >= len) {
        return seg->data; /* 容量足够,只增不减 */
    }

    uint8_t *d = realloc(seg->data, len);
    if (d == NULL) {
        return NULL; /* 保留旧 data 供效果继续运行 */
    }
    /* 增量清零:保留旧内容,仅清空新扩展区域 */
    memset(d + seg->data_len, 0, (size_t)len - seg->data_len);
    s_used_segment_data += (unsigned)len - seg->data_len;
    seg->data = d;
    seg->data_len = len;
    return d;
}

void wledc_segment_mark_for_reset(wledc_segment_t *seg)
{
    if (seg != NULL) {
        seg->options |= SEG_OPT_RESET;
    }
}

void wledc_segment_reset_if_required(wledc_segment_t *seg)
{
    if (seg == NULL || !seg->active) {
        return;
    }
    if (!(seg->options & SEG_OPT_RESET)) {
        return;
    }
    seg->options &= (uint16_t)~SEG_OPT_RESET;
    seg->step = 0;
    seg->call = 0;
    seg->aux0 = 0;
    seg->aux1 = 0;
    seg_release_data(seg);
}

unsigned wledc_segment_used_data(void)
{
    return s_used_segment_data;
}