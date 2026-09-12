/**
 * @file wled_c_strip.c
 * @brief 灯带核心：帧率调度与总线编排（契约见 wled_c_strip.h）。
 *
 * WLED 参照：FX_fcn.cpp（service / setBrightness / restartRuntime /
 * setTransitionMode / resetTimebase / trigger / setTargetFps，以及
 * blendSegment 的段像素→总线输出映射，含 1D 直通与 2D 虚拟网格两条
 * 路径）。
 *
 * 不移植范围：
 *  - suspend / waitForIt / _isServicing 服务保护（单线程组件）；
 *  - blendSegment 的过渡期像素混合 / clip / push / modeBlend 路径
 *    （平滑由 wled_c_transition 参数插值保证，service 不做像素混合）；
 *  - cctFromRgb / _pixelCCT / gammaCorrectBri（色温仅输出期经 kelvin
 *    校色；亮度伽马不移植，颜色伽马归 wled_c_color）；
 *  - realtime 常驻层与 FPS 统计（_cumulativeFrames 等）；
 *  - 1D 段 offset 绕回（移植规则⑤，任务 8 已声明偏差）；
 *  - 2D 段 grouping/spacing 展开输出（任务 8 决策：2D 不建模折叠）。
 */

/*
 * SPDX-License-Identifier: GPL-3.0-only
 * This file is part of the WLED C rewrite (wled_c component).
 */

#include "wled_c/wled_c_strip.h"

#include "wled_c/wled_c_bri.h"
#include "wled_c/wled_c_bus.h"
#include "wled_c/wled_c_color.h"
#include "wled_c/wled_c_fx.h"
#include "wled_c/wled_c_segment.h"

#ifdef WLEDC_ENABLE_TRANSITIONS
#include "wled_c/wled_c_transition.h"
#endif

#ifdef WLEDC_ENABLE_2D
#include "wled_c/wled_c_2d.h"
#endif

/* ------------------------------------------------------------------ */
/* 内部状态                                                             */
/* ------------------------------------------------------------------ */

static wledc_time_ms_t s_last_show = 0;             /* 上次输出帧时刻 */
static bool s_triggered = false;                    /* 强制刷新标志 */
static wledc_time_ms_t s_timebase = 0;              /* 效果时间轴偏移 */
static uint8_t s_target_fps = WLEDC_FPS;            /* 目标帧率（默认 42） */
static uint16_t s_frametime = WLEDC_FRAME_TIME_MS;  /* 帧时间（默认 1000/42） */
static uint8_t s_brightness = 128;                  /* 原始亮度（WLED 默认 128） */

/* ------------------------------------------------------------------ */
/* 输出工具（FX_fcn.cpp blendSegment 输出映射）                          */
/* ------------------------------------------------------------------ */

/**
 * @brief 段像素输出混合：亮度缩放 + 色温校色。
 *
 * 亮度缩放只做一次：全局缩放已在 set_brightness 内下发总线，此处 bri
 * 为段级视觉亮度（opacity 或过渡值）。CCT 127 表示不校色（WLED 中点
 * 语义）；其余值线性映射 1900K..10060K（cct*8160/255 + 1900）。
 * WLED 的 gammaCorrectBri 不移植。
 */
static uint32_t output_mix_color(uint32_t color, uint8_t bri, uint8_t cct)
{
    if (bri < 255u) {
        color = wledc_fast_color_scale(color, bri);
    }
    if (cct != 127u) {
        uint16_t kelvin =
            (uint16_t)(1900u + ((uint32_t)cct * 8160u) / 255u);
        color = wledc_color_balance_from_kelvin(kelvin, color);
    }
    return color;
}

/**
 * @brief 全局像素写入：逐活跃总线路由（WLED 多总线同写语义）。
 *
 * 全局索引落在多条总线范围内时同时写；不在任何总线范围内则静默忽略
 * （bus_set_pixel_color 返回 ESP_ERR_INVALID_ARG，越界为常态）。
 */
