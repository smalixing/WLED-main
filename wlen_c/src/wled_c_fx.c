/*
 * WLED-C 效果引擎:调度、动态调色板解析与随机调色板维护。
 *
 * 重写自 WLED wled00/FX_fcn.cpp(效果调度与调色板管理部分)。
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "wled_c/wled_c_fx.h"
#include "wled_c/wled_c_fx_1d.h"
#include "wled_c/wled_c_fx_2d.h" /* 2D 槽表;头文件内部按 WLEDC_ENABLE_2D 守卫 */

#include <string.h>

#include "wled_c/wled_c_color.h"
#include "wled_c/wled_c_fx_support.h"
#include "wled_c/wled_c_palette.h"

/* ================================================================ */
/* 效果函数表路由:1D 槽 0..76(wled_c_fx_1d.c,任务 9)               */
/* 2D 槽 114..186(wled_c_fx_2d.c,任务 10,WLEDC_ENABLE_2D)          */
/* 未实现槽为 NULL,由 fx_run 渲染黑帧回退                          */
/* ================================================================ */

#define FX_1D_TABLE_LEN wledc_fx_1d_count()
#define FX_2D_FIRST_ID  ((wledc_fx_id_t)114) /* 原生 FX_MODE_2DPLASMAROTOZOOM */

/* ================================================================ */
/* 引擎全局状态                                                      */
/* ================================================================ */

wledc_segment_t *_wledc_current_segment = NULL;

static wledc_time_ms_t s_fx_now; /* 上次 fx_run 时刻,供效果函数 */

/*
 * 统一槽位解析:
 *  - id 0..76    -> 1D 表(索引 = id)
 *  - id 114..186 -> 2D 表(索引 = id - 114,仅 WLEDC_ENABLE_2D 时)
 *  - 其余(77..113 原生空档 / 2D 关闭 / 越界)-> NULL -> 黑帧回退
 */
static wledc_fx_fn fx_slot_lookup(wledc_fx_id_t id)
{
    const wledc_fx_fn *table = wledc_fx_1d_table();
    size_t len = FX_1D_TABLE_LEN;

    if ((size_t)id < len) {
        return table[id];
    }

#ifdef WLEDC_ENABLE_2D
    table = wledc_fx_2d_table();
    len = wledc_fx_2d_count();
    if (id >= FX_2D_FIRST_ID && (size_t)(id - FX_2D_FIRST_ID) < len) {
        return table[id - FX_2D_FIRST_ID];
    }
#endif

    return NULL;
}

size_t wledc_fx_count(void)
{
#ifdef WLEDC_ENABLE_2D
    /* id 空间上界(不含):0..76 连续 + 114..186 连续,77..113 空档。 */
    return (size_t)FX_2D_FIRST_ID + wledc_fx_2d_count();
#else
    return FX_1D_TABLE_LEN;
#endif
}

wledc_fx_fn wledc_fx_get(wledc_fx_id_t id)
{
    return fx_slot_lookup(id);
}

wledc_time_ms_t wledc_fx_now(void)
{
    return s_fx_now;
}

/* ================================================================ */
/* 随机调色板(palette id 1 的数据源)                               */
/* ================================================================ */

#define RANDOM_PAL_INTERVAL_S 5u   /* 默认再生间隔(秒),对齐 WLED */
#define RANDOM_PAL_BLEND_MS  750u  /* 切换混合时长(毫秒) */

static uint8_t s_random_pal[WLEDC_PALETTE_ENTRIES_BYTES];   /* 当前随机调色板 */
static uint8_t s_blend_target[WLEDC_PALETTE_ENTRIES_BYTES]; /* 混合目标 */
static bool s_random_valid;    /* 随机调色板是否已生成 */
static bool s_blend_active;    /* 混合是否进行中 */
static uint16_t s_last_change_s; /* 上次再生时刻(16 位秒,~18.2h 回绕) */
static uint16_t s_interval_s = RANDOM_PAL_INTERVAL_S;

void wledc_fx_set_random_palette_interval(uint16_t seconds)
{
    s_interval_s = (seconds == 0u) ? 1u : seconds;
}

