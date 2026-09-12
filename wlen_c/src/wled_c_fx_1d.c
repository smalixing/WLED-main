/*
 * wled_c_fx_1d.c — WLED 1D 效果移植(纯 C,ESP-IDF 风格)
 *
 * 逐函数对齐 wled00/FX.cpp 原生实现,移植规则见 wled_c_fx_1d.h:
 *   1) 段 pixels 为物理缓冲(length = stop - start);三件套写入时内联折叠
 *      (reverse -> grouping 展开 -> mirror),对齐 FX_fcn.cpp
 *      blendSegment/setMirroredPixel(L1694-1751)。
 *   2) fill / fade_out / fadeToBlackBy / fadeToSecondaryBy / blur 遍历
 *      物理像素(对齐原生 raw 遍历语义)。
 *   3) request_data 失败 -> 回退 FX_FALLBACK_STATIC(填 SEGCOLOR(0))。
 *   4) 原生 bug 注释后原样复刻。
 *   5) 1D 移植忽略 offset。
 *
 * 帧约定:效果函数返回 0 = 使用默认帧时间(WLEDC_FRAME_TIME_MS)。
 * 效果循环域 SEGLEN = 虚拟长度(FX.h L112:#define SEGLEN Segment::vLength())。
 * 原生 check1/2/3 位域宿主段结构不提供,按 WLED UI 默认值固化。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wled_c/wled_c_fx_1d.h"
#include "wled_c/wled_c_color.h"
#include "wled_c/wled_c_fx.h"
#include "wled_c/wled_c_fx_support.h"
#include "wled_c/wled_c_palette.h"
#include "wled_c/wled_c_segment.h"

/* ============================ 内部宏 ============================ */

#define FX1D_RAWLEN(seg)  ((uint16_t)((seg)->stop - (seg)->start))
#define FX1D_OPT(seg, f)  (((seg)->options & (f)) != 0)

/* 原生 paletteBlend 宏(FX.cpp L70-72),移植映射到段字段 blend_mode:
 * paletteBlend: 0=移动时回绕混合;1=总是回绕;2=从不回绕;3=不混合 */
#define FX1D_SOLID_WRAP(seg)  ((seg)->blend_mode == 1 || (seg)->blend_mode == 3)
#define FX1D_MOVING_WRAP(seg) \
    (!((seg)->blend_mode == 2 || ((seg)->blend_mode == 0 && (seg)->speed == 0)))

/* ============================ 三件套 ============================ */

/* 虚拟长度(FX_fcn.cpp virtualLength L744-747 直译) */
static uint16_t fx1d_virtual_length(const wledc_segment_t *seg)
{
    uint16_t length   = FX1D_RAWLEN(seg);
    uint16_t group_len = (uint16_t)(seg->grouping + seg->spacing);
    if (group_len == 0) group_len = 1; /* UI 约束 grouping>=1,防御性兜底 */
    uint16_t v_length = (uint16_t)((length + group_len - 1) / group_len); /* ceil */
    if (FX1D_OPT(seg, WLEDC_SEG_OPT_MIRROR)) {
        v_length = (uint16_t)((v_length + 1) / 2);
    }
    return v_length;
}

/* 写像素:写入时折叠(FX_fcn.cpp blendSegment L1744-1751 + setMirroredPixel
 * L1700-1714 直译;offset 回绕按移植规则⑤忽略,opacity 过渡不施加)
 * setMirroredPixel:主索引 = start+i(私缓冲 0 基 = i);镜像 = stop-i-1 */
static void fx1d_set_pixel(wledc_segment_t *seg, uint16_t k, uint32_t c)
{
    uint16_t n_len = fx1d_virtual_length(seg);
    if (seg->pixels == NULL || k >= n_len) return; /* 越界静默忽略(原生语义) */

    uint16_t length    = FX1D_RAWLEN(seg);
    uint16_t group_len = (uint16_t)(seg->grouping + seg->spacing);
    if (group_len == 0) group_len = 1;

    uint16_t i = k;
    if (FX1D_OPT(seg, WLEDC_SEG_OPT_REVERSE)) i = (uint16_t)(n_len - i - 1);

    uint32_t t     = (uint32_t)i * group_len + seg->grouping;
    uint16_t max_i = (t < length) ? (uint16_t)t : length;

    while (i < max_i) {
        if (FX1D_OPT(seg, WLEDC_SEG_OPT_MIRROR)) {
            seg->pixels[(uint16_t)(length - 1 - i)] = c;
        }
        seg->pixels[i] = c;
        i++;
    }
}

/* 读像素(FX_fcn.cpp getPixelColor L991-1046 直译:1D 无折叠,raw 直读) */
static uint32_t fx1d_get_pixel(const wledc_segment_t *seg, uint16_t i)
{
    if (!seg->active || seg->pixels == NULL) return 0;
    if (i >= fx1d_virtual_length(seg)) return 0;
    return seg->pixels[i]; /* vLength <= rawLen,索引安全 */
}

/* ============================ helpers(直译) ============================ */

/* fill(FX_fcn.cpp L1086-1089 直译:raw 全填) */
static void fx1d_fill(wledc_segment_t *seg, uint32_t c)
{
    if (seg->pixels == NULL) return;
    for (uint16_t i = 0; i < FX1D_RAWLEN(seg); i++) seg->pixels[i] = c;
}

/* fade_out(FX_fcn.cpp L1096-1117 直译:向 colors[1] 衰减,raw 遍历) */
static void fx1d_fade_out(wledc_segment_t *seg, uint8_t rate)
{
    if (seg->pixels == NULL) return;
    uint8_t r           = (uint8_t)((256u - rate) >> 1);
    int     mapped_rate = 256 / (r + 1);
    uint32_t bg         = seg->colors[1];
    for (uint16_t j = 0; j < FX1D_RAWLEN(seg); j++) {
        uint32_t color = seg->pixels[j];
        if (color == bg) continue; /* 已达目标色 */
        for (int i = 0; i < 32; i += 8) { /* 4 通道(含 W) */
            uint8_t c2 = (uint8_t)(bg >> i);    /* 背景通道 */
            uint8_t c1 = (uint8_t)(color >> i); /* 前景通道 */
            int delta = (c2 - c1) * mapped_rate / 256;
            /* 衰减未完成时 delta 至少 ±1(修正舍入) */
            if (delta == 0) delta += (c2 == c1) ? 0 : (c2 > c1) ? 1 : -1;
            color &= ~((uint32_t)0xFF << i);
            color |= ((uint32_t)((uint8_t)(c1 + delta))) << i;
        }
        seg->pixels[j] = color;
    }
}

/* fadeToSecondaryBy(FX_fcn.cpp L1120-1124 直译) */
static void fx1d_fade_to_secondary_by(wledc_segment_t *seg, uint8_t fade_by)
{
    if (seg->pixels == NULL || fade_by == 0) return;
    for (uint16_t i = 0; i < FX1D_RAWLEN(seg); i++) {
        seg->pixels[i] = wledc_color_blend(seg->pixels[i], seg->colors[1], fade_by);
    }
}

/* fadeToBlackBy(FX_fcn.cpp L1127-1131 直译) */
static void fx1d_fade_to_black_by(wledc_segment_t *seg, uint8_t fade_by)
{
    if (seg->pixels == NULL || fade_by == 0) return;
    for (uint16_t i = 0; i < FX1D_RAWLEN(seg); i++) {
        seg->pixels[i] =
            wledc_fast_color_scale(seg->pixels[i], (uint8_t)(255 - fade_by));
    }
}

/* blur(FX_fcn.cpp L1137-1163 直译;宿主调用方均未传 smear,固化 false)
 * raw 直操作(i < vLength,无折叠);FastLED 盲渗式:首像素单独处理
 * 避免循环内分支,渗出 part 计入 i-1,carryover 链式向右传递 */
static void fx1d_blur(wledc_segment_t *seg, uint8_t blur_amount)
{
    if (blur_amount == 0 || !seg->active || seg->pixels == NULL) return;

    const uint8_t keep = (uint8_t)(255 - blur_amount);
    const uint8_t seep = blur_amount >> 1;
    const uint16_t vlength = fx1d_virtual_length(seg);

    /* handle first pixel to avoid conditional in loop (faster) */
    uint32_t cur = seg->pixels[0];
    uint32_t carryover = wledc_fast_color_scale(cur, seep);
    seg->pixels[0] = wledc_fast_color_scale(cur, keep);

    for (uint16_t i = 1; i < vlength; i++) {
        cur = seg->pixels[i];
        uint32_t part = wledc_fast_color_scale(cur, seep);
        cur = wledc_fast_color_scale(cur, keep);
        cur = wledc_color_add(cur, carryover, false);
        seg->pixels[i - 1] =
            wledc_color_add(seg->pixels[i - 1], part, false); /* previous pixel */
        seg->pixels[i] = cur; /* current pixel */
        carryover = part;
    }
}

/* 前向声明(color_wheel 调用点在定义之前) */
static uint32_t fx1d_seg_palette_color(wledc_segment_t *seg, uint16_t i,
                                       bool mapping, bool moving,
                                       uint8_t mcol, uint8_t pbri);

/* color_wheel(FX_fcn.cpp L1170-1176 直译) */
static uint32_t fx1d_color_wheel(wledc_segment_t *seg, uint8_t pos)
{
    if (seg->palette) {
        return fx1d_seg_palette_color(seg, pos, false, true, 0, 255);
    }
    wledc_hsv32_t hsv = { (uint16_t)((uint16_t)pos << 8), 255, 255 };
    uint32_t rgbw;
    wledc_hsv2rgb_rainbow(&hsv, &rgbw);
    /* 白通道取自 color0(L1174) */
    rgbw = (rgbw & 0x00FFFFFFu) | ((uint32_t)WLEDC_W(seg->colors[0]) << 24);
    return rgbw;
}