static void output_set_global(const wledc_bus_t *buses, uint16_t index,
                              uint32_t color)
{
    for (uint8_t b = 0; b < WLEDC_MAX_BUSES; b++) {
        if (buses[b].active) {
            (void)wledc_bus_set_pixel_color((wledc_bus_id_t)b, index, color);
        }
    }
}

/**
 * @brief 1D 段输出：物理像素直通（blendSegment 1D 路径）。
 *
 * grouping/spacing/reverse/mirror 折叠已在 fx1d_set_pixel 写入期内
 * 联完成，段 pixels 即物理像素；此处仅做亮度/色温混合与全局寻址
 * （index = start + i）。offset 绕回不移植（偏差声明）。
 */
static void output_segment_1d(const wledc_segment_t *seg,
                              const wledc_bus_t *buses,
                              uint8_t bri, uint8_t cct)
{
    if (seg->pixels == NULL) {
        return; /* 懒分配未就绪（0 长防御，正常不达） */
    }
    uint16_t len = (uint16_t)(seg->stop - seg->start);
    for (uint16_t i = 0; i < len; i++) {
        uint32_t color = output_mix_color(seg->pixels[i], bri, cct);
        output_set_global(buses, (uint16_t)(seg->start + i), color);
    }
}

#ifdef WLEDC_ENABLE_2D
/**
 * @brief 2D 物理坐标写入（全局地址 = start + y * 行宽 + x）。
 *
 * 偏差声明：行宽取段物理宽度（WLED 以 matrix 全局 maxWidth 对齐，
 * 此处简化——本组件约定 2D 段行宽即段宽，多段共享矩阵需各自独占行）。
 */
static void output_2d_point(const wledc_segment_t *seg,
                            const wledc_bus_t *buses,
                            uint16_t phys_w, uint16_t x, uint16_t y,
                            uint32_t color)
{
    uint32_t index =
        (uint32_t)seg->start + (uint32_t)y * (uint32_t)phys_w + x;
    if (index > 0xFFFFu) {
        return; /* 超出 uint16 全局索引空间，静默丢弃 */
    }
    output_set_global(buses, (uint16_t)index, color);
}

/**
 * @brief 2D 段输出：虚拟网格遍历 + 输出期变换 + 镜像多写。
 *
 * blendSegment 2D 路径：遍历虚拟 (cx,cy)∈vw×vh 读 pixels；输出期完成
 * 坐标变换（reverse/reverseY/transpose，复用 wled_c_2d 纯函数）与镜像
 * 多写（MIRROR 写 x 镜像位、MIRROR_Y 写 y 镜像位、双开补对角点；
 * transpose 时镜像轴对调）。grouping 展开不建模（任务 8 决策）。
 */