/* 从 48 字节调色板取第 i 槽 RGB 打包为 0x00RRGGBB。 */
static uint32_t pal_entry_rgb(const uint8_t *e, unsigned i)
{
    return ((uint32_t)e[3u * i] << 16) |
           ((uint32_t)e[3u * i + 1u] << 8) |
           (uint32_t)e[3u * i + 2u];
}

/* 写第 i 槽 RGB(丢弃 W 通道:调色板槽为 RGB 三字节)。 */
static void pal_set_entry_rgb(uint8_t *e, unsigned i, uint32_t rgb)
{
    e[3u * i] = WLEDC_R(rgb);
    e[3u * i + 1u] = WLEDC_G(rgb);
    e[3u * i + 2u] = WLEDC_B(rgb);
}

/*
 * 4 锚点(位于槽 0/5/10/15)线性插值展开为 16 槽调色板。
 * 语义对齐 FastLED CRGBPalette16(a, b, c, d)。
 */
static void palette_from_anchors(const uint32_t anchors[4],
                                 uint8_t out[WLEDC_PALETTE_ENTRIES_BYTES])
{
    for (unsigned i = 0; i < 16u; i++) {
        unsigned seg = i / 5u;           /* 锚点序号 0..3 */
        unsigned off = i - seg * 5u;      /* 段内位置 0..4 */
        uint32_t c1 = anchors[seg];
        uint32_t c2 = anchors[(seg < 3u) ? seg + 1u : seg];
        uint8_t blend = (uint8_t)((off * 255u) / 5u);
        pal_set_entry_rgb(out, i, wledc_color_blend(c1, c2, blend));
    }
}

/*
 * 纯随机调色板:4 个高饱和随机色。
 * 重写自 wled00/colors.cpp generateRandomPalette():
 *   4x CHSV(random8(), random8(160,255), random8(128,255))
 */
static void generate_random_palette(void)
{
    uint32_t anchors[4];
    for (int i = 0; i < 4; i++) {
        wledc_hsv32_t hsv = {
            .h = (uint16_t)(wledc_random8() << 8), /* 8 位 hue -> 16 位 */
            .s = wledc_random8_between(160, 255),
            .v = wledc_random8_between(128, 255),
        };
        uint32_t rgbw = 0;
        wledc_hsv2rgb_rainbow(&hsv, &rgbw);
        anchors[i] = rgbw; /* rainbow 输出 W 恒 0 */
    }
    palette_from_anchors(anchors, s_random_pal);
}

/*
 * 和声随机调色板:以当前随机调色板为基色衍生。
 * 重写自 wled00/colors.cpp generateHarmonicRandomPalette()。
 */