/* seg_palette_color(FX_fcn.cpp color_from_palette L1187-1208 直译)
 * 签名对齐原生 (i, mapping, moving, mcol=0, pbri=255);
 * !isRGB 分支省略(逻辑缓冲恒 RGB(W));palette==0 && mcol<3 特例
 * 返回 color_fade(getCurrentColor(mcol), pbri, true) */
static uint32_t fx1d_seg_palette_color(wledc_segment_t *seg, uint16_t i,
                                       bool mapping, bool moving,
                                       uint8_t mcol, uint8_t pbri)
{
    uint32_t color = seg->colors[(mcol < WLEDC_NUM_COLORS) ? mcol : 0];

    if (seg->palette == 0 && mcol < WLEDC_NUM_COLORS) {
        return wledc_color_fade(color, pbri, true);
    }

    uint16_t v_length = fx1d_virtual_length(seg);
    if (v_length == 0) return wledc_color_fade(color, pbri, true); /* 防御 */

    uint32_t t = ((uint32_t)i * 255u) / v_length;
    uint8_t palette_index =
        mapping ? (uint8_t)((t > 255u) ? 255u : t) : (uint8_t)i;

    wledc_blend_t blend; /* paletteBlend 三分支(L1196-1200) */
    switch (seg->blend_mode) {
    case 1:  blend = WLEDC_BLEND_LINEAR;         break; /* 总是回绕 */
    case 2:  blend = WLEDC_BLEND_LINEAR_NOWRAP;  break; /* 从不回绕 */
    case 3:  blend = WLEDC_BLEND_NONE;           break; /* 不混合 */
    default: blend = moving ? WLEDC_BLEND_LINEAR : WLEDC_BLEND_LINEAR_NOWRAP; break;
    }

    uint8_t entries[WLEDC_PALETTE_ENTRIES_BYTES];
    if (!wledc_fx_segment_palette_entries(seg, entries)) {
        return wledc_color_fade(color, pbri, true); /* 取板失败兜底 */
    }
    uint32_t palcol = wledc_color_from_palette(entries, palette_index, pbri, blend);
    /* 白通道保留自 mcol 色(L1207) */
    palcol = (palcol & 0x00FFFFFFu) | ((uint32_t)WLEDC_W(color) << 24);
    return palcol;
}

/* ============================ 批1 效果 ============================ */

/* mode_static(FX.cpp L136 直译) [槽0] */
static uint16_t fx_1d_static(wledc_segment_t *seg)
{
    fx1d_fill(seg, seg->colors[0]); /* SEGMENT.fill(SEGCOLOR(0)) */
    return 0;
}

/* mode_blink 核心(FX.cpp L195-218 直译) */
static uint16_t fx1d_blink_core(wledc_segment_t *seg, uint32_t color1,
                                uint32_t color2, bool strobe, bool do_palette)
{
    uint32_t now       = wledc_fx_now();
    uint32_t cycle_time = (uint32_t)(255 - seg->speed) * 20;
    uint32_t on_time   = WLEDC_FRAME_TIME_MS;
    if (!strobe) on_time += (cycle_time * seg->intensity) >> 8;
    cycle_time += (uint32_t)WLEDC_FRAME_TIME_MS * 2;
    uint32_t it  = now / cycle_time;
    uint32_t rem = now % cycle_time;
    bool on = (it != seg->step) || (rem <= on_time);
    seg->step = it;
    uint32_t color = on ? color1 : color2;
    if (color == color1 && do_palette) {
        for (uint16_t i = 0; i < fx1d_virtual_length(seg); i++) {
            fx1d_set_pixel(seg, i,
                fx1d_seg_palette_color(seg, i, true, FX1D_SOLID_WRAP(seg), 0, 255));
        }
    } else {
        fx1d_fill(seg, color);
    }
    return 0;
}

/* mode_blink(FX.cpp L224 直译) [槽1] */
static uint16_t fx_1d_blink(wledc_segment_t *seg)
{
    return fx1d_blink_core(seg, seg->colors[0], seg->colors[1], false, true);
}

/* mode_blink_rainbow(FX.cpp L234 直译) [槽26] */
static uint16_t fx_1d_blink_rainbow(wledc_segment_t *seg)
{
    return fx1d_blink_core(seg,
        fx1d_color_wheel(seg, (uint8_t)(seg->call & 0xFF)),
        seg->colors[1], false, false);
}

/* mode_strobe(FX.cpp L242 直译) [槽23] */
static uint16_t fx_1d_strobe(wledc_segment_t *seg)
{
    return fx1d_blink_core(seg, seg->colors[0], seg->colors[1], true, true);
}

/* mode_strobe_rainbow(FX.cpp L251 直译) [槽24] */
static uint16_t fx_1d_strobe_rainbow(wledc_segment_t *seg)
{
    return fx1d_blink_core(seg,
        fx1d_color_wheel(seg, (uint8_t)(seg->call & 0xFF)),
        seg->colors[1], true, false);
}

/* mode_multi_strobe(FX.cpp L826-847 直译) [槽25] */
static uint16_t fx_1d_multi_strobe(wledc_segment_t *seg)
{
    uint32_t now    = wledc_fx_now();
    uint16_t seglen = fx1d_virtual_length(seg);
    for (uint16_t i = 0; i < seglen; i++) { /* 背景 mcol=1 */
        fx1d_set_pixel(seg, i,
            fx1d_seg_palette_color(seg, i, true, FX1D_SOLID_WRAP(seg), 1, 255));
    }
    seg->aux0 = (uint16_t)(50 + 20 * (255 - seg->speed)); /* 先无条件设基线 */
    unsigned count = 2 * ((seg->intensity / 10) + 1);
    if (seg->aux1 < count) {
        if ((seg->aux1 & 1) == 0) {
            fx1d_fill(seg, seg->colors[0]); /* 偶步 fill 覆盖背景(原生行为) */
            seg->aux0 = 15;
        } else {
            seg->aux0 = 50;
        }
    }
    if (now - seg->aux0 > seg->step) {
        seg->aux1++;
        if (seg->aux1 > count) seg->aux1 = 0;
        seg->step = now;
    }
    return 0;
}

/* ============================ 批2 效果 ============================ */

/* ---- 本地助手(fx_support.h 未提供,原生直译) ---- */

/* sin_gap(FX.cpp L90-93 直译) */
static uint8_t fx1d_sin_gap(uint16_t in)
{
    if (in & 0x100) return 0;
    return wledc_sin8((uint8_t)(in + 192));
}

/* get_random_wheel_index(util.cpp L748-756 直译:色环距离 >= 42) */
static uint8_t fx1d_random_wheel_index(uint8_t pos)
{
    uint8_t r = 0, x = 0, y = 0, d = 0;
    while (d < 42) {
        r = wledc_random8();
        x = (uint8_t)((pos > r) ? (pos - r) : (r - pos)); /* abs(pos-r) */
        y = (uint8_t)(255 - x);
        d = (x < y) ? x : y;
    }
    return r;
}

/* map(Arduino map 直译,long -> int32_t) */
static int32_t fx1d_map(int32_t x, int32_t in_min, int32_t in_max,
                        int32_t out_min, int32_t out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

/* mode_breath(FX.cpp L432-446 直译) [槽2] */
static uint16_t fx_1d_breath(wledc_segment_t *seg)
{
    uint32_t var = 0;
    uint32_t now = wledc_fx_now();
    uint32_t counter = (now * ((seg->speed >> 3) + 10u)) & 0xFFFFu;
    counter = (counter >> 2) + (counter >> 4);
    if (counter < 16384) {
        if (counter > 8192) counter = 8192 - (counter - 8192);
        var = (uint32_t)(wledc_sin16((uint16_t)counter) / 103);
    }
    uint8_t lum = (uint8_t)(30 + var);
    uint16_t seglen = fx1d_virtual_length(seg);
    for (uint16_t i = 0; i < seglen; i++) {
        fx1d_set_pixel(seg, i, wledc_color_blend(seg->colors[1],
            fx1d_seg_palette_color(seg, i, true, FX1D_SOLID_WRAP(seg), 0, 255), lum));
    }
    return 0;
}

/* mode_color_wipe 核心(FX.cpp L262-310 直译) */
static void fx1d_color_wipe_core(wledc_segment_t *seg, bool rev,
                                 bool use_random_colors)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    if (seglen <= 1) { /* FX_FALLBACK_STATIC */
        fx1d_fill(seg, seg->colors[0]);
        return;
    }
    uint32_t now = wledc_fx_now();
    uint32_t cycle_time = 750 + (uint32_t)(255 - seg->speed) * 150;
    uint32_t perc = now % cycle_time;
    uint32_t prog = (perc * 65535u) / cycle_time;
    bool back = prog > 32767u;
    if (back) {
        prog -= 32767u;
        if (seg->step == 0) seg->step = 1;
    } else {
        if (seg->step == 2) seg->step = 3;
    }
    if (use_random_colors) {
        if (seg->call == 0) {
            seg->aux0 = wledc_random8();
            seg->step = 3;
        }
        if (seg->step == 1) {
            seg->aux1 = fx1d_random_wheel_index((uint8_t)seg->aux0);
            seg->step = 2;
        }
        if (seg->step == 3) {
            seg->aux0 = fx1d_random_wheel_index((uint8_t)seg->aux1);
            seg->step = 0;
        }
    }
    uint32_t led_index = (prog * seglen) >> 15;
    uint16_t rem = (uint16_t)((prog * seglen) * 2u); /* 乘后截 uint16(原生) */
    rem = (uint16_t)(rem / (uint16_t)(seg->intensity + 1u));
    if (rem > 255) rem = 255;

    uint32_t col1 = use_random_colors
        ? fx1d_color_wheel(seg, (uint8_t)seg->aux1)
        : seg->colors[1];
    for (uint32_t i = 0; i < seglen; i++) {
        /* 非随机分支调色板取色用镜像后 index(原生) */
        uint32_t index = (rev && back) ? (uint32_t)(seglen - 1u - i) : i;
        uint32_t col0 = use_random_colors
            ? fx1d_color_wheel(seg, (uint8_t)seg->aux0)
            : fx1d_seg_palette_color(seg, (uint16_t)index, true,
                                     FX1D_SOLID_WRAP(seg), 0, 255);
        if (i < led_index) {
            fx1d_set_pixel(seg, (uint16_t)index, back ? col1 : col0);
        } else {
            fx1d_set_pixel(seg, (uint16_t)index, back ? col0 : col1);
            if (i == led_index) {
                fx1d_set_pixel(seg, (uint16_t)index,
                    wledc_color_blend(back ? col0 : col1, back ? col1 : col0,
                                      (uint8_t)rem));
            }
        }
    }
}