static void output_segment_2d(const wledc_segment_t *seg,
                              const wledc_bus_t *buses,
                              uint8_t bri, uint8_t cct)
{
    uint16_t vw = 0, vh = 0;
    if (wledc_segment_virtual_width(seg, &vw) != ESP_OK) return;
    if (wledc_segment_virtual_height(seg, &vh) != ESP_OK) return;
    if (seg->pixels == NULL) return;

    uint16_t phys_w = (uint16_t)(seg->stop - seg->start); /* 行宽=物理宽 */
    uint16_t phys_h = (uint16_t)(seg->stopY - seg->startY);
    bool reverse   = (seg->options & SEG_OPT_REVERSE)   != 0;
    bool reverse_y = (seg->options & SEG_OPT_REVERSE_Y) != 0;
    bool transpose = (seg->options & SEG_OPT_TRANSPOSE) != 0;
    bool mirror    = (seg->options & SEG_OPT_MIRROR)    != 0;
    bool mirror_y  = (seg->options & SEG_OPT_MIRROR_Y)  != 0;

    for (uint16_t cy = 0; cy < vh; cy++) {
        for (uint16_t cx = 0; cx < vw; cx++) {
            uint32_t color = output_mix_color(
                seg->pixels[wledc_2d_xy_index(cx, cy, vw)], bri, cct);

            uint16_t x = cx, y = cy;
            if (reverse)   x = wledc_2d_reverse_x(x, vw);
            if (reverse_y) y = wledc_2d_reverse_y(y, vh);
            if (transpose) {
                uint16_t t = x;
                x = y;
                y = t;
            }

            output_2d_point(seg, buses, phys_w, x, y, color);
            if (mirror) { /* transpose 时镜像轴对调 */
                output_2d_point(seg, buses, phys_w,
                                transpose ? x
                                           : (uint16_t)(phys_w - 1u - x),
                                transpose ? (uint16_t)(phys_h - 1u - y)
                                           : y,
                                color);
            }
            if (mirror_y) {
                output_2d_point(seg, buses, phys_w,
                                transpose ? (uint16_t)(phys_w - 1u - x)
                                           : x,
                                transpose ? y
                                           : (uint16_t)(phys_h - 1u - y),
                                color);
            }
            if (mirror && mirror_y) { /* 对角补点 */
                output_2d_point(seg, buses, phys_w,
                                (uint16_t)(phys_w - 1u - x),
                                (uint16_t)(phys_h - 1u - y), color);
            }
        }
    }
}
#endif /* WLEDC_ENABLE_2D */

/**
 * @brief 段输出分发：取段级视觉亮度/色温，2D 段走虚拟网格路径。
 *
 * 过渡关闭时 current_bri 退化为 on ? opacity : 0、cct 取段当前值
 * （wled_c_transition 同语义，此处直接内联避免空依赖）。
 */
static void output_segment(const wledc_segment_t *seg,
                            const wledc_bus_t *buses)
{
#ifdef WLEDC_ENABLE_TRANSITIONS
    uint8_t bri = wledc_segment_current_bri(seg);
    uint8_t cct = wledc_segment_current_cct(seg);
#else
    uint8_t bri = (seg->options & SEG_OPT_ON) ? seg->opacity : 0;
    uint8_t cct = seg->cct;
#endif
#ifdef WLEDC_ENABLE_2D
    if (wledc_segment_is_2d(seg)) {
        output_segment_2d(seg, buses, bri, cct);
        return;
    }
#endif
    output_segment_1d(seg, buses, bri, cct);
}

/* ------------------------------------------------------------------ */
/* 主循环                                                              */
/* ------------------------------------------------------------------ */

void wledc_strip_service(wledc_time_ms_t now)
{
    uint8_t seg_count = 0;
    uint8_t bus_count = 0;
    wledc_segment_t *segs = wledc_segment_get_all(&seg_count);
    const wledc_bus_t *buses = wledc_bus_get_all(&bus_count);

    /* 帧率门控（WLED service() 同语义，四步见头文件） */
    wledc_time_ms_t elapsed = now - s_last_show;
    bool time_to_show = elapsed >= s_frametime;
    bool can_show = time_to_show || s_triggered
                    || (s_target_fps == WLEDC_FPS_UNLIMITED);
    if (!can_show) {
        return;
    }
    if (elapsed <= WLEDC_MIN_FRAME_DELAY_MS) {
        return; /* 最小帧间隔限流（2ms） */
    }

    /* 活跃段探测（FREEZE 段仍 active，仍参与输出） */
    bool any_active = false;
    for (uint8_t i = 0; i < seg_count; i++) {
        if (segs[i].active) {
            any_active = true;
            break;
        }
    }

    /* 过渡推进：全槽位（含非 active，与 WLED 全量更新一致） */
#ifdef WLEDC_ENABLE_TRANSITIONS
    for (uint8_t i = 0; i < seg_count; i++) {
        wledc_segment_update_transition_progress(&segs[i], now);
    }
#endif

    if (any_active) {
        /* 帧首清屏：覆盖段外/已停用段残留像素 */
        for (uint8_t b = 0; b < bus_count; b++) {
            if (buses[b].active) {
                (void)wledc_bus_clear((wledc_bus_id_t)b, WLEDC_BLACK);
            }
        }
    }

    for (uint8_t i = 0; i < seg_count; i++) {
        wledc_segment_t *seg = &segs[i];
        if (!seg->active) {
            continue;
        }
        if (!(seg->options & SEG_OPT_FREEZE)) {
            /* fx_run 内含 reset_if_required / random palette，勿重复 */
            (void)wledc_fx_run(seg, now + s_timebase);
        }
        output_segment(seg, buses);
    }

    if (any_active) {
        s_last_show = now;
        (void)wledc_bus_show(); /* 单总线跳帧（ESP_ERR_TIMEOUT）不算失败 */
    }
    s_triggered = false;
}