static void generate_harmonic_palette(
    const uint8_t base[WLEDC_PALETTE_ENTRIES_BYTES],
    uint8_t out[WLEDC_PALETTE_ENTRIES_BYTES])
{
    wledc_hsv32_t pal[4];
    uint32_t anchors[4];

    /* 随机保留一个基色(锚点即槽 0/5/10/15),hue 加 ±5 抖动。 */
    uint8_t keep = wledc_random8_lim(4);
    for (uint8_t i = 0; i < 4u; i++) {
        uint32_t rgb = pal_entry_rgb(base, i);
        wledc_rgb2hsv(rgb, &pal[i]);
        if (i == keep) {
            pal[i].h = (uint16_t)(pal[i].h +
                (uint16_t)(wledc_random8_between(0, 10) - 5u) * 256u);
        }
    }

    /* 3 个高饱和高亮度 + 1 个低饱和,再 Fisher-Yates 洗牌 s/v。 */
    for (uint8_t i = 0; i < 3u; i++) {
        pal[i].s = wledc_random8_between(200, 255);
        pal[i].v = wledc_random8_between(220, 255);
    }
    pal[3].s = wledc_random8_between(20, 255);
    pal[3].v = wledc_random8_between(80, 255);
    for (int i = 3; i > 0; i--) {
        uint8_t j = wledc_random8_lim((uint8_t)(i + 1));
        uint8_t t = pal[i].s;
        pal[i].s = pal[j].s;
        pal[j].s = t;
        t = pal[i].v;
        pal[i].v = pal[j].v;
        pal[j].v = t;
    }

    /* 5 种和声类型:以保留色 hue 为基准计算 3 个衍生 hue(8 位回绕)。 */
    uint8_t basehue = (uint8_t)(pal[keep].h >> 8);
    uint8_t harm[3];
    switch (wledc_random8_lim(5)) {
    case 0: /* analogous */
        harm[0] = (uint8_t)(basehue + wledc_random8_between(30, 50));
        harm[1] = (uint8_t)(basehue + wledc_random8_between(10, 30));
        harm[2] = (uint8_t)(basehue - wledc_random8_between(10, 30));
        break;
    case 1: /* triadic */
        harm[0] = (uint8_t)(basehue + 113u + wledc_random8_between(0, 15));
        harm[1] = (uint8_t)(basehue + 233u + wledc_random8_between(0, 15));
        harm[2] = (uint8_t)(basehue - 7u + wledc_random8_between(0, 15));
        break;
    case 2: /* split-complementary */
        harm[0] = (uint8_t)(basehue + 145u + wledc_random8_between(0, 10));
        harm[1] = (uint8_t)(basehue + 205u + wledc_random8_between(0, 10));
        harm[2] = (uint8_t)(basehue - 5u + wledc_random8_between(0, 10));
        break;
    case 3: /* square */
        harm[0] = (uint8_t)(basehue + 85u + wledc_random8_between(0, 10));
        harm[1] = (uint8_t)(basehue + 175u + wledc_random8_between(0, 10));
        harm[2] = (uint8_t)(basehue + 265u + wledc_random8_between(0, 10));
        break;
    default: /* tetradic */
        harm[0] = (uint8_t)(basehue + 80u + wledc_random8_between(0, 20));
        harm[1] = (uint8_t)(basehue + 170u + wledc_random8_between(0, 20));
        harm[2] = (uint8_t)(basehue - 15u + wledc_random8_between(0, 30));
        break;
    }
    if (wledc_random8() < 128u) { /* 50% 概率打乱和声顺序 */
        for (int i = 2; i > 0; i--) {
            uint8_t j = wledc_random8_lim((uint8_t)(i + 1));
            uint8_t t = harm[i];
            harm[i] = harm[j];
            harm[j] = t;
        }
    }
    uint8_t k = 0;
    for (uint8_t i = 0; i < 4u; i++) {
        if (i == keep) {
            continue;
        }
        pal[i].h = (uint16_t)(harm[k++] << 8);
    }

    /* ~10% 概率粉彩化(降饱和)。 */
    bool pastel = wledc_random8() < 25u;
    for (uint8_t i = 0; i < 4u; i++) {
        if (pastel && pal[i].s > 180u) {
            pal[i].s = (uint8_t)(pal[i].s - 160u);
        }
        uint32_t rgbw = 0;
        wledc_hsv2rgb_rainbow(&pal[i], &rgbw);
        anchors[i] = rgbw;
    }
    palette_from_anchors(anchors, out);
}

/*
 * 随机调色板维护:周期再生(和声衍生) + 向目标混合。
 * 重写自 wled00/FX_fcn.cpp handleRandomPalette()。
 */
static void handle_random_palette(wledc_time_ms_t now)
{
    uint16_t now_s = (uint16_t)(now / 1000u);

    if (!s_random_valid) {
        generate_random_palette();
        s_random_valid = true;
        s_last_change_s = now_s;
        s_blend_active = false;
        return; /* 首次生成不混合 */
    }

    /* 16 位秒回绕:无符号差比较,~18.2h 周期归零安全。 */
    if ((uint16_t)(now_s - s_last_change_s) > s_interval_s) {
        s_last_change_s = now_s;
        generate_harmonic_palette(s_random_pal, s_blend_target);
        s_blend_active = true;
    }

    if (s_blend_active) {
        uint16_t frames = RANDOM_PAL_BLEND_MS / WLEDC_FRAME_TIME_MS;
        if (frames == 0u) {
            frames = 1u;
        }
        uint8_t max_changes = (frames > 255u)
                                 ? 1u
                                 : (uint8_t)((255u + frames / 2u) / frames);
        if (max_changes == 0u) {
            max_changes = 1u;
        }
        if (wledc_nblend_bytes(s_random_pal, s_blend_target,
                               WLEDC_PALETTE_ENTRIES_BYTES, max_changes)) {
            s_blend_active = false; /* 已收敛 */
        }
    }
}