/* mode_color_wipe(FX.cpp L316-347 直译) [槽3] */
static uint16_t fx_1d_color_wipe(wledc_segment_t *seg)
{
    fx1d_color_wipe_core(seg, false, false);
    return 0;
}

/* mode_color_wipe_random(FX.cpp L316-347 直译) [槽4] */
static uint16_t fx_1d_color_wipe_random(wledc_segment_t *seg)
{
    fx1d_color_wipe_core(seg, false, true);
    return 0;
}

/* mode_color_sweep(FX.cpp L316-347 直译) [槽6] */
static uint16_t fx_1d_color_sweep(wledc_segment_t *seg)
{
    fx1d_color_wipe_core(seg, true, false);
    return 0;
}

/* mode_color_sweep_random(FX.cpp L316-347 直译) [槽36] */
static uint16_t fx_1d_color_sweep_random(wledc_segment_t *seg)
{
    fx1d_color_wipe_core(seg, true, true);
    return 0;
}

/* mode_random_color(FX.cpp L354-378 直译) [槽5] */
static uint16_t fx_1d_random_color(wledc_segment_t *seg)
{
    uint32_t now = wledc_fx_now();
    uint32_t cycle_time = 200 + (uint32_t)(255 - seg->speed) * 50;
    uint32_t it = now / cycle_time;
    uint32_t rem = now % cycle_time;
    uint32_t fadedur = (cycle_time * seg->intensity) >> 8;

    uint32_t fade = 255;
    if (fadedur != 0) {
        fade = (rem * 255u) / fadedur;
        if (fade > 255) fade = 255;
    }
    if (seg->call == 0) {
        seg->aux0 = wledc_random8();
        seg->step = 2;
    }
    if (it != seg->step) {
        seg->aux1 = seg->aux0;
        seg->aux0 = fx1d_random_wheel_index((uint8_t)seg->aux0);
        seg->step = it;
    }
    fx1d_fill(seg, wledc_color_blend(
        fx1d_color_wheel(seg, (uint8_t)seg->aux1),
        fx1d_color_wheel(seg, (uint8_t)seg->aux0),
        (uint8_t)fade));
    return 0;
}

/* mode_dynamic(FX.cpp L386-413 直译) [槽7]
 * check1(Smooth,UI 默认 false)固化 -> setPixelColor 分支 */
static uint16_t fx_1d_dynamic(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    uint8_t *data = wledc_segment_request_data(seg, seglen);
    if (data == NULL) { /* FX_FALLBACK_STATIC */
        fx1d_fill(seg, seg->colors[0]);
        return 0;
    }
    if (seg->call == 0) {
        for (uint16_t i = 0; i < seglen; i++) data[i] = wledc_random8();
    }
    uint32_t now = wledc_fx_now();
    uint32_t cycle_time = 50 + (uint32_t)(255 - seg->speed) * 15;
    uint32_t it = now / cycle_time;
    if (it != seg->step && seg->speed != 0) {
        for (uint16_t i = 0; i < seglen; i++) {
            if (wledc_random8() <= seg->intensity) data[i] = wledc_random8();
        }
        seg->step = it;
    }
    for (uint16_t i = 0; i < seglen; i++) {
        fx1d_set_pixel(seg, i, fx1d_color_wheel(seg, data[i]));
    }
    return 0;
}

/* mode_rainbow(FX.cpp L514-523 直译) [槽8] */
static uint16_t fx_1d_rainbow(wledc_segment_t *seg)
{
    uint32_t now = wledc_fx_now();
    uint32_t counter = (now * ((seg->speed >> 2) + 2u)) & 0xFFFFu;
    counter >>= 8;
    if (seg->intensity < 128) {
        fx1d_fill(seg, wledc_color_blend(
            fx1d_color_wheel(seg, (uint8_t)counter),
            0x00FFFFFFu, /* WHITE(FX.h L121) */
            (uint8_t)(128 - seg->intensity)));
    } else {
        fx1d_fill(seg, fx1d_color_wheel(seg, (uint8_t)counter));
    }
    return 0;
}

/* mode_rainbow_cycle(FX.cpp L530-539 直译) [槽9] */
static uint16_t fx_1d_rainbow_cycle(wledc_segment_t *seg)
{
    uint32_t now = wledc_fx_now();
    uint32_t counter = (now * ((seg->speed >> 2) + 2u)) & 0xFFFFu;
    counter >>= 8;
    uint16_t seglen = fx1d_virtual_length(seg);
    if (seglen == 0) return 0;
    for (uint16_t i = 0; i < seglen; i++) {
        uint8_t index = (uint8_t)((i * (16u << (seg->intensity / 29)) / seglen) + counter);
        fx1d_set_pixel(seg, i, fx1d_color_wheel(seg, index));
    }
    return 0;
}

/* mode_fade(FX.cpp L453-460 直译) [槽12]
 * counter 无 &0xFFFF,经 triwave16(uint16_t) 隐式截位(原生复刻) */
static uint16_t fx_1d_fade(wledc_segment_t *seg)
{
    uint32_t now = wledc_fx_now();
    uint32_t counter = now * ((seg->speed >> 3) + 10u);
    uint8_t lum = wledc_triwave16((uint16_t)counter) >> 8;
    uint16_t seglen = fx1d_virtual_length(seg);
    for (uint16_t i = 0; i < seglen; i++) {
        fx1d_set_pixel(seg, i, wledc_color_blend(
            seg->colors[1],
            fx1d_seg_palette_color(seg, i, true, FX1D_SOLID_WRAP(seg), 0, 255),
            lum));
    }
    return 0;
}

/* scan(FX.cpp L467-490 直译)
 * check2(背景填充,UI 默认 false)固化 -> false 分支 */
static void fx1d_scan_core(wledc_segment_t *seg, bool dual)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    if (seglen <= 1) { /* FX_FALLBACK_STATIC */
        fx1d_fill(seg, seg->colors[0]);
        return;
    }
    uint32_t now = wledc_fx_now();
    uint32_t cycle_time = 750 + (uint32_t)(255 - seg->speed) * 150;
    uint32_t perc = now % cycle_time;
    int32_t prog = (int32_t)((perc * 65535u) / cycle_time);
    int32_t size = 1 + (int32_t)(((uint32_t)seg->intensity * seglen) >> 9);
    int32_t led_index = (prog * ((int32_t)seglen * 2 - size * 2)) >> 16;

    /* if (!SEGMENT.check2) */ fx1d_fill(seg, seg->colors[1]);

    int32_t led_offset = led_index - (int32_t)(seglen - size);
    if (led_offset < 0) led_offset = -led_offset; /* abs */

    if (dual) {
        for (int32_t j = led_offset; j < led_offset + size; j++) {
            uint16_t i2 = (uint16_t)(seglen - 1 - j);
            fx1d_set_pixel(seg, i2, fx1d_seg_palette_color(seg, i2, true,
                FX1D_SOLID_WRAP(seg), (seg->colors[2] != 0) ? 2 : 0, 255));
        }
    }
    for (int32_t j = led_offset; j < led_offset + size; j++) {
        fx1d_set_pixel(seg, (uint16_t)j, fx1d_seg_palette_color(seg,
            (uint16_t)j, true, FX1D_SOLID_WRAP(seg), 0, 255));
    }
}

/* mode_scan(FX.cpp L496-498 直译) [槽10] */
static uint16_t fx_1d_scan(wledc_segment_t *seg)
{
    fx1d_scan_core(seg, false);
    return 0;
}

/* mode_dual_scan(FX.cpp L505-507 直译) [槽11] */
static uint16_t fx_1d_dual_scan(wledc_segment_t *seg)
{
    fx1d_scan_core(seg, true);
    return 0;
}

/* running(FX.cpp L546-568 直译)
 * 原生复刻:usePalette 时循环内重取 color1(勿提出循环) */
static void fx1d_running_core(wledc_segment_t *seg, uint32_t color1,
                              uint32_t color2, bool theatre)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    uint32_t width = (uint32_t)(theatre ? 3 : 1) + (seg->intensity >> 4); /* window */
    uint32_t cycle_time = 50 + (uint32_t)(255 - seg->speed);
    uint32_t now = wledc_fx_now();
    uint32_t it = now / cycle_time;
    bool use_palette = (color1 == seg->colors[0]);

    for (uint16_t i = 0; i < seglen; i++) {
        uint32_t col = color2;
        if (use_palette) {
            color1 = fx1d_seg_palette_color(seg, i, true, FX1D_SOLID_WRAP(seg), 0, 255);
        }
        if (theatre) {
            if ((i % width) == seg->aux0) col = color1;
        } else {
            uint32_t pos = i % (width << 1);
            if ((pos < seg->aux0 - width) ||
                ((pos >= seg->aux0) && (pos < seg->aux0 + width))) col = color1;
        }
        fx1d_set_pixel(seg, i, col);
    }

    if (it != seg->step) {
        seg->aux0 = (uint16_t)((seg->aux0 + 1) % (theatre ? width : (width << 1)));
        seg->step = it;
    }
}

