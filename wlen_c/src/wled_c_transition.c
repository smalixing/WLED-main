/**
 * @file wled_c_transition.c
 * @brief 段过渡实现(契约见 wled_c_transition.h)。
 *
 * WLED 参照:FX_fcn.cpp startTransition / updateTransitionProgress /
 * currentBri / currentColor(L293-420)。
 */
#include "wled_c_transition.h"

#ifdef WLEDC_ENABLE_TRANSITIONS

#include <string.h>

#include "wled_c_color.h"
#include "wled_c_fx.h"
#include "wled_c_segment.h"

/* ------------------------------------------------------------------ */
/* 内部辅助                                                             */
/* ------------------------------------------------------------------ */

/** 段当前开(SEG_OPT_ON)即视觉亮度来源为 opacity,否则 0。 */
static uint8_t seg_bri_now(const wledc_segment_t *seg)
{
    return (seg->options & SEG_OPT_ON) ? seg->opacity : 0;
}

/**
 * 当前段调色板 entries(经效果引擎解析:ID 0-5 动态构造,>=6 透传)。
 * 解析失败返回 false 且填 0(视觉为黑)。
 */
static bool palette_now(const wledc_segment_t *seg,
                        uint8_t out[WLEDC_PALETTE_ENTRIES_BYTES])
{
    if (!wledc_fx_segment_palette_entries(seg, out)) {
        memset(out, 0, WLEDC_PALETTE_ENTRIES_BYTES);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                             */
/* ------------------------------------------------------------------ */

void wledc_segment_start_transition(wledc_segment_t *seg,
                                    uint16_t duration_ms,
                                    wledc_time_ms_t now)
{
    if (seg == NULL) {
        return;
    }
    /* 段未开或零时长:WLED 语义为立即结束(若在过渡中)。 */
    if (duration_ms == 0 || !(seg->options & SEG_OPT_ON)) {
        wledc_segment_stop_transition(seg);
        return;
    }

    if (seg->transition.active) {
        /* 压缩重启:快照 = 当前视觉混合值,避免新过渡从旧起点跳变。 */
        wledc_segment_update_transition_progress(seg, now);
        for (int i = 0; i < WLEDC_NUM_COLORS; i++) {
            seg->transition.colors[i] =
                wledc_segment_current_color(seg, i);
        }
        seg->transition.bri    = wledc_segment_current_bri(seg);
        seg->transition.cct    = wledc_segment_current_cct(seg);
        {
            uint8_t mix[WLEDC_PALETTE_ENTRIES_BYTES];
            (void)wledc_segment_current_palette(seg, mix);
            memcpy(seg->transition.pal_entries, mix, sizeof(mix));
        }
    } else {
        /* 首次启动:捕获段当前参数为混合起点。 */
        for (int i = 0; i < WLEDC_NUM_COLORS; i++) {
            seg->transition.colors[i] = seg->colors[i];
        }
        seg->transition.bri = seg_bri_now(seg);
        seg->transition.cct = seg->cct;
        (void)palette_now(seg, seg->transition.pal_entries);
    }

    seg->transition.active   = true;
    seg->transition.duration = duration_ms;
    seg->transition.start    = now;
    seg->transition.progress = 0;
}

void wledc_segment_stop_transition(wledc_segment_t *seg)
{
    if (seg != NULL) {
        seg->transition.active = false;
    }
}

bool wledc_segment_in_transition(const wledc_segment_t *seg)
{
    return seg != NULL && seg->transition.active;
}

void wledc_segment_update_transition_progress(wledc_segment_t *seg,
                                              wledc_time_ms_t now)
{
    if (seg == NULL || !seg->transition.active) {
        return;
    }
    /* 无符号差对时间轴回绕安全(见 wled_c_types.h wledc_time_ms_t)。 */
    wledc_time_ms_t elapsed = now - seg->transition.start;
    if (elapsed >= seg->transition.duration) {
        seg->transition.progress = WLEDC_TRANSITION_MAX;
        seg->transition.active   = false; /* 完成即结束 */
        return;
    }
    seg->transition.progress = (uint16_t)((elapsed * WLEDC_TRANSITION_MAX) /
                                          seg->transition.duration);
}

/* ------------------------------------------------------------------ */
/* 过渡期视觉取值(消费方:任务 8/9 效果绘制、任务 11 输出层)          */
/* ------------------------------------------------------------------ */

uint32_t wledc_segment_current_color(const wledc_segment_t *seg, int idx)
{
    if (seg == NULL) {
        return 0;
    }
    if (idx < 0 || idx >= WLEDC_NUM_COLORS) {
        idx = 0; /* 越界回退层 0(WLED 数组访问安全策略)。 */
    }
    if (!seg->transition.active) {
        return seg->colors[idx];
    }
    return wledc_color_blend16(seg->transition.colors[idx], seg->colors[idx],
                               seg->transition.progress);
}

uint8_t wledc_segment_current_bri(const wledc_segment_t *seg)
{
    if (seg == NULL) {
        return 0;
    }
    if (!seg->transition.active) {
        return seg_bri_now(seg);
    }
    return (uint8_t)wledc_color_blend16(seg->transition.bri, seg_bri_now(seg),
                                        seg->transition.progress);
}

uint8_t wledc_segment_current_cct(const wledc_segment_t *seg)
{
    if (seg == NULL) {
        return 0;
    }
    if (!seg->transition.active) {
        return seg->cct;
    }
    return (uint8_t)wledc_color_blend16(seg->transition.cct, seg->cct,
                                        seg->transition.progress);
}

bool wledc_segment_current_palette(const wledc_segment_t *seg,
                                    uint8_t out[WLEDC_PALETTE_ENTRIES_BYTES])
{
    if (seg == NULL || out == NULL) {
        return false;
    }
    {
        uint8_t nowp[WLEDC_PALETTE_ENTRIES_BYTES];
        bool ok = palette_now(seg, nowp);
        if (!seg->transition.active) {
            memcpy(out, nowp, WLEDC_PALETTE_ENTRIES_BYTES);
            return ok;
        }
        /* 偏差:直接逐字节线性插值(= WLED 渐进 nblend 的数学极限)。 */
        for (int i = 0; i < WLEDC_PALETTE_ENTRIES_BYTES; i++) {
            out[i] = (uint8_t)wledc_color_blend16(seg->transition.pal_entries[i],
                                                  nowp[i],
                                                  seg->transition.progress);
        }
        return ok;
    }
}

#endif /* WLEDC_ENABLE_TRANSITIONS */