/* ================================================================ */
/* 段调色板解析(palette id 0-5 动态构造,>=6 透传)                  */
/* ================================================================ */

bool wledc_fx_segment_palette_entries(
    const wledc_segment_t *seg, uint8_t out[WLEDC_PALETTE_ENTRIES_BYTES])
{
    if (seg == NULL) {
        memset(out, 0, WLEDC_PALETTE_ENTRIES_BYTES);
        return false;
    }

    uint32_t c0 = seg->colors[0];
    uint32_t c1 = seg->colors[1];
    uint32_t c2 = seg->colors[2];

    switch (seg->palette) {
    case 0: /* Default:固定表 PartyColors(经 palette 模块透传)。 */
        return wledc_palette_get_entries(6, out);
    case 1: /* Random:随机调色板当前(混合)状态快照。 */
        if (!s_random_valid) {
            generate_random_palette();
            s_random_valid = true;
        }
        memcpy(out, s_random_pal, WLEDC_PALETTE_ENTRIES_BYTES);
        return true;
    case 2: { /* 单色:primary 填满。 */
        uint32_t anchors[4] = {c0, c0, c0, c0};
        palette_from_anchors(anchors, out);
        return true;
    }
    case 3: { /* 两色:CRGBPalette16(p, p, s, s)。 */
        uint32_t anchors[4] = {c0, c0, c1, c1};
        palette_from_anchors(anchors, out);
        return true;
    }
    case 4: { /* 三色循环:(ter, sec, prim) 依序循环。 */
        const uint32_t cyc[3] = {c2, c1, c0};
        for (unsigned i = 0; i < 16u; i++) {
            pal_set_entry_rgb(out, i, cyc[i % 3u]);
        }
        return true;
    }
    case 5: /* 主次色分块(含三级色时三段 + 末槽回主色)。 */
        if (c2 != 0u) {
            for (unsigned i = 0; i < 16u; i++) {
                uint32_t c = (i < 5u) ? c0 : (i < 10u) ? c1
                            : (i < 15u) ? c2
                                        : c0;
                pal_set_entry_rgb(out, i, c);
            }
        } else {
            for (unsigned i = 0; i < 16u; i++) {
                pal_set_entry_rgb(out, i, (i < 8u) ? c0 : c1);
            }
        }
        return true;
    default: /* >=6:固定调色板 / 渐变调色板,由 palette 模块解析。 */
        return wledc_palette_get_entries(seg->palette, out);
    }
}

/* ================================================================ */
/* 效果调度                                                          */
/* ================================================================ */

/* 渲染黑帧:清空段像素缓冲(pixels 为懒分配 0x00RRGGBB 数组)。 */
static void render_black(wledc_segment_t *seg)
{
    if (seg->pixels == NULL) {
        return;
    }
    for (uint16_t i = 0; i < seg->pixels_len; i++) {
        seg->pixels[i] = 0u;
    }
}

uint16_t wledc_fx_run(wledc_segment_t *seg, wledc_time_ms_t now)
{
    s_fx_now = now;

    if (seg == NULL) {
        return WLEDC_FRAME_TIME_MS;
    }

    /* 调度序对齐 design.md 3.5:随机调色板 -> 运行时重置 -> 效果函数。 */
    handle_random_palette(now);
    wledc_segment_reset_if_required(seg);

    wledc_fx_fn fx = fx_slot_lookup((wledc_fx_id_t)seg->mode);

    if (fx == NULL) {
        render_black(seg); /* 越界 / 未注册槽:安全黑帧 */
        return WLEDC_FRAME_TIME_MS;
    }

    _wledc_current_segment = seg;
    uint16_t delay = fx(seg);

    /* 0 = 默认帧时;同时封顶防坏效果卡死调度器。 */
    if (delay == 0u || delay > WLEDC_FRAME_TIME_MS) {
        return WLEDC_FRAME_TIME_MS;
    }
    return delay;
}