/* mode_theater_chase(FX.cpp L575-577 直译) [槽13] */
static uint16_t fx_1d_theater_chase(wledc_segment_t *seg)
{
    fx1d_running_core(seg, seg->colors[0], seg->colors[1], true);
    return 0;
}

/* mode_theater_chase_rainbow(FX.cpp L585-587 直译) [槽14]
 * color1 取 color_wheel(step) */
static uint16_t fx_1d_theater_chase_rainbow(wledc_segment_t *seg)
{
    fx1d_running_core(seg, fx1d_color_wheel(seg, (uint8_t)seg->step),
                      seg->colors[1], true);
    return 0;
}

/* running_base(FX.cpp L594-620 直译)
 * unsigned a/b 回绕;saw 用 fx1d_map */
static void fx1d_running_base(wledc_segment_t *seg, bool saw, bool dual)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    uint32_t x_scale = seg->intensity >> 2;
    uint32_t now = wledc_fx_now();
    uint32_t counter = (now * seg->speed) >> 9;

    for (uint16_t i = 0; i < seglen; i++) {
        uint32_t a = i * x_scale - counter; /* unsigned 回绕(原生) */
        if (saw) {
            a &= 0xFF;
            if (a < 16) {
                a = 192 + a * 8;
            } else {
                a = (uint32_t)fx1d_map((int32_t)a, 16, 255, 64, 192);
            }
            a = 255 - a;
        }
        uint8_t s = dual ? fx1d_sin_gap((uint16_t)a) : wledc_sin8((uint8_t)a);
        uint32_t ca = wledc_color_blend(seg->colors[1],
            fx1d_seg_palette_color(seg, i, true, FX1D_SOLID_WRAP(seg), 0, 255), s);
        if (dual) {
            uint32_t b = (uint32_t)(seglen - 1 - i) * x_scale - counter;
            uint8_t t = fx1d_sin_gap((uint16_t)b);
            uint32_t cb = wledc_color_blend(seg->colors[1],
                fx1d_seg_palette_color(seg, i, true, FX1D_SOLID_WRAP(seg), 2, 255), t);
            ca = wledc_color_blend(ca, cb, 127);
        }
        fx1d_set_pixel(seg, i, ca);
    }
}

/* mode_running_dual(FX.cpp L627-629 直译) [槽52] */
static uint16_t fx_1d_running_dual(wledc_segment_t *seg)
{
    fx1d_running_base(seg, false, true);
    return 0;
}

/* mode_running_lights(FX.cpp L636-638 直译) [槽15] */
static uint16_t fx_1d_running_lights(wledc_segment_t *seg)
{
    fx1d_running_base(seg, false, false);
    return 0;
}

/* mode_saw(FX.cpp L645-647 直译) [槽16] */
static uint16_t fx_1d_saw(wledc_segment_t *seg)
{
    fx1d_running_base(seg, true, false);
    return 0;
}

/* ------------------------- 批 3:chase 系(L890-1285) ------------------------- */

/*
 * chase 核心(FX.cpp L895-957 直译)
 * color1=背景色;color2/color3=相邻两灯颜色
 * 原生 chase_random 依赖 SEGMENT.mode,此处参数化
 */
static void fx1d_chase_core(wledc_segment_t *seg, uint32_t color1,
                            uint32_t color2, uint32_t color3,
                            bool do_palette, bool chase_random)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    uint32_t now = wledc_fx_now();
    uint16_t counter = (uint16_t)(now * ((seg->speed >> 2) + 1));
    uint16_t a = (uint16_t)((counter * seglen) >> 16);

    if (chase_random) {
        if (a < (uint16_t)seg->step) { /* 回到起点,选新随机色 */
            seg->aux1 = seg->aux0;    /* 保存上一随机色 */
            seg->aux0 = fx1d_random_wheel_index(seg->aux0);
        }
        color1 = fx1d_color_wheel(seg, (uint8_t)seg->aux0);
    }
    seg->step = a;

    /* intensity 控制追逐长度,最高 1/2 段长 */
    unsigned size = 1 + ((seg->intensity * seglen) >> 10);

    uint16_t b = a + size; /* 尾迹,填 color1 */
    if (b > seglen) b -= seglen;
    uint16_t c = b + size;
    if (c > seglen) c -= seglen;

    /* 背景 */
    if (do_palette) {
        for (unsigned i = 0; i < seglen; i++) {
            fx1d_set_pixel(seg, (uint16_t)i,
                fx1d_seg_palette_color(seg, (uint16_t)i, true,
                                       FX1D_SOLID_WRAP(seg), 1, 255));
        }
    } else {
        fx1d_fill(seg, color1);
    }

    /* random:用旧背景色填 a 至段尾 */
    if (chase_random) {
        color1 = fx1d_color_wheel(seg, (uint8_t)seg->aux1);
        for (unsigned i = a; i < seglen; i++) {
            fx1d_set_pixel(seg, (uint16_t)i, color1);
        }
    }

    /* a..b 填 color2 */
    if (a < b) {
        for (unsigned i = a; i < b; i++)
            fx1d_set_pixel(seg, (uint16_t)i, color2);
    } else {
        for (unsigned i = a; i < seglen; i++) /* 填至段尾 */
            fx1d_set_pixel(seg, (uint16_t)i, color2);
        for (unsigned i = 0; i < b; i++) /* 从段首填至 b */
            fx1d_set_pixel(seg, (uint16_t)i, color2);
    }

    /* b..c 填 color3 */
    if (b < c) {
        for (unsigned i = b; i < c; i++)
            fx1d_set_pixel(seg, (uint16_t)i, color3);
    } else {
        for (unsigned i = b; i < seglen; i++) /* 填至段尾 */
            fx1d_set_pixel(seg, (uint16_t)i, color3);
        for (unsigned i = 0; i < c; i++) /* 从段首填至 c */
            fx1d_set_pixel(seg, (uint16_t)i, color3);
    }
}

/* mode_chase_color(FX.cpp L963-965 直译) [槽28] */
static uint16_t fx_1d_chase_color(wledc_segment_t *seg)
{
    fx1d_chase_core(seg, seg->colors[1],
                    (seg->colors[2]) ? seg->colors[2] : seg->colors[0],
                    seg->colors[0], true, false);
    return 0;
}

/* mode_chase_random(FX.cpp L972-974 直译) [槽29] */
static uint16_t fx_1d_chase_random(wledc_segment_t *seg)
{
    fx1d_chase_core(seg, seg->colors[1],
                    (seg->colors[2]) ? seg->colors[2] : seg->colors[0],
                    seg->colors[0], false, true);
    return 0;
}

/* mode_chase_rainbow(FX.cpp L981-988 直译) [槽30]
 * 段长>256 时 color_sep 修正为 1 */
static uint16_t fx_1d_chase_rainbow(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    unsigned color_sep = 256 / seglen;
    if (color_sep == 0) color_sep = 1; /* 段长超过 256 的修正 */
    unsigned color_index = seg->call & 0xFF;
    uint32_t color = fx1d_color_wheel(seg,
        (uint8_t)(((seg->step * color_sep) + color_index) & 0xFF));

    fx1d_chase_core(seg, color, seg->colors[0], seg->colors[1], false, false);
    return 0;
}

/* mode_chase_rainbow_white(FX.cpp L995-1002 直译) [槽33] */
static uint16_t fx_1d_chase_rainbow_white(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    uint16_t n = (uint16_t)seg->step;
    uint16_t m = (uint16_t)((seg->step + 1) % seglen);
    uint32_t color2 = fx1d_color_wheel(seg,
        (uint8_t)(((n * 256 / seglen) + (seg->call & 0xFF)) & 0xFF));
    uint32_t color3 = fx1d_color_wheel(seg,
        (uint8_t)(((m * 256 / seglen) + (seg->call & 0xFF)) & 0xFF));

    fx1d_chase_core(seg, seg->colors[0], color2, color3, false, false);
    return 0;
}

/* mode_running_color(FX.cpp L1164-1166 直译) [槽37] */
static uint16_t fx_1d_running_color(wledc_segment_t *seg)
{
    fx1d_running_core(seg, seg->colors[0], seg->colors[1], false);
    return 0;
}

/* FLASH_COUNT(FX.cpp L1082) */
#define FX1D_FLASH_COUNT 4

/* mode_chase_flash(FX.cpp L1083-1114 直译) [槽31]
 * 限速更新但每帧渲染 */
static uint16_t fx_1d_chase_flash(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    if (seglen <= 1) { /* FX_FALLBACK_STATIC */
        fx1d_fill(seg, seg->colors[0]);
        return 0;
    }
    uint32_t now = wledc_fx_now(); /* 保存时间用于延迟计算 */
    bool advance = true;
    unsigned flash_step = seg->aux1 % ((FX1D_FLASH_COUNT * 2) + 1);
    if (now < seg->step) {
        advance = false; /* 限速更新但每帧渲染 */
    } else {
        seg->aux1++;
    }

    for (unsigned i = 0; i < seglen; i++) {
        fx1d_set_pixel(seg, (uint16_t)i,
            fx1d_seg_palette_color(seg, (uint16_t)i, true,
                                   FX1D_SOLID_WRAP(seg), 0, 255));
    }
    unsigned index = seg->aux0;
    unsigned n = index;
    unsigned m = (index + 1) % seglen;

    unsigned delay = 10 + ((30 * (uint16_t)(255 - seg->speed)) / seglen);
    if (flash_step < (FX1D_FLASH_COUNT * 2)) {
        if (flash_step % 2 == 0) {
            fx1d_set_pixel(seg, (uint16_t)n, seg->colors[1]);
            fx1d_set_pixel(seg, (uint16_t)m, seg->colors[1]);
            delay = 20;
        } else {
            delay = 30;
        }
    } else if (advance) {
        seg->aux0 = (uint16_t)m; /* 推进到下一位置 */
    }
    if (advance) {
        seg->step = now + delay; /* 设定下次更新时间 */
    }
    return 0;
}