void wledc_strip_trigger(void)
{
    s_triggered = true;
}

bool wledc_strip_triggered(void)
{
    return s_triggered;
}

/* ------------------------------------------------------------------ */
/* 运行时状态                                                          */
/* ------------------------------------------------------------------ */

void wledc_strip_restart_runtime(void)
{
    uint8_t count = 0;
    wledc_segment_t *segs = wledc_segment_get_all(&count);
    for (uint8_t i = 0; i < count; i++) {
        wledc_segment_mark_for_reset(&segs[i]);
        wledc_segment_reset_if_required(&segs[i]);
    }
}

void wledc_strip_set_transition_mode(bool t, wledc_time_ms_t now)
{
#ifdef WLEDC_ENABLE_TRANSITIONS
    uint16_t duration = t ? wledc_transition() : 0;
    uint8_t count = 0;
    wledc_segment_t *segs = wledc_segment_get_all(&count);
    for (uint8_t i = 0; i < count; i++) {
        wledc_segment_start_transition(&segs[i], duration, now);
    }
#else
    (void)t;
    (void)now;
#endif
}

/* ------------------------------------------------------------------ */
/* 时间轴                                                              */
/* ------------------------------------------------------------------ */

void wledc_strip_reset_timebase(wledc_time_ms_t now)
{
    s_timebase = 0 - now; /* 无符号回绕，WLED resetTimebase 同式 */
}

wledc_time_ms_t wledc_strip_now(wledc_time_ms_t now)
{
    return now + s_timebase;
}

/* ------------------------------------------------------------------ */
/* 亮度与帧率                                                          */
/* ------------------------------------------------------------------ */

void wledc_strip_set_brightness(uint8_t b, bool direct)
{
    if (s_brightness == b) {
        return;
    }
    s_brightness = b;

    if (b == 0) {
        /* 亮度归零解冻全部段（WLED setBrightness 同步语义） */
        uint8_t count = 0;
        wledc_segment_t *segs = wledc_segment_get_all(&count);
        for (uint8_t i = 0; i < count; i++) {
            segs[i].options &= ~SEG_OPT_FREEZE;
        }
    }

    /* 全局缩放一次下发总线（段级路径用 current_bri，不重复缩放） */
    uint8_t scaled = wledc_scaled_bri(b);
    uint8_t bus_count = 0;
    wledc_bus_t *buses = wledc_bus_get_all(&bus_count);
    for (uint8_t i = 0; i < bus_count; i++) {
        if (buses[i].active) {
            (void)wledc_bus_set_brightness((wledc_bus_id_t)i, scaled);
        }
    }

    if (!direct) {
        /* 偏差声明：签名无 now，简化为恒触发（WLED 有帧间隔门控） */
        wledc_strip_trigger();
    }
}

uint8_t wledc_strip_brightness(void)
{
    return s_brightness;
}

void wledc_strip_set_target_fps(uint8_t fps)
{
    s_target_fps = fps;
    s_frametime = (fps > 0) ? (uint16_t)(1000u / fps)
                            : WLEDC_MIN_FRAME_DELAY_MS;
}

uint16_t wledc_strip_frametime(void)
{
    return s_frametime;
}