/* mode_chase_flash_random(FX.cpp L1121-1157 直译) [槽32]
 * now<step 时回退 call 计数,仅重渲染同帧 */
static uint16_t fx_1d_chase_flash_random(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    if (seglen <= 1) { /* FX_FALLBACK_STATIC */
        fx1d_fill(seg, seg->colors[0]);
        return 0;
    }
    uint32_t now = wledc_fx_now(); /* 保存时间用于延迟计算 */
    bool advance = true;
    if (now < seg->step) {
        seg->call--; /* 回退自增以跳过动画推进,仅重渲染同帧 */
        advance = false;
    }
    unsigned flash_step = seg->call % ((FX1D_FLASH_COUNT * 2) + 1);

    for (int i = 0; i < seg->aux1; i++) {
        fx1d_set_pixel(seg, (uint16_t)i,
                       fx1d_color_wheel(seg, (uint8_t)seg->aux0));
    }

    unsigned delay = 1 + ((10 * (uint16_t)(255 - seg->speed)) / seglen);
    if (flash_step < (FX1D_FLASH_COUNT * 2)) {
        unsigned n = seg->aux1;
        unsigned m = (seg->aux1 + 1) % seglen;
        if (flash_step % 2 == 0) {
            fx1d_set_pixel(seg, (uint16_t)n, seg->colors[0]);
            fx1d_set_pixel(seg, (uint16_t)m, seg->colors[0]);
            delay = 20;
        } else {
            fx1d_set_pixel(seg, (uint16_t)n,
                           fx1d_color_wheel(seg, (uint8_t)seg->aux0));
            fx1d_set_pixel(seg, (uint16_t)m, seg->colors[1]);
            delay = 30;
        }
    } else if (advance) {
        seg->aux1 = (uint16_t)((seg->aux1 + 1) % seglen);

        if (seg->aux1 == 0) {
            seg->aux0 = fx1d_random_wheel_index(seg->aux0);
        }
    }
    if (advance) {
        seg->step = now + delay; /* 设定下次更新时间 */
    }
    return 0;
}

/* mode_colorful(FX.cpp L1009-1045 直译) [槽34]
 * 4/3/5 色;palette==52(C9 2)取 5 色;intensity<80 粉彩 */
static uint16_t fx_1d_colorful(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    unsigned numColors = 4; /* 3,4 或 5 */
    uint32_t cols[9] = {0x00FF0000, 0x00EEBB00, 0x0000EE00, 0x000077CC};
    if (seg->intensity > 160 || seg->palette) { /* palette 或显式色 */
        if (!seg->palette) {
            numColors = 3;
            for (size_t i = 0; i < 3; i++) cols[i] = seg->colors[i];
        } else {
            unsigned fac = 80;
            if (seg->palette == 52) { /* C9 2 有 5 色 */
                numColors = 5;
                fac = 61;
            }
            for (size_t i = 0; i < numColors; i++) {
                cols[i] = fx1d_seg_palette_color(seg,
                    (uint16_t)(i * fac), false, true, 255, 255);
            }
        }
    } else if (seg->intensity < 80) { /* 粉彩(复活节)色 */
        cols[0] = 0x00FF8040;
        cols[1] = 0x00E5D241;
        cols[2] = 0x0077FF77;
        cols[3] = 0x0077F0F0;
    }
    for (size_t i = numColors; i < numColors * 2 - 1U; i++)
        cols[i] = cols[i - numColors];

    uint32_t now = wledc_fx_now();
    uint32_t cycleTime = 50 + (8 * (uint32_t)(255 - seg->speed));
    uint32_t it = now / cycleTime;
    if (it != seg->step) {
        if (seg->speed > 0) seg->aux0++;
        if (seg->aux0 >= numColors) seg->aux0 = 0;
        seg->step = it;
    }

    for (unsigned i = 0; i < seglen; i += numColors) {
        for (unsigned j = 0; j < numColors; j++)
            fx1d_set_pixel(seg, (uint16_t)(i + j),
                           cols[seg->aux0 + j]);
    }
    return 0;
}

/* mode_traffic_light(FX.cpp L1052-1075 直译) [槽35]
 * case3 用逆 gamma 恢复 16.0 前观感 */
static uint16_t fx_1d_traffic_light(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    if (seglen <= 1) { /* FX_FALLBACK_STATIC */
        fx1d_fill(seg, seg->colors[0]);
        return 0;
    }
    uint32_t now = wledc_fx_now();
    for (unsigned i = 0; i < seglen; i++) {
        fx1d_set_pixel(seg, (uint16_t)i,
            fx1d_seg_palette_color(seg, (uint16_t)i, true,
                                   FX1D_SOLID_WRAP(seg), 1, 255));
    }
    uint32_t mdelay = 500;
    for (unsigned i = 0; i < seglen - 2; i += 3) {
        switch (seg->aux0) {
        case 0:
            fx1d_set_pixel(seg, (uint16_t)i, 0x00FF0000);
            mdelay = 150 + (100 * (uint32_t)(255 - seg->speed));
            break;
        case 1:
            fx1d_set_pixel(seg, (uint16_t)i, 0x00FF0000);
            mdelay = 150 + (20 * (uint32_t)(255 - seg->speed));
            fx1d_set_pixel(seg, (uint16_t)(i + 1), 0x00EECC00);
            break;
        case 2:
            fx1d_set_pixel(seg, (uint16_t)(i + 2), 0x0000FF00);
            mdelay = 150 + (100 * (uint32_t)(255 - seg->speed));
            break;
        case 3:
            /* 逆 gamma 恢复 16.0 前观感 */
            fx1d_set_pixel(seg, (uint16_t)(i + 1),
                           wledc_inverse_gamma32(0x00EECC00));
            mdelay = 150 + (20 * (uint32_t)(255 - seg->speed));
            break;
        }
    }

    if (now - seg->step > mdelay) {
        seg->aux0++;
        if (seg->aux0 == 1 && seg->intensity > 140) seg->aux0 = 2; /* 跳过红+黄,美式序列 */
        if (seg->aux0 > 3) seg->aux0 = 0;
        seg->step = now;
    }
    return 0;
}

/* mode_running_random(FX.cpp L1173-1202 直译) [槽39]
 * hw_random() 映射为 wledc_random16();abs() 改为范围比较 */
static uint16_t fx_1d_running_random(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    uint32_t now = wledc_fx_now();
    uint32_t cycleTime = 25 + (3 * (uint32_t)(255 - seg->speed));
    uint32_t it = now / cycleTime;
    /* random seed for PRNG on start */
    if (seg->call == 0) seg->aux0 = wledc_random16();

    unsigned zoneSize = (((unsigned)(255 - seg->intensity)) >> 4) + 1;
    uint16_t PRNG16 = seg->aux0;

    unsigned z = it % zoneSize;
    bool nzone = (!z && it != seg->aux1);
    for (int i = (int)seglen - 1; i >= 0; i--) {
        if (nzone || z >= zoneSize) {
            unsigned lastrand = PRNG16 >> 8;
            int16_t diff = 0;
            /* make sure the difference between adjacent colors is big enough (>= 42) */
            while (diff > -42 && diff < 42) {
                /* next zone, next 'random' number */
                PRNG16 = (uint16_t)(PRNG16 * 2053) + 13849;
                diff = (int16_t)(PRNG16 >> 8) - (int16_t)lastrand;
            }
            if (nzone) {
                seg->aux0 = PRNG16; /* save next starting seed */
                nzone = false;
            }
            z = 0;
        }
        fx1d_set_pixel(seg, (uint16_t)i,
                       fx1d_color_wheel(seg, (uint8_t)(PRNG16 >> 8)));
        z++;
    }

    seg->aux1 = (uint16_t)it;
    return 0;
}

/* mode_larson_scanner(FX.cpp L1209-1249 直译) [槽40]
 * check1 固化为 dual 参数(DUAL_LARSON 传 true),check2 固化 false */
static uint16_t fx1d_larson_core(wledc_segment_t *seg, bool dual)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    if (seglen <= 1) { /* FX_FALLBACK_STATIC */
        fx1d_fill(seg, seg->colors[0]);
        return 0;
    }

    /* map into useful range */
    const unsigned speed = (unsigned)WLEDC_FRAME_TIME_MS *
        fx1d_map((int32_t)seg->speed, 0, 255, 96, 2);
    /* how many pixels to advance per frame */
    const unsigned pixels = seglen / speed;

    fx1d_fade_out(seg, 255 - seg->intensity);

    uint32_t now = wledc_fx_now();
    if (seg->step > now) return 0; /* we have a pause */

    unsigned index = seg->aux1 + pixels;
    /* are we slow enough to use frames per pixel? */
    if (pixels == 0) {
        const unsigned frames = speed / seglen; /* how many frames per 1 pixel */
        if (seg->step++ < frames) return 0;
        seg->step = 0;
        index++;
    }

    if (index > seglen) {
        seg->aux0 = !seg->aux0; /* change direction */
        seg->aux1 = 0; /* reset position */
        /* set delay(check2 固化 false,仅反向时延迟) */
        if (seg->aux0) seg->step = now + (uint32_t)seg->custom1 * 25;
        else seg->step = 0;
    } else {
        /* paint as many pixels as needed */
        for (unsigned i = seg->aux1; i < index; i++) {
            unsigned j = seg->aux0 ? i : (unsigned)seglen - 1 - i;
            uint32_t c = fx1d_seg_palette_color(seg, (uint16_t)j,
                true, FX1D_SOLID_WRAP(seg), 0, 255);
            fx1d_set_pixel(seg, (uint16_t)j, c);
            if (dual) {
                fx1d_set_pixel(seg, (uint16_t)(seglen - 1 - j),
                    seg->colors[2] ? seg->colors[2] : c);
            }
        }
        seg->aux1 = (uint16_t)index;
    }
    return 0;
}

static uint16_t fx_1d_larson_scanner(wledc_segment_t *seg)
{
    return fx1d_larson_core(seg, false);
}

/* mode_dual_larson_scanner(FX.cpp L1256-1259) [槽60]
 * 双向拉森扫描仪 */
static uint16_t fx_1d_dual_larson_scanner(wledc_segment_t *seg)
{
    return fx1d_larson_core(seg, true);
}

/* mode_comet(FX.cpp L1265-1284 直译) [槽41]
 * 从一端发射彗星("Lighthouse");原 aux0 = index++ 后缀自增被弃 */
static uint16_t fx_1d_comet(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    if (seglen <= 1) { /* FX_FALLBACK_STATIC */
        fx1d_fill(seg, seg->colors[0]);
        return 0;
    }
    uint32_t now = wledc_fx_now();
    unsigned counter =
        (unsigned)((now * ((seg->speed >> 2) + 1)) & 0xFFFF);
    unsigned index = (unsigned)((counter * (uint32_t)seglen) >> 16);
    if (seg->call == 0) seg->aux0 = (uint16_t)index;

    fx1d_fade_out(seg, seg->intensity);

    fx1d_set_pixel(seg, (uint16_t)index,
        fx1d_seg_palette_color(seg, (uint16_t)index, true,
                               FX1D_SOLID_WRAP(seg), 0, 255));
    if (index > seg->aux0) {
        for (unsigned i = seg->aux0; i < index; i++) {
            fx1d_set_pixel(seg, (uint16_t)i,
                fx1d_seg_palette_color(seg, (uint16_t)i, true,
                                       FX1D_SOLID_WRAP(seg), 0, 255));
        }
    } else if (index < seg->aux0 && index < 10) {
        for (unsigned i = 0; i < index; i++) {
            fx1d_set_pixel(seg, (uint16_t)i,
                fx1d_seg_palette_color(seg, (uint16_t)i, true,
                                       FX1D_SOLID_WRAP(seg), 0, 255));
        }
    }
    seg->aux0 = (uint16_t)index;
    return 0;
}

/* ============================ 批4(第1段) ============================ */

/* mode_twinkle(FX.cpp L655-681 直译) [槽17] */
static uint16_t fx_1d_twinkle(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    fx1d_fade_out(seg, 224); /* 22/32 fade */

    uint32_t cycle_time = 20 + (255u - seg->speed) * 5u;
    uint32_t it = wledc_fx_now() / cycle_time;
    if (it != seg->step) {
        /* make sure at least one LED is on */
        unsigned max_on =
            (unsigned)fx1d_map(seg->intensity, 0, 255, 1, (int32_t)seglen);
        if (seg->aux0 >= max_on) {
            seg->aux0 = 0;
            seg->aux1 = wledc_random16(); /* new seed for our PRNG */
        }
        seg->aux0++;
        seg->step = it;
    }

    uint16_t prng16 = seg->aux1;

    for (unsigned i = 0; i < seg->aux0; i++) {
        prng16 = (uint16_t)(prng16 * 2053u) + 13849u; /* next 'random' number */
        uint32_t p = (uint32_t)seglen * (uint32_t)prng16;
        unsigned j = p >> 16;
        fx1d_set_pixel(seg, (uint16_t)j,
            fx1d_seg_palette_color(seg, (uint16_t)j, true,
                                   FX1D_SOLID_WRAP(seg), 0, 255));
    }
    return 0;
}

/* dissolve core(FX.cpp L688-740 直译)
 * check2(Complete,UI 默认 false)固化 -> c^=1 颜色微调/incompletePixels
 * 统计/step 完成度门控全剪除,step 直接 ++ */
static void fx_1d_dissolve_core(wledc_segment_t *seg, uint32_t color)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    /* sizeof(uint32_t)*SEGLEN;seglen > 16383 时 16 位容量参数
     * 溢出,极端长段由 request_data 失败回退静态色兜底 */
    uint8_t *raw = wledc_segment_request_data(
        seg, (uint16_t)(sizeof(uint32_t) * (size_t)seglen));
    if (raw == NULL) { /* allocation failed */
        fx1d_fill(seg, seg->colors[0]);
        return;
    }
    uint32_t *pixels = (uint32_t *)raw;

    if (seg->call == 0) {
        for (uint16_t i = 0; i < seglen; i++) pixels[i] = seg->colors[1];
        seg->aux0 = 1;
    }

    for (uint16_t j = 0; j <= seglen / 15u; j++) {
        if (wledc_random8() <= seg->intensity) {
            /* attempt to spawn a new pixel 10 times */
            for (unsigned times = 0; times < 10; times++) {
                uint16_t i = wledc_random16_lim(seglen);
                if (seg->aux0) { /* dissolve to primary/palette */
                    if (pixels[i] == seg->colors[1]) {
                        uint32_t c = (color == seg->colors[0])
                            ? fx1d_seg_palette_color(seg, i, true,
                                                     FX1D_SOLID_WRAP(seg), 0, 255)
                            : color;
                        pixels[i] = c;
                        break; /* only spawn 1 new pixel per frame */
                    }
                } else { /* dissolve to secondary */
                    if (pixels[i] != seg->colors[1]) {
                        pixels[i] = seg->colors[1];
                        break;
                    }
                }
            }
        }
    }
    for (uint16_t i = 0; i < seglen; i++) {
        fx1d_set_pixel(seg, i, pixels[i]); /* fix for #4401 */
    }

    if (seg->step > (255u - seg->speed) + 15u) {
        seg->aux0 = !seg->aux0;
        seg->step = 0;
    } else {
        seg->step++;
    }
}

/* mode_dissolve(FX.cpp L746-748 直译) [槽18]
 * check1(Random,UI 默认 false)固化 -> 恒传 SEGCOLOR(0) */
static uint16_t fx_1d_dissolve(wledc_segment_t *seg)
{
    fx_1d_dissolve_core(seg, seg->colors[0]);
    return 0;
}

/* mode_dissolve_random(FX.cpp L755-757 直译) [槽19] */
static uint16_t fx_1d_dissolve_random(wledc_segment_t *seg)
{
    fx_1d_dissolve_core(seg, fx1d_color_wheel(seg, wledc_random8()));
    return 0;
}

/* mode_sparkle(FX.cpp L764-777 直译) [槽20]
 * check2(Overlay,UI 默认 false)固化 -> 恒重画背景 */
static uint16_t fx_1d_sparkle(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    for (uint16_t i = 0; i < seglen; i++) {
        fx1d_set_pixel(seg, i,
            fx1d_seg_palette_color(seg, i, true, FX1D_SOLID_WRAP(seg), 1, 255));
    }
    uint32_t cycle_time = 10 + (255u - seg->speed) * 2u;
    uint32_t it = wledc_fx_now() / cycle_time;
    if (it != seg->step) {
        seg->aux0 = wledc_random16_lim(seglen); /* random led index */
        seg->step = it;
    }

    fx1d_set_pixel(seg, seg->aux0, seg->colors[0]);
    return 0;
}

/* mode_flash_sparkle(FX.cpp L784-796 直译) [槽21]
 * check2(Overlay,UI 默认 false)固化 -> 恒重画背景 */
static uint16_t fx_1d_flash_sparkle(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    uint32_t now = wledc_fx_now();
    for (uint16_t i = 0; i < seglen; i++) {
        fx1d_set_pixel(seg, i,
            fx1d_seg_palette_color(seg, i, true, FX1D_SOLID_WRAP(seg), 0, 255));
    }

    if (now - seg->aux0 > seg->step) {
        if (wledc_random8_lim((uint8_t)((255 - seg->intensity) >> 4)) == 0) {
            fx1d_set_pixel(seg, wledc_random16_lim(seglen), seg->colors[1]); /* flash */
        }
        seg->step = now;
        seg->aux0 = (uint16_t)(255 - seg->speed);
    }
    return 0;
}

/* mode_hyper_sparkle(FX.cpp L804-819 直译) [槽22]
 * check2(Overlay,UI 默认 false)固化 -> 恒重画背景 */
static uint16_t fx_1d_hyper_sparkle(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    uint32_t now = wledc_fx_now();
    for (uint16_t i = 0; i < seglen; i++) {
        fx1d_set_pixel(seg, i,
            fx1d_seg_palette_color(seg, i, true, FX1D_SOLID_WRAP(seg), 0, 255));
    }

    if (now - seg->aux0 > seg->step) {
        if (wledc_random8_lim((uint8_t)((255 - seg->intensity) >> 4)) == 0) {
            int len = ((int)seglen / 3 > 1) ? (int)seglen / 3 : 1; /* max(1, SEGLEN/3) */
            for (int i = 0; i < len; i++) {
                fx1d_set_pixel(seg, wledc_random16_lim(seglen), seg->colors[1]);
            }
        }
        seg->step = now;
        seg->aux0 = (uint16_t)(255 - seg->speed);
    }
    return 0;
}

/* ===================== 批4(第2段): fireworks/rain/fire_flicker/gradient 系 ===================== */

/* mode_fireworks(FX.cpp L1290-1326 直译) [槽42] (1D 分支,2D 剪除) */
static uint16_t fx_1d_fireworks(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    if (seglen <= 1) { /* FX_FALLBACK_STATIC */
        fx1d_fill(seg, seg->colors[0]);
        return 0;
    }
    const uint16_t width  = seglen;
    const uint16_t height = 1;

    if (seg->call == 0) {
        seg->aux0 = UINT16_MAX;
        seg->aux1 = UINT16_MAX;
    }
    fx1d_fade_out(seg, 128);

    if (!seg->step) {
        /* fireworks mode (blur flares) */
        bool valid1 = (seg->aux0 < width * height);
        bool valid2 = (seg->aux1 < width * height);
        uint32_t sv1 = 0, sv2 = 0;
        if (valid1) sv1 = fx1d_get_pixel(seg, seg->aux0); /* get spark color */
        if (valid2) sv2 = fx1d_get_pixel(seg, seg->aux1);
        fx1d_blur(seg, 16); /* used in mode_rain() */
        if (valid1) fx1d_set_pixel(seg, seg->aux0, sv1); /* restore spark color after blur */
        if (valid2) fx1d_set_pixel(seg, seg->aux1, sv2); /* restore old spark color after blur */
    }

    int i_max = ((int)width / 20 > 1) ? (int)width / 20 : 1; /* max(1, width/20) */
    for (int i = 0; i < i_max; i++) {
        if (wledc_random8_lim((uint8_t)(129 - (seg->intensity >> 1))) == 0) {
            uint16_t index = wledc_random16_lim((uint16_t)(width * height));
            uint32_t col = fx1d_seg_palette_color(seg, wledc_random8(), false, false, 0, 255);
            fx1d_set_pixel(seg, index, col);
            seg->aux1 = seg->aux0; /* old spark */
            seg->aux0 = index;     /* remember where spark occurred */
        }
    }
    return 0;
}

/* mode_rain(FX.cpp L1330-1360 直译) [槽43] (1D 分支)
 * SPEED_FORMULA_L 展开 = 5+(50*(255-speed))/seglen
 * 原生 bug 照抄(L1355): aux1==0 时误写 aux0=UINT16_MAX(原意应为 aux1) */
static uint16_t fx_1d_rain(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    if (seglen <= 1) { /* FX_FALLBACK_STATIC */
        fx1d_fill(seg, seg->colors[0]);
        return 0;
    }
    const unsigned width  = seglen;
    const unsigned height = 1;
    seg->step += WLEDC_FRAME_TIME_MS;
    if (seg->call && seg->step > 5u + (50u * (255u - seg->speed)) / seglen) {
        seg->step = 1;
        /* 1D: shift all leds left */
        uint32_t ctemp = fx1d_get_pixel(seg, 0);
        for (unsigned i = 0; i < (unsigned)seglen - 1u; i++) {
            fx1d_set_pixel(seg, (uint16_t)i, fx1d_get_pixel(seg, (uint16_t)(i + 1u)));
        }
        fx1d_set_pixel(seg, (uint16_t)(seglen - 1), ctemp); /* wrap around */
        seg->aux0++; /* increase spark index */
        seg->aux1++;
        if (seg->aux0 == 0) seg->aux0 = UINT16_MAX; /* reset previous spark position */
        if (seg->aux1 == 0) seg->aux0 = UINT16_MAX; /* 原生 bug(L1355)照抄:应为 aux1 */
        if (seg->aux0 >= width * height) seg->aux0 = 0; /* ignore */
        if (seg->aux1 >= width * height) seg->aux1 = 0;
    }
    fx_1d_fireworks(seg);
    return 0;
}

/* mode_fire_flicker(FX.cpp L1366-1387 直译) [槽45]
 * MAX 嵌套与 MAX(r-f,0) 均用条件表达式实现(宿主无 MAX 宏) */
static uint16_t fx_1d_fire_flicker(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    uint32_t cycle_time = 40 + (255u - seg->speed);
    uint32_t it = wledc_fx_now() / cycle_time;
    if (seg->step == it) return 0;

    uint8_t w = WLEDC_W(seg->colors[0]);
    uint8_t r = WLEDC_R(seg->colors[0]);
    uint8_t g = WLEDC_G(seg->colors[0]);
    uint8_t b = WLEDC_B(seg->colors[0]);
    /* lum = (palette==0) ? MAX(w, MAX(r, MAX(g, b))) : 255 */
    uint8_t m_rb = (r > g) ? r : g;
    uint8_t m_rgb = (m_rb > b) ? m_rb : b;
    uint8_t lum = (seg->palette == 0) ? ((m_rgb > w) ? m_rgb : w) : 255;
    lum /= (uint8_t)(((256u - seg->intensity) / 16u) + 1u);
    for (uint16_t i = 0; i < seglen; i++) {
        uint8_t flicker = wledc_random8_lim(lum);
        if (seg->palette == 0) {
            fx1d_set_pixel(seg, i, WLEDC_RGBW32(
                (r > flicker) ? (uint8_t)(r - flicker) : 0,
                (g > flicker) ? (uint8_t)(g - flicker) : 0,
                (b > flicker) ? (uint8_t)(b - flicker) : 0,
                (w > flicker) ? (uint8_t)(w - flicker) : 0));
        } else {
            fx1d_set_pixel(seg, i, fx1d_seg_palette_color(seg, i, true,
                FX1D_SOLID_WRAP(seg), 0, (uint8_t)(255 - flicker)));
        }
    }

    seg->step = it;
    return 0;
}

/* gradient_base(FX.cpp L1394-1414 直译)
 * 原生 bug 照抄(L1400): "1 + loading ? a : b" 按 C 优先级解析为
 * (1 + loading) ? a : b,判别式恒非零 -> brd 恒为 intensity/2 */
static void fx_1d_gradient_base(wledc_segment_t *seg, bool loading)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    if (seglen <= 1) { /* FX_FALLBACK_STATIC */
        fx1d_fill(seg, seg->colors[0]);
        return;
    }
    uint16_t counter = (uint16_t)(wledc_fx_now() * ((seg->speed >> 2) + 1u));
    uint16_t pp = (uint16_t)((counter * seglen) >> 16);
    if (seg->call == 0) pp = 0;
    int val; /* 0 = sec 1 = pri */
    int brd = 1 + loading ? seg->intensity / 2 : seg->intensity / 4; /* 原生优先级 bug 照抄(L1400) */
    int p1 = (int)pp - (int)seglen;
    int p2 = (int)pp + (int)seglen;

    for (int i = 0; i < (int)seglen; i++) {
        if (loading) {
            val = abs(((i > (int)pp) ? p2 : (int)pp) - i);
        } else {
            int d1 = abs((int)pp - i);
            int d2 = abs(p1 - i);
            int d3 = abs(p2 - i);
            int m23 = (d2 < d3) ? d2 : d3;      /* min(abs(p1-i), abs(p2-i)) */
            val = (d1 < m23) ? d1 : m23;        /* min(abs(pp-i), min(...)) */
        }
        val = (brd > val) ? (val * 255) / brd : 255;
        fx1d_set_pixel(seg, (uint16_t)i, wledc_color_blend(seg->colors[0],
            fx1d_seg_palette_color(seg, (uint16_t)i, true, FX1D_SOLID_WRAP(seg), 1, 255),
            (uint8_t)val));
    }
}

/* mode_gradient(FX.cpp L1420-1422 直译) [槽46] */
static uint16_t fx_1d_gradient(wledc_segment_t *seg)
{
    fx_1d_gradient_base(seg, false);
    return 0;
}

/* mode_loading(FX.cpp L1429-1431 直译) [槽47] */
static uint16_t fx_1d_loading(wledc_segment_t *seg)
{
    fx_1d_gradient_base(seg, true);
    return 0;
}

/* ===================== 批4(第3段): multi_comet/meteor ===================== */

/* mode_multi_comet(FX.cpp L1777-1807 直译) [槽59] */
#define FX1D_MAX_COMETS 8
static uint16_t fx_1d_multi_comet(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    uint32_t cycle_time = 10 + (uint32_t)(255 - seg->speed);
    uint32_t it = wledc_fx_now() / cycle_time;
    if (seg->step == it) return 0;
    /* allocation failed -> FX_FALLBACK_STATIC (原生 L1782: allocateData(sizeof(uint16_t)*8)) */
    if (wledc_segment_request_data(seg, sizeof(uint16_t) * FX1D_MAX_COMETS) == NULL) {
        fx1d_fill(seg, seg->colors[0]);
        return 0;
    }

    fx1d_fade_out(seg, (uint8_t)(seg->intensity / 2 + 128));

    uint16_t *comets = (uint16_t *)seg->data;

    for (uint8_t i = 0; i < FX1D_MAX_COMETS; i++) {
        if (comets[i] < seglen) {
            uint16_t index = comets[i];
            if (seg->colors[2] != 0) {
                fx1d_set_pixel(seg, index, (i % 2)
                    ? fx1d_seg_palette_color(seg, index, true, FX1D_SOLID_WRAP(seg), 0, 255)
                    : seg->colors[2]);
            } else {
                fx1d_set_pixel(seg, index,
                    fx1d_seg_palette_color(seg, index, true, FX1D_SOLID_WRAP(seg), 0, 255));
            }
            comets[i]++;
        } else {
            if (wledc_random16_lim(seglen) == 0) {
                comets[i] = 0;
            }
        }
    }

    seg->step = it;
    return 0;
}

/* mode_meteor(FX.cpp L2378-2441 直译) [槽76]
 * check3(Smooth)固化 false -> meteorSmooth 分支整段剪除(L2386/L2397-2403/L2423-2427)
 * 原生 max = (palette==5 || !check1) ? 240 : 255,check1 固化 false -> 恒 240(照抄+注释) */
static uint16_t fx_1d_meteor(wledc_segment_t *seg)
{
    uint16_t seglen = fx1d_virtual_length(seg);
    if (seglen <= 1) { /* FX_FALLBACK_STATIC */
        fx1d_fill(seg, seg->colors[0]);
        return 0;
    }
    /* allocation failed -> FX_FALLBACK_STATIC (原生 L2380: allocateData(SEGLEN)) */
    if (wledc_segment_request_data(seg, seglen) == NULL) {
        fx1d_fill(seg, seg->colors[0]);
        return 0;
    }
    uint8_t *trail = (uint8_t *)seg->data;

    const uint16_t meteor_size = 1 + seglen / 20; /* 5% */
    /* meteorSmooth 固化 false -> 仅保留原生 else 分支(L2388-2389) */
    uint32_t counter = wledc_fx_now() * ((seg->speed >> 2) + 8u);
    uint16_t meteorstart = (uint16_t)((counter * seglen) >> 16);

    /* 原生 L2392: check1 固化 false -> !check1 恒真 -> max 恒 240 */
    const int trail_max = 240;

    /* fade all leds to colors[1] in LEDs one step (仅原生非 smooth 分支 L2405-2414,
     * check1 固化 false -> idx=0, index=map(i,0,SEGLEN,0,max), bri=trail[i]) */
    for (uint16_t i = 0; i < seglen; i++) {
        if (wledc_random8() <= (uint8_t)(255 - seg->intensity)) {
            trail[i] = (uint8_t)wledc_fast_color_scale(trail[i],
                (uint8_t)(128 + wledc_random8_lim(127)));
            int32_t index = fx1d_map((int32_t)i, 0, (int32_t)seglen, 0, trail_max);
            uint32_t col = fx1d_seg_palette_color(seg, (uint16_t)index, false, false, 0, trail[i]);
            fx1d_set_pixel(seg, i, col);
        }
    }

    /* draw meteor (仅原生非 smooth 分支 L2428-2437,check1 固化 false -> i=map, idx=0) */
    for (uint16_t j = 0; j < meteor_size; j++) {
        uint16_t index = (uint16_t)((meteorstart + j) % seglen);
        trail[index] = (uint8_t)trail_max;
        int32_t i = fx1d_map((int32_t)index, 0, (int32_t)seglen, 0, trail_max);
        uint32_t col = fx1d_seg_palette_color(seg, (uint16_t)i, false, false, 0, 255); /* full brightness */
        fx1d_set_pixel(seg, index, col);
    }

    seg->step += seg->speed + 1u;
    return 0;
}
#undef FX1D_MAX_COMETS

/* ================================================================ */
/* 77 槽效果表(槽号对齐 wled00/FX.h FX_MODE_* 0..76)               */
/* 已实现 49 槽;未实现槽为 NULL,由引擎 render_black 回退           */
/* ================================================================ */

#define WLEDC_FX_1D_SLOT_COUNT 77u

static const wledc_fx_fn fx_1d_slot_table[WLEDC_FX_1D_SLOT_COUNT] = {
    fx_1d_static,               /*  0 FX_MODE_STATIC               */
    fx_1d_blink,                /*  1 FX_MODE_BLINK                */
    fx_1d_breath,               /*  2 FX_MODE_BREATH               */
    fx_1d_color_wipe,           /*  3 FX_MODE_COLOR_WIPE           */
    fx_1d_color_wipe_random,    /*  4 FX_MODE_COLOR_WIPE_RANDOM    */
    fx_1d_random_color,         /*  5 FX_MODE_RANDOM_COLOR         */
    fx_1d_color_sweep,          /*  6 FX_MODE_COLOR_SWEEP          */
    fx_1d_dynamic,              /*  7 FX_MODE_DYNAMIC              */
    fx_1d_rainbow,              /*  8 FX_MODE_RAINBOW              */
    fx_1d_rainbow_cycle,        /*  9 FX_MODE_RAINBOW_CYCLE        */
    fx_1d_scan,                 /* 10 FX_MODE_SCAN                 */
    fx_1d_dual_scan,           /* 11 FX_MODE_DUAL_SCAN            */
    fx_1d_fade,                 /* 12 FX_MODE_FADE                 */
    fx_1d_theater_chase,       /* 13 FX_MODE_THEATER_CHASE        */
    fx_1d_theater_chase_rainbow,/* 14 FX_MODE_THEATER_CHASE_RAINBOW*/
    fx_1d_running_lights,      /* 15 FX_MODE_RUNNING_LIGHTS       */
    fx_1d_saw,                  /* 16 FX_MODE_SAW                  */
    fx_1d_twinkle,              /* 17 FX_MODE_TWINKLE              */
    fx_1d_dissolve,             /* 18 FX_MODE_DISSOLVE            */
    fx_1d_dissolve_random,      /* 19 FX_MODE_DISSOLVE_RANDOM     */
    fx_1d_sparkle,              /* 20 FX_MODE_SPARKLE              */
    fx_1d_flash_sparkle,        /* 21 FX_MODE_FLASH_SPARKLE       */
    fx_1d_hyper_sparkle,        /* 22 FX_MODE_HYPER_SPARKLE       */
    fx_1d_strobe,               /* 23 FX_MODE_STROBE               */
    fx_1d_strobe_rainbow,       /* 24 FX_MODE_STROBE_RAINBOW      */
    fx_1d_multi_strobe,         /* 25 FX_MODE_MULTI_STROBE        */
    fx_1d_blink_rainbow,        /* 26 FX_MODE_BLINK_RAINBOW       */
    NULL,                       /* 27 FX_MODE_ANDROID(未移植)     */
    fx_1d_chase_color,          /* 28 FX_MODE_CHASE_COLOR          */
    fx_1d_chase_random,         /* 29 FX_MODE_CHASE_RANDOM         */
    fx_1d_chase_rainbow,        /* 30 FX_MODE_CHASE_RAINBOW       */
    fx_1d_chase_flash,          /* 31 FX_MODE_CHASE_FLASH          */
    fx_1d_chase_flash_random,   /* 32 FX_MODE_CHASE_FLASH_RANDOM  */
    fx_1d_chase_rainbow_white,  /* 33 FX_MODE_CHASE_RAINBOW_WHITE */
    fx_1d_colorful,             /* 34 FX_MODE_COLORFUL             */
    fx_1d_traffic_light,        /* 35 FX_MODE_TRAFFIC_LIGHT       */
    fx_1d_color_sweep_random,   /* 36 FX_MODE_COLOR_SWEEP_RANDOM  */
    fx_1d_running_color,       /* 37 FX_MODE_RUNNING_COLOR       */
    NULL,                       /* 38 FX_MODE_AURORA(未移植)      */
    fx_1d_running_random,       /* 39 FX_MODE_RUNNING_RANDOM      */
    fx_1d_larson_scanner,       /* 40 FX_MODE_LARSON_SCANNER      */
    fx_1d_comet,                /* 41 FX_MODE_COMET                */
    fx_1d_fireworks,            /* 42 FX_MODE_FIREWORKS            */
    fx_1d_rain,                 /* 43 FX_MODE_RAIN                 */
    NULL,                       /* 44 FX_MODE_TETRIX(未移植)     */
    fx_1d_fire_flicker,         /* 45 FX_MODE_FIRE_FLICKER         */
    fx_1d_gradient,             /* 46 FX_MODE_GRADIENT             */
    fx_1d_loading,              /* 47 FX_MODE_LOADING              */
    NULL,                       /* 48 FX_MODE_ROLLINGBALLS(未移植)*/
    NULL,                       /* 49 FX_MODE_FAIRY(未移植)       */
    NULL,                       /* 50 FX_MODE_TWO_DOTS(未移植)     */
    NULL,                       /* 51 FX_MODE_FAIRYTWINKLE(未移植)*/
    fx_1d_running_dual,         /* 52 FX_MODE_RUNNING_DUAL         */
    NULL,                       /* 53 FX_MODE_IMAGE(未移植)        */
    NULL,                       /* 54 FX_MODE_TRICOLOR_CHASE       */
    NULL,                       /* 55 FX_MODE_TRICOLOR_WIPE        */
    NULL,                       /* 56 FX_MODE_TRICOLOR_FADE        */
    NULL,                       /* 57 FX_MODE_LIGHTNING(未移植)    */
    NULL,                       /* 58 FX_MODE_ICU(未移植)          */
    fx_1d_multi_comet,          /* 59 FX_MODE_MULTI_COMET          */
    fx_1d_dual_larson_scanner,  /* 60 FX_MODE_DUAL_LARSON_SCANNER */
    NULL,                       /* 61 FX_MODE_RANDOM_CHASE(未移植)*/
    NULL,                       /* 62 FX_MODE_OSCILLATE(未移植)    */
    NULL,                       /* 63 FX_MODE_PRIDE_2015(未移植)   */
    NULL,                       /* 64 FX_MODE_JUGGLE(未移植)       */
    NULL,                       /* 65 FX_MODE_PALETTE(未移植)      */
    NULL,                       /* 66 FX_MODE_FIRE_2012(未移植)    */
    NULL,                       /* 67 FX_MODE_COLORWAVES(未移植)   */
    NULL,                       /* 68 FX_MODE_BPM(未移植)          */
    NULL,                       /* 69 FX_MODE_FILLNOISE8(未移植)   */
    NULL,                       /* 70 FX_MODE_NOISE16_1(未移植)    */
    NULL,                       /* 71 FX_MODE_NOISE16_2(未移植)    */
    NULL,                       /* 72 FX_MODE_NOISE16_3(未移植)    */
    NULL,                       /* 73 FX_MODE_NOISE16_4(未移植)    */
    NULL,                       /* 74 FX_MODE_COLORTWINKLE(未移植)*/
    NULL,                       /* 75 FX_MODE_LAKE(未移植)         */
    fx_1d_meteor,               /* 76 FX_MODE_METEOR               */
};

const wledc_fx_fn *wledc_fx_1d_table(void)
{
    return fx_1d_slot_table;
}

size_t wledc_fx_1d_count(void)
{
    return WLEDC_FX_1D_SLOT_COUNT;
}
