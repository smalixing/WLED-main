/*
 * wled_c_fx_2d.c — WLED 2D 效果移植(纯 C,ESP-IDF 风格)
 *
 * 逐函数对齐 wled00/FX.cpp / FX_2Dfcn.cpp 原生实现,移植规则见 wled_c_fx_2d.h:
 *   1) 虚拟几何 vw/vh 经 wled_c_2d 寻址层(wledc_segment_virtual_width/
 *      virtual_height);原生 virtualWidth/virtualHeight 含 groupLen 折叠,
 *      宿主寻址层有意不含(任务 8 设计,段布局未建模 grouping)。
 *   2) 像素写入/读取经 wledc_segment_set/get_pixel_color_xy(越界静默 /
 *      返 0,对齐原生 PixelMatrix 映射链);blur2D 按原生 raw 语义经
 *      wledc_2d_xy_index(x,y,vw) 直访物理缓冲。
 *   3) fill / fadeToBlackBy 遍历物理 pixels(对齐原生 raw 遍历语义)。
 *   4) request_data 失败 -> 回退 FX_FALLBACK_STATIC(填 colors[0])。
 *   5) 原生 check1/2/3 位域宿主段结构不提供,按 WLED UI 默认值固化,
 *      逐效果注释说明。
 *   6) 原生 bug 注释后原样复刻。
 *
 * 帧约定:效果函数返回 0 = 使用默认帧时间(WLEDC_FRAME_TIME_MS)。
 * 效果入口检查 is_2d,失败填 colors[0] 并返回 0。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wled_c/wled_c_fx_2d.h"

#ifdef WLEDC_ENABLE_2D

#include "wled_c/wled_c_2d.h"
#include "wled_c/wled_c_color.h"
#include "wled_c/wled_c_fx.h"
#include "wled_c/wled_c_fx_support.h"
#include "wled_c/wled_c_palette.h"
#include "wled_c/wled_c_segment.h"

/* ============================ 内部宏 ============================ */

/* 位域工具(原生 bits.h bitRead/bitSet/bitClear,Matrix 用) */
#define FX2D_BIT_READ(buf, i)  (((buf)[(i) >> 3] >> ((i) & 7u)) & 1u)
#define FX2D_BIT_SET(buf, i)   ((buf)[(i) >> 3] |= (uint8_t)(1u << ((i) & 7u)))
#define FX2D_BIT_CLEAR(buf, i) ((buf)[(i) >> 3] &= (uint8_t)~(1u << ((i) & 7u)))

/* DARKSLATEGRAY(原生 FX_2Dfcn.cpp 常量,HTML #2F4F4F) */
#define FX2D_DARKSLATEGRAY WLEDC_RGB32(47, 79, 79)

/* ============================ 几何与像素访问 ============================ */

/* 虚拟宽(失败返 0,供调用方零尺寸判断) */
static uint16_t fx2d_vw(const wledc_segment_t *seg)
{
    uint16_t v = 0;
    (void)wledc_segment_virtual_width(seg, &v);
    return v;
}

/* 虚拟高(同上) */
static uint16_t fx2d_vh(const wledc_segment_t *seg)
{
    uint16_t v = 0;
    (void)wledc_segment_virtual_height(seg, &v);
    return v;
}

/* setPixelColorXY(越界静默,2d 寻址层已含) */
static void fx2d_set_pixel(wledc_segment_t *seg, uint16_t x, uint16_t y,
                           uint32_t c)
{
    (void)wledc_segment_set_pixel_color_xy(seg, x, y, c);
}

/* getPixelColorXY(越界返 0) */
static uint32_t fx2d_get_pixel(const wledc_segment_t *seg, uint16_t x,
                               uint16_t y)
{
    return wledc_segment_get_pixel_color_xy(seg, x, y);
}

/* addPixelColorXY(FX.h L744:preserveCR 默认 true) */
static void fx2d_add_pixel_color_xy(wledc_segment_t *seg, uint16_t x,
                                    uint16_t y, uint32_t c)
{
    fx2d_set_pixel(seg, x, y,
                   wledc_color_add(fx2d_get_pixel(seg, x, y), c, true));
}

/* fadePixelColorXY(FX.h L750) */
static void fx2d_fade_pixel_color_xy(wledc_segment_t *seg, uint16_t x,
                                     uint16_t y, uint8_t fade)
{
    fx2d_set_pixel(seg, x, y,
                   wledc_color_fade(fx2d_get_pixel(seg, x, y), fade, true));
}

/* ============================ 直译辅助 ============================ */

/* fill(FX_fcn.cpp L1086-1089 直译:raw 全填) */
static void fx2d_fill(wledc_segment_t *seg, uint32_t c)
{
    if (seg->pixels == NULL) return;
    for (uint16_t i = 0; i < seg->pixels_len; i++) seg->pixels[i] = c;
}

/* fadeToBlackBy(FX_fcn.cpp L1127-1131 直译) */
static void fx2d_fade_to_black_by(wledc_segment_t *seg, uint8_t fade_by)
{
    if (seg->pixels == NULL || fade_by == 0) return;
    for (uint16_t i = 0; i < seg->pixels_len; i++) {
        seg->pixels[i] =
            wledc_fast_color_scale(seg->pixels[i], (uint8_t)(255 - fade_by));
    }
}

/* blur2D(FX_2Dfcn.cpp L246-289 直译)
 * X pass 在前(逐 row),Y pass 在后(逐 col),两 pass 相互独立;
 * FastLED 盲渗式:首像素单独处理避免循环内分支,渗出 part 计入
 * 前一像素,carryover 链式传递;raw 直访 XY(x,y)=x+y*vw。
 * 原生 blur(x) = blur2D(x,x,smear) 收缩形由调用方传参表达;
 * smear 调用方均未启用,固化 false 省略。 */
static void fx2d_blur(wledc_segment_t *seg, uint8_t blur_x, uint8_t blur_y)
{
    if (!seg->active || seg->pixels == NULL) return;

    const uint16_t vw = fx2d_vw(seg);
    const uint16_t vh = fx2d_vh(seg);
    if (vw == 0 || vh == 0) return;
    const uint32_t stride = (uint32_t)vw * (uint32_t)vh;
    if (stride > seg->pixels_len) return; /* 防御:虚拟域超出物理缓冲 */

    if (blur_x > 0) { /* blur rows(水平) */
        const uint8_t keep = (uint8_t)(255 - blur_x);
        const uint8_t seep = blur_x >> 1;
        for (uint16_t row = 0; row < vh; row++) {
            const uint32_t row_base = (uint32_t)row * vw;
            /* handle first pixel to avoid conditional in loop (faster) */
            uint32_t cur = seg->pixels[row_base];
            uint32_t carryover = wledc_fast_color_scale(cur, seep);
            seg->pixels[row_base] = wledc_fast_color_scale(cur, keep);
            for (uint16_t x = 1; x < vw; x++) {
                const uint32_t idx = row_base + x;
                cur = seg->pixels[idx];
                uint32_t part = wledc_fast_color_scale(cur, seep);
                cur = wledc_fast_color_scale(cur, keep);
                cur = wledc_color_add(cur, carryover, false);
                seg->pixels[idx - 1] = wledc_color_add(
                    seg->pixels[idx - 1], part, false); /* previous pixel */
                seg->pixels[idx] = cur;                  /* current pixel */
                carryover = part;
            }
        }
    }

    if (blur_y > 0) { /* blur columns(垂直) */
        const uint8_t keep = (uint8_t)(255 - blur_y);
        const uint8_t seep = blur_y >> 1;
        for (uint16_t col = 0; col < vw; col++) {
            /* handle first pixel to avoid conditional in loop (faster) */
            uint32_t cur = seg->pixels[col];
            uint32_t carryover = wledc_fast_color_scale(cur, seep);
            seg->pixels[col] = wledc_fast_color_scale(cur, keep);
            for (uint16_t y = 1; y < vh; y++) {
                const uint32_t idx = col + (uint32_t)y * vw;
                cur = seg->pixels[idx];
                uint32_t part = wledc_fast_color_scale(cur, seep);
                cur = wledc_fast_color_scale(cur, keep);
                cur = wledc_color_add(cur, carryover, false);
                seg->pixels[idx - vw] = wledc_color_add(
                    seg->pixels[idx - vw], part, false); /* previous pixel */
                seg->pixels[idx] = cur;                  /* current pixel */
                carryover = part;
            }
        }
    }
}

/* ============================ 调色板辅助 ============================ */

/* 前向声明(调用点在定义之前) */
static uint32_t fx2d_seg_palette_color(wledc_segment_t *seg, uint16_t i,
                                       bool mapping, bool moving,
                                       uint8_t mcol, uint8_t pbri);

/* 直接 ColorFromPalette(SEGPALETTE, idx, bri, LINEARBLEND) 的移植目标
 * (装载当前段调色板,LINEAR 混合);装载失败返黑兜底 */
static uint32_t fx2d_palette_color(wledc_segment_t *seg, uint8_t idx,
                                   uint8_t bri)
{
    uint8_t entries[WLEDC_PALETTE_ENTRIES_BYTES];
    if (!wledc_fx_segment_palette_entries(seg, entries)) {
        return WLEDC_BLACK; /* 取板失败兜底 */
    }
    return wledc_color_from_palette(entries, idx, bri, WLEDC_BLEND_LINEAR);
}

/* color_from_palette(FX_fcn.cpp L1187-1208 直译;2D 版 v_length = vw*vh)
 * 签名对齐原生 (i, mapping, moving, mcol=0, pbri=255);
 * palette==0 && mcol<3 特例返回 color_fade(colors[mcol], pbri, true) */
static uint32_t fx2d_seg_palette_color(wledc_segment_t *seg, uint16_t i,
                                       bool mapping, bool moving,
                                       uint8_t mcol, uint8_t pbri)
{
    uint32_t color = seg->colors[(mcol < WLEDC_NUM_COLORS) ? mcol : 0];

    if (seg->palette == 0 && mcol < WLEDC_NUM_COLORS) {
        return wledc_color_fade(color, pbri, true);
    }

    uint32_t v_length = (uint32_t)fx2d_vw(seg) * (uint32_t)fx2d_vh(seg);
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

/* ============================ 数值辅助 ============================ */

/* abs8(fastled_slim.h L17 直译;参数 int8_t:原生调用点传较宽差值时
 * 先隐式窄化再取绝对值——怪癖保留,ColoredBursts/DNASpiral 依赖) */
static int8_t fx2d_abs8(int8_t i)
{
    return (i < 0) ? (int8_t)-i : i;
}

/* lerp8by8(fastled_slim.h L18 直译,注意为 WLED 自带 shim 实现,
 * 非 FastLED 原版;a + ((b-a)*(frac+1))>>8,中间量 int32 精确) */
static int8_t fx2d_lerp8by8(uint8_t a, uint8_t b, uint8_t frac)
{
    return (int8_t)(a + ((((int32_t)b - (int32_t)a) * ((int32_t)frac + 1)) >> 8));
}

/* map(Arduino map 直译,long -> int32_t,照 fx1d_map) */
static int32_t fx2d_map(int32_t x, int32_t in_min, int32_t in_max,
                        int32_t out_min, int32_t out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

/* ======================= 2D 效果函数(槽号 114-186) ======================= */

/* Matrix XY 折叠索引(原生 L5713 lambda: (x%cols)+(y%rows)*cols) */
static unsigned fx2d_matrix_xy(int x, int y, int cols, int rows)
{
    return (unsigned)((x % cols) + (y % rows) * cols);
}

/* sin_approx/cos_approx 复刻(原生 wled_math.cpp L92-104):
 * 先 (int) 后 (uint16_t) 截断,再 sin16,除 0x7FFF;
 * 勿直接负 float 转 uint(C3 上未定义),照抄原生注释。 */
#define FX2D_TWO_PI 6.283185307179586
#define FX2D_DEG_TO_RAD 0.017453292519943295

static float fx2d_sin_t(float theta)
{
    const uint16_t scaled_theta =
        (uint16_t)(int)(theta * (float)(0xFFFF / FX2D_TWO_PI));
    return (float)wledc_sin16(scaled_theta) / 32767.0f;
}

static float fx2d_cos_t(float theta)
{
    const uint16_t scaled_theta =
        (uint16_t)(int)(theta * (float)(0xFFFF / FX2D_TWO_PI));
    return (float)wledc_sin16((uint16_t)(scaled_theta + 0x4000u)) / 32767.0f;
}

/* 槽 150 | FX_MODE_2DSQUAREDSWIRL | 原生 FX.cpp L5971-5994
 * check1/2/3 无;原生 blur(custom3>>1) 单参 = blur2D(v,v) 对称。 */
static uint16_t fx2d_mode_squaredswirl(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);

    const uint8_t k_border_width = 2;

    fx2d_fade_to_black_by(seg, (uint8_t)(1 + seg->intensity / 5));
    const uint8_t swirl_blur = seg->custom3 >> 1;
    fx2d_blur(seg, swirl_blur, swirl_blur);

    /* use two out-of-sync sine waves */
    const uint32_t now = wledc_fx_now();
    const uint8_t hi_x = (uint8_t)(cols - k_border_width); /* 窄化怪癖照原生 */
    const uint8_t hi_y = (uint8_t)(rows - k_border_width);
    int i = wledc_beatsin8(19, k_border_width, hi_x, now, 0, 0);
    int j = wledc_beatsin8(22, k_border_width, hi_x, now, 0, 0);
    int k = wledc_beatsin8(17, k_border_width, hi_x, now, 0, 0);
    int m = wledc_beatsin8(18, k_border_width, hi_y, now, 0, 0);
    int n = wledc_beatsin8(15, k_border_width, hi_y, now, 0, 0);
    int p = wledc_beatsin8(20, k_border_width, hi_y, now, 0, 0);

    fx2d_add_pixel_color_xy(seg, (uint16_t)i, (uint16_t)m,
                            fx2d_palette_color(seg, (uint8_t)(now / 29), 255));
    fx2d_add_pixel_color_xy(seg, (uint16_t)j, (uint16_t)n,
                            fx2d_palette_color(seg, (uint8_t)(now / 41), 255));
    fx2d_add_pixel_color_xy(seg, (uint16_t)k, (uint16_t)p,
                            fx2d_palette_color(seg, (uint8_t)(now / 73), 255));
    return 0;
}

/* 槽 152 | FX_MODE_2DDNA | 原生 FX.cpp L5243-5256
 * check1 固化 0:blur(intensity/8, smear=false) = blur2D(v,v)。 */
static uint16_t fx2d_mode_dna(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);
    const uint32_t now = wledc_fx_now();

    fx2d_fade_to_black_by(seg, 64);
    for (int i = 0; i < cols; i++) {
        const uint8_t bri = wledc_beatsin8(5, 55, 255, now, 0,
                                           (uint16_t)(i * 10));
        fx2d_set_pixel(seg, (uint16_t)i,
                       wledc_beatsin8(seg->speed / 8, 0,
                                      (uint8_t)(rows - 1), now, 0,
                                      (uint16_t)(i * 4)),
                       fx2d_palette_color(seg,
                                          (uint8_t)(i * 5 + now / 17),
                                          bri));
        const uint8_t bri2 = wledc_beatsin8(5, 55, 255, now, 0,
                                            (uint16_t)(i * 10 + 128));
        fx2d_set_pixel(seg, (uint16_t)i,
                       wledc_beatsin8(seg->speed / 8, 0,
                                      (uint8_t)(rows - 1), now, 0,
                                      (uint16_t)(i * 4 + 128)),
                       fx2d_palette_color(seg,
                                          (uint8_t)(i * 5 + 128 + now / 17),
                                          bri2));
    }
    const uint8_t dna_blur = seg->intensity / 8; /* 8 - check1*2,固化 0 */
    fx2d_blur(seg, dna_blur, dna_blur);
    return 0;
}

/* 槽 153 | FX_MODE_2DMATRIX | 原生 FX.cpp L5708-5770
 * check1 固化 false:不用 SEGCOLOR(0/1),spawn/trail 用
 * gamma8inv(2.2) 硬编码复刻色(175→215/255→255/175→215;27→91/130→187/39→108)。 */
static uint16_t fx2d_mode_matrix(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);

    const uint16_t data_size = (uint16_t)((seg->stop - seg->start + 7) >> 3);
    uint8_t *data = wledc_segment_request_data(seg, data_size);
    if (data == NULL) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* request_data 失败回退 */
    }

    if (seg->call == 0) {
        fx2d_fill(seg, WLEDC_BLACK);
        seg->step = 0;
    }

    const uint32_t now = wledc_fx_now();
    const uint8_t fade = (uint8_t)fx2d_map(seg->custom1, 0, 255, 30, 250);
    const int min_rows = (rows < 150) ? rows : 150; /* min(rows,150) */
    const uint8_t speed = (uint8_t)((256 - (int)seg->speed)
                                    >> fx2d_map(min_rows, 0, 150, 0, 3));

    uint32_t spawn_color; /* check1 固化 false(否则 SEGCOLOR(0/1)) */
    uint32_t trail_color;
    spawn_color = WLEDC_RGBW32(215, 255, 215, 0); /* gamma8inv(175,255,175) */
    trail_color = WLEDC_RGBW32(91, 187, 108, 0);   /* gamma8inv(27,130,39)  */

    bool empty_screen = true;
    if (now - seg->step >= speed) {
        seg->step = now;
        fx2d_fade_to_black_by(seg, fade);
        for (int row = rows - 1; row >= 0; row--) {
            for (int col = 0; col < cols; col++) {
                const unsigned xy = fx2d_matrix_xy(col, row, cols, rows);
                if (FX2D_BIT_READ(data, xy)) {
                    fx2d_set_pixel(seg, (uint16_t)col, (uint16_t)row,
                                   trail_color);
                    FX2D_BIT_CLEAR(data, xy);
                    if (row < rows - 1) {
                        const unsigned xy2 = fx2d_matrix_xy(col, row + 1,
                                                           cols, rows);
                        fx2d_set_pixel(seg, (uint16_t)col,
                                       (uint16_t)(row + 1), spawn_color);
                        FX2D_BIT_SET(data, xy2);
                        empty_screen = false;
                    }
                }
            }
        }

        /* spawn new falling code */
        if (wledc_random8() <= seg->intensity || empty_screen) {
            const uint8_t spawn_x = wledc_random8_lim((uint8_t)cols);
            fx2d_set_pixel(seg, spawn_x, 0, spawn_color);
            FX2D_BIT_SET(data, fx2d_matrix_xy(spawn_x, 0, cols, rows));
        }
    }
    return 0;
}

/* 槽 162 | FX_MODE_2DPULSER | 原生 FX.cpp L5923-5936
 * blur(intensity>>4) 单参 = blur2D(v,v) 对称;sin8 参数 uint8_t 自然窄化。 */
static uint16_t fx2d_mode_pulser(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);

    fx2d_fade_to_black_by(seg, (uint8_t)(8 - (seg->intensity >> 5)));
    const uint32_t now = wledc_fx_now();
    const uint32_t a = now / (uint32_t)(18 - seg->speed / 16);
    const int x = (int)((a / 14) % (uint32_t)cols);
    const int y = (int)fx2d_map(wledc_sin8((uint8_t)(a * 5))
                                    + wledc_sin8((uint8_t)(a * 4))
                                    + wledc_sin8((uint8_t)(a * 2)),
                                0, 765, rows - 1, 0);
    fx2d_set_pixel(seg, (uint16_t)x, (uint16_t)y,
                   fx2d_palette_color(seg,
                                      (uint8_t)fx2d_map(y, 0, rows - 1,
                                                        0, 255), 255));

    const uint8_t pulser_blur = seg->intensity >> 4;
    fx2d_blur(seg, pulser_blur, pulser_blur);
    return 0;
}

/* 槽 164 | FX_MODE_2DDRIFT | 原生 FX.cpp L5307-5328
 * check1 固化 0:Twin 分支省略;check2 固化 0:blur(intensity>>3, smear=false)
 * 落为 blur2D(v,v) 对称;除数 32-(speed>>3) 恒 >= 1(speed<=255),无除零。
 * maxDim 先 MAX() 整除后转 float,照原生。 */
static uint16_t fx2d_mode_drift(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);
    const uint32_t now = wledc_fx_now();

    const int cols_center = (cols >> 1) + (cols % 2);
    const int rows_center = (rows >> 1) + (rows % 2);

    fx2d_fade_to_black_by(seg, 128);
    const float max_dim =
        (float)((cols > rows ? cols : rows) / 2); /* MAX() 先整除后转 float */
    const uint32_t t = now / (uint32_t)(32 - (seg->speed >> 3));
    const uint32_t t_20 = t / 20;
    for (float i = 1.0f; i < max_dim; i += 0.25f) {
        const float angle =
            ((float)t * (max_dim - i)) * FX2D_DEG_TO_RAD; /* radians() 展开 */
        const int my_sin = (int)(fx2d_sin_t(angle) * i);
        const int my_cos = (int)(fx2d_cos_t(angle) * i);
        fx2d_set_pixel(seg, (uint16_t)(cols_center + my_sin),
                       (uint16_t)(rows_center + my_cos),
                       fx2d_palette_color(seg,
                                          (uint8_t)((i * 20) + t_20),
                                          255));
    }
    const uint8_t drift_blur = seg->intensity >> 3; /* 3 - check2,固化 0 */
    fx2d_blur(seg, drift_blur, drift_blur);
    return 0;
}

/* 槽 167 | FX_MODE_2DCOLOREDBURSTS | 原生 FX.cpp L5193-5236
 * check1(grad)/check3(dot) 固化 0:梯度 fade 像素与端点白/灰分支省略;
 * check2 固化 0:fadeToBlackBy(40),blur(custom3>>1, smear=false) 对称。
 * 原生怪癖照抄:x2 上限用 rows-1、y1 上限用 cols-1(坐标交叉);
 * abs8 参数 int8 隐式窄化。 */
static uint16_t fx2d_mode_coloredbursts(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);
    const uint32_t now = wledc_fx_now();

    if (seg->call == 0) {
        seg->aux0 = 0; /* SEGENV.aux0 = 0 */
    }

    const uint8_t num_lines = seg->intensity / 16 + 1;

    seg->aux0++; /* hue */
    fx2d_fade_to_black_by(seg, 40); /* 40 - check2*8,固化 0 */
    for (uint8_t i = 0; i < num_lines; i++) {
        const uint8_t x1 = wledc_beatsin8((uint16_t)(2 + seg->speed / 16),
                                          0, (uint8_t)(cols - 1),
                                          now, 0, 0);
        const uint8_t x2 = wledc_beatsin8((uint16_t)(1 + seg->speed / 16),
                                          0, (uint8_t)(rows - 1),
                                          now, 0, 0); /* 怪癖:上限 rows-1 */
        const uint8_t y1 = wledc_beatsin8((uint16_t)(5 + seg->speed / 16),
                                          0, (uint8_t)(cols - 1),
                                          now, 0,
                                          (uint16_t)(i * 24)); /* 上限 cols-1 */
        const uint8_t y2 = wledc_beatsin8((uint16_t)(3 + seg->speed / 16),
                                          0, (uint8_t)(rows - 1),
                                          now, 0,
                                          (uint16_t)(i * 48 + 64));
        const uint32_t color =
            fx2d_palette_color(seg,
                               (uint8_t)(i * 255 / num_lines
                                         + (seg->aux0 & 0xFF)),
                               255);

        /* abs8 参数隐式窄化 int8,照原生怪癖 */
        const uint8_t xsteps =
            (uint8_t)(fx2d_abs8((int8_t)(x1 - y1)) + 1);
        const uint8_t ysteps =
            (uint8_t)(fx2d_abs8((int8_t)(x2 - y2)) + 1);
        const uint8_t steps = xsteps >= ysteps ? xsteps : ysteps;
        /* 逐线渐变绘制 */
        for (uint8_t j = 1; j <= steps; j++) {
            const uint8_t rate = (uint8_t)(j * 255 / steps);
            const uint8_t dx = (uint8_t)fx2d_lerp8by8(x1, y1, rate);
            const uint8_t dy = (uint8_t)fx2d_lerp8by8(x2, y2, rate);
            fx2d_add_pixel_color_xy(seg, dx, dy, color);
        }
    }
    const uint8_t bursts_blur = seg->custom3 >> 1;
    fx2d_blur(seg, bursts_blur, bursts_blur);
    return 0;
}

/* 槽 173 | FX_MODE_2DTARTAN | 原生 FX.cpp L6049-6079
 * 无 fade/blur;三参 beatsin16_t(3/2/10,...) 补 now/0/0;
 * -360 经 (uint16_t) 隐式转 65176,rangewidth 环绕后下游 sin8() 内
 * 参数隐式窄化 mod 256,与 -360 等价(原生怪癖照抄)。
 * size_t intensity 幂运算:循环乘 bri 后整体 >>= 8*sharpness 一次执行。 */
static uint16_t fx2d_mode_tartan(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);
    const uint32_t now = wledc_fx_now();

    if (seg->call == 0) {
        fx2d_fill(seg, WLEDC_BLACK);
    }

    uint8_t hue;
    uint8_t bri;
    size_t intensity;
    const int offset_x = (int)wledc_beatsin16(3, (uint16_t)-360, 360u,
                                               now, 0, 0);
    const int offset_y = (int)wledc_beatsin16(2, (uint16_t)-360, 360u,
                                               now, 0, 0);
    const uint8_t sharpness = seg->custom3 / 8; /* 0-3 */

    for (int x = 0; x < cols; x++) {
        for (int y = 0; y < rows; y++) {
            hue = (uint8_t)(x * wledc_beatsin16(10, 1, 10, now, 0, 0)
                            + offset_y);
            intensity = bri =
                wledc_sin8((uint8_t)(x * seg->speed / 2 + offset_x));
            for (uint8_t i = 0; i < sharpness; i++) {
                intensity *= bri;
            }
            intensity >>= 8 * sharpness;
            fx2d_set_pixel(seg, (uint16_t)x, (uint16_t)y,
                           fx2d_palette_color(seg, hue,
                                              (uint8_t)intensity));
            hue = (uint8_t)(y * 3 + offset_x);
            /* 第二段用 intensity 而非 speed,照原生 */
            intensity = bri =
                wledc_sin8((uint8_t)(y * seg->intensity / 2 + offset_y));
            for (uint8_t i = 0; i < sharpness; i++) {
                intensity *= bri;
            }
            intensity >>= 8 * sharpness;
            fx2d_add_pixel_color_xy(seg, (uint16_t)x, (uint16_t)y,
                                    fx2d_palette_color(seg, hue,
                                                       (uint8_t)intensity));
        }
    }
    return 0;
}

/* 槽 176 | FX_MODE_2DLISSAJOUS | 原生 FX.cpp L5681-5701
 * check1 固化 0:blur(custom1>>1, smear=false) 对称;
 * map 特判 (2*v+1)/2 四舍五入 + cols/rows<2 防除零,照原生;
 * phase 由运行时 custom3 控制转速;palette 索引 mcol=0。 */
static uint16_t fx2d_mode_lissajous(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);
    const uint32_t now = wledc_fx_now();

    fx2d_fade_to_black_by(seg, seg->intensity);
    const uint_fast16_t phase =
        (now * (uint32_t)(1 + seg->custom3)) / 32; /* custom3 控速 */

    for (int i = 0; i < 256; i++) {
        uint_fast8_t xlocn = wledc_sin8(
            (uint8_t)(phase / 2 + (i * seg->speed) / 32));
        uint_fast8_t ylocn = wledc_cos8(
            (uint8_t)(phase / 2 + i * 2));
        xlocn = (cols < 2) ? 1 :
            (fx2d_map(2 * (int32_t)xlocn, 0, 511, 0,
                      2 * (cols - 1)) + 1) / 2;
        ylocn = (rows < 2) ? 1 :
            (fx2d_map(2 * (int32_t)ylocn, 0, 511, 0,
                      2 * (rows - 1)) + 1) / 2;
        fx2d_set_pixel(seg, (uint8_t)xlocn, (uint8_t)ylocn,
                       fx2d_seg_palette_color(seg, now / 100 + i,
                                              false,
                                              seg->blend_mode == 1
                                              || seg->blend_mode == 3,
                                              0, 255));
    }
    const uint8_t liss_blur = seg->custom1 >> 1; /* 1 + check1*3,固化 0 */
    fx2d_blur(seg, liss_blur, liss_blur);
    return 0;
}

/* 槽 177 | FX_MODE_2DFRIZZLES | 原生 FX.cpp L5366-5379
 * check1 固化 0:fadeToBlackBy(16),blur(custom1>>3, smear=false) 对称;
 * 三参 beatsin8_t(12,0,255) 补 now/0/0;i 从 8 递减,照原生;
 * intensity/8 - i 为 int 可为负,bpm 隐式转 uint16 环绕(照原生)。 */
static uint16_t fx2d_mode_frizzles(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);
    const uint32_t now = wledc_fx_now();

    fx2d_fade_to_black_by(seg, 16); /* 16 + check1*10,固化 0 */
    for (uint8_t i = 8; i > 0; i--) {
        fx2d_add_pixel_color_xy(
            seg,
            wledc_beatsin8((uint16_t)(seg->speed / 8 + i), 0,
                           (uint8_t)(cols - 1), now, 0, 0),
            wledc_beatsin8((uint16_t)(seg->intensity / 8 - i), 0,
                           (uint8_t)(rows - 1), now, 0, 0),
            fx2d_palette_color(seg,
                               wledc_beatsin8(12, 0, 255, now, 0, 0),
                               255));
    }
    const uint8_t frizzles_blur = seg->custom1 >> 3; /* 3 + check1,固化 0 */
    fx2d_blur(seg, frizzles_blur, frizzles_blur);
    return 0;
}

/* 槽 180 | FX_MODE_2DHIPHOTIC | 原生 FX.cpp L5546-5558
 * 无 fade/blur;mcol=0;sin8/cos8 参数隐式窄化照原生。 */
static uint16_t fx2d_mode_hiphotic(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);
    const uint32_t now = wledc_fx_now();

    const uint32_t a = now / (uint32_t)((seg->custom3 >> 1) + 1);

    for (int x = 0; x < cols; x++) {
        for (int y = 0; y < rows; y++) {
            fx2d_set_pixel(seg, (uint16_t)x, (uint16_t)y,
                           fx2d_seg_palette_color(
                               seg,
                               wledc_sin8((uint8_t)(
                                   wledc_cos8((uint8_t)(
                                       x * seg->speed / 16 + a / 3))
                                   + wledc_sin8((uint8_t)(
                                       y * seg->intensity / 16 + a / 4))
                                   + a)),
                               false,
                               seg->blend_mode == 1
                               || seg->blend_mode == 3,
                               0, 255));
        }
    }
    return 0;
}

/* 槽 181 | FX_MODE_2DSINDOTS | 原生 FX.cpp L5943-5963
 * check1 固化 0:fadeToBlackBy(custom1>>3),blur(custom2>>3,smear=false) 对称;
 * t1 为 byte 窄化照原生(除数 257-speed>=2,无除零)。 */
static uint16_t fx2d_mode_sindots(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);
    const uint32_t now = wledc_fx_now();

    if (seg->call == 0) {
        fx2d_fill(seg, WLEDC_BLACK);
    }

    fx2d_fade_to_black_by(seg, seg->custom1 >> 3); /* + check1*24,固化 0 */

    /* max index now 255x15/255=15! */
    const uint8_t t1 =
        (uint8_t)(now / (uint32_t)(257 - seg->speed)); /* byte 窄化 */
    const uint8_t t2 = (uint8_t)(wledc_sin8(t1) / 4 * 2);
    for (int i = 0; i < 13; i++) {
        const int x = wledc_sin8((uint8_t)(t1 + i * seg->intensity / 8))
                      * (cols - 1) / 255;
        const int y = wledc_sin8((uint8_t)(t2 + i * seg->intensity / 8))
                      * (rows - 1) / 255;
        fx2d_set_pixel(seg, (uint16_t)x, (uint16_t)y,
                       fx2d_palette_color(seg,
                                          (uint8_t)(i * 255 / 13),
                                          255));
    }
    const uint8_t sindots_blur = seg->custom2 >> 3; /* 3 + check1,固化 0 */
    fx2d_blur(seg, sindots_blur, sindots_blur);
    return 0;
}

/* 槽 182 | FX_MODE_2DDNASPIRAL | 原生 FX.cpp L5261-5300
 * check1 固化 0:blur((custom1*3)/6, smear=false) 对称(分母 6+check1 固化 6);
 * speeds/freq 为 unsigned;防洞 dx 三目照抄;abs8 参数 int8 隐式窄化照抄。 */
static uint16_t fx2d_mode_dnaspiral(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);
    const uint32_t now = wledc_fx_now();

    if (seg->call == 0) {
        fx2d_fill(seg, WLEDC_BLACK);
    }

    const unsigned speeds = seg->speed / 2 + 7;
    const unsigned freq = seg->intensity / 8;

    const uint32_t ms = now / 20;
    fx2d_fade_to_black_by(seg, 135);

    for (int i = 0; i < rows; i++) {
        int x = wledc_beatsin8((uint16_t)speeds, 0, (uint8_t)(cols - 1),
                              now, 0, (uint16_t)(i * freq))
                + wledc_beatsin8((uint16_t)(speeds - 7), 0,
                                 (uint8_t)(cols - 1), now, 0,
                                 (uint16_t)(i * freq + 128));
        int x1 = wledc_beatsin8((uint16_t)speeds, 0, (uint8_t)(cols - 1),
                                now, 0, (uint16_t)(128 + i * freq))
                 + wledc_beatsin8((uint16_t)(speeds - 7), 0,
                                  (uint8_t)(cols - 1), now, 0,
                                  (uint16_t)(128 + 64 + i * freq));
        const unsigned hue = (unsigned)(i * 128 / rows) + ms;
        /* skip every 4th row every now and then (fade it more) */
        if ((i + ms / 8) & 3) {
            /* draw a gradient line between x and x1 */
            x = x / 2;
            x1 = x1 / 2;
            const unsigned steps =
                (unsigned)(fx2d_abs8((int8_t)(x - x1)) + 1);
            const bool positive = (x1 >= x); /* direction of drawing */
            for (size_t k = 1; k <= steps; k++) {
                const unsigned rate = (unsigned)(k * 255 / steps);
                /* behaves the same as "lerp8by8" but does not create holes */
                const unsigned dx = positive
                    ? (unsigned)(x + (int)k - 1)
                    : (unsigned)(x - (int)k + 1);
                fx2d_add_pixel_color_xy(seg, (uint16_t)dx, (uint16_t)i,
                                       fx2d_palette_color(seg,
                                                          (uint8_t)hue,
                                                          255));
                fx2d_fade_pixel_color_xy(seg, (uint16_t)dx, (uint16_t)i,
                                         (uint8_t)rate);
            }
            fx2d_set_pixel(seg, (uint16_t)x, (uint16_t)i,
                           FX2D_DARKSLATEGRAY);
            fx2d_set_pixel(seg, (uint16_t)x1, (uint16_t)i,
                           WLEDC_WHITE_RGB);
        }
    }
    /* 6 + check1,固化 0 → 分母 6 */
    const uint8_t spiral_blur =
        (uint8_t)(((uint16_t)seg->custom1 * 3) / 6);
    fx2d_blur(seg, spiral_blur, spiral_blur);
    return 0;
}

/* 槽 183 | FX_MODE_2DBLACKHOLE | 原生 FX.cpp L5161-5186
 * check1 固化 0:mcol=255(check1?0:255);check3 固化 0:结尾 blur 省略;
 * 内星 y 的 bpm 用 custom3 不 >>3,照原生;中心白点。 */
static uint16_t fx2d_mode_blackhole(wledc_segment_t *seg)
{
    if (!wledc_segment_is_2d(seg)) {
        fx2d_fill(seg, seg->colors[0]);
        return 0; /* FX_FALLBACK_STATIC */
    }

    const int cols = fx2d_vw(seg);
    const int rows = fx2d_vh(seg);
    const uint32_t now = wledc_fx_now();

    fx2d_fade_to_black_by(seg,
                          (uint8_t)(16 + (seg->speed >> 3))); /* fading trails */
    const uint32_t t = now / 128; /* timebase */

    /* outer stars */
    for (uint8_t i = 0; i < 8; i++) {
        const int x = wledc_beatsin8((uint16_t)(seg->custom1 >> 3), 0,
                                     (uint8_t)(cols - 1), now, 0,
                                     (uint16_t)(((i % 2) ? 128 : 0)
                                                + t * i));
        const int y = wledc_beatsin8((uint16_t)(seg->intensity >> 3), 0,
                                     (uint8_t)(rows - 1), now, 0,
                                     (uint16_t)(((i % 2) ? 192 : 64)
                                                + t * i));
        fx2d_add_pixel_color_xy(
            seg, (uint16_t)x, (uint16_t)y,
            fx2d_seg_palette_color(seg, i * 32, false,
                                   seg->blend_mode == 1
                                   || seg->blend_mode == 3,
                                   255, 255));
    }
    /* inner stars */
    for (uint8_t i = 0; i < 4; i++) {
        const int x = wledc_beatsin8((uint16_t)(seg->custom2 >> 3),
                                     (uint8_t)(cols / 4),
                                     (uint8_t)(cols - 1 - cols / 4),
                                     now, 0,
                                     (uint16_t)(((i % 2) ? 128 : 0)
                                                + t * i));
        const int y = wledc_beatsin8((uint16_t)seg->custom3, /* 不>>3,照抄 */
                                     (uint8_t)(rows / 4),
                                     (uint8_t)(rows - 1 - rows / 4),
                                     now, 0,
                                     (uint16_t)(((i % 2) ? 192 : 64)
                                                + t * i));
        fx2d_add_pixel_color_xy(
            seg, (uint16_t)x, (uint16_t)y,
            fx2d_seg_palette_color(seg, (uint16_t)(255 - i * 64), false,
                                   seg->blend_mode == 1
                                   || seg->blend_mode == 3,
                                   255, 255));
    }
    /* central white dot */
    fx2d_set_pixel(seg, (uint16_t)(cols / 2), (uint16_t)(rows / 2),
                   WLEDC_WHITE_RGB);
    /* blur everything a bit(check3 固化 0,原生省略) */
    return 0;
}

/* ================================================================ */
/* 73 槽效果表(槽号对齐 wled00/FX.h FX_MODE_* 114..186)               */
/* 已实现 13 槽;未实现槽为 NULL,由引擎 render_black 回退;            */
/* 槽内含非 2D 的 gap-fill/SR 枚举,照原生编号保留,一律 NULL          */
/* ================================================================ */

#define WLEDC_FX_2D_SLOT_COUNT 73u

static const wledc_fx_fn fx_2d_slot_table[WLEDC_FX_2D_SLOT_COUNT] = {
    NULL,                        /*  0 114 FX_MODE_2DPLASMAROTOZOOM(未移植) */
    NULL,                        /*  1 115 FX_MODE_BLENDS(gap-fill,非 2D) */
    NULL,                        /*  2 116 FX_MODE_TV_SIMULATOR(非 2D) */
    NULL,                        /*  3 117 FX_MODE_DYNAMIC_SMOOTH(非 2D) */
    NULL,                        /*  4 118 FX_MODE_2DSPACESHIPS(未移植) */
    NULL,                        /*  5 119 FX_MODE_2DCRAZYBEES(未移植) */
    NULL,                        /*  6 120 FX_MODE_2DGHOSTRIDER(未移植) */
    NULL,                        /*  7 121 FX_MODE_2DBLOBS(未移植) */
    NULL,                        /*  8 122 FX_MODE_2DSCROLLTEXT(未移植) */
    NULL,                        /*  9 123 FX_MODE_2DDRIFTROSE(未移植) */
    NULL,                        /* 10 124 FX_MODE_2DDISTORTIONWAVES(未移植) */
    NULL,                        /* 11 125 FX_MODE_2DSOAP(未移植) */
    NULL,                        /* 12 126 FX_MODE_2DOCTOPUS(未移植) */
    NULL,                        /* 13 127 FX_MODE_2DWAVINGCELL(未移植) */
    NULL,                        /* 14 128 FX_MODE_PIXELS(SR,未移植) */
    NULL,                        /* 15 129 FX_MODE_PIXELWAVE(SR,未移植) */
    NULL,                        /* 16 130 FX_MODE_JUGGLES(SR,未移植) */
    NULL,                        /* 17 131 FX_MODE_MATRIPIX(SR,未移植) */
    NULL,                        /* 18 132 FX_MODE_GRAVIMETER(SR,未移植) */
    NULL,                        /* 19 133 FX_MODE_PLASMOID(SR,未移植) */
    NULL,                        /* 20 134 FX_MODE_PUDDLES(SR,未移植) */
    NULL,                        /* 21 135 FX_MODE_MIDNOISE(SR,未移植) */
    NULL,                        /* 22 136 FX_MODE_NOISEMETER(SR,未移植) */
    NULL,                        /* 23 137 FX_MODE_FREQWAVE(SR,未移植) */
    NULL,                        /* 24 138 FX_MODE_FREQMATRIX(SR,未移植) */
    NULL,                        /* 25 139 FX_MODE_2DGEQ(未移植) */
    NULL,                        /* 26 140 FX_MODE_WATERFALL(SR,未移植) */
    NULL,                        /* 27 141 FX_MODE_FREQPIXELS(SR,未移植) */
    NULL,                        /* 28 142 FX_MODE_BINMAP(SR,未移植) */
    NULL,                        /* 29 143 FX_MODE_NOISEFIRE(SR,未移植) */
    NULL,                        /* 30 144 FX_MODE_PUDDLEPEAK(SR,未移植) */
    NULL,                        /* 31 145 FX_MODE_NOISEMOVE(SR,未移植) */
    NULL,                        /* 32 146 FX_MODE_2DNOISE(未移植) */
    NULL,                        /* 33 147 FX_MODE_PERLINMOVE(SR,未移植) */
    NULL,                        /* 34 148 FX_MODE_RIPPLEPEAK(SR,未移植) */
    NULL,                        /* 35 149 FX_MODE_2DFIRENOISE(未移植) */
    fx2d_mode_squaredswirl,      /* 36 150 FX_MODE_2DSQUAREDSWIRL */
    NULL,                        /* 37 151 FX_MODE_PACMAN(gap-fill,非 SR) */
    fx2d_mode_dna,               /* 38 152 FX_MODE_2DDNA */
    fx2d_mode_matrix,            /* 39 153 FX_MODE_2DMATRIX */
    NULL,                        /* 40 154 FX_MODE_2DMETABALLS(未移植) */
    NULL,                        /* 41 155 FX_MODE_FREQMAP(SR,未移植) */
    NULL,                        /* 42 156 FX_MODE_GRAVCENTER(SR,未移植) */
    NULL,                        /* 43 157 FX_MODE_GRAVCENTRIC(SR,未移植) */
    NULL,                        /* 44 158 FX_MODE_GRAVFREQ(SR,未移植) */
    NULL,                        /* 45 159 FX_MODE_DJLIGHT(SR,未移植) */
    NULL,                        /* 46 160 FX_MODE_2DFUNKYPLANK(未移植) */
    NULL,                        /* 47 161 FX_MODE_SHIMMER(gap-fill,非 2D) */
    fx2d_mode_pulser,            /* 48 162 FX_MODE_2DPULSER */
    NULL,                        /* 49 163 FX_MODE_BLURZ(SR,未移植) */
    fx2d_mode_drift,             /* 50 164 FX_MODE_2DDRIFT */
    NULL,                        /* 51 165 FX_MODE_2DWAVERLY(未移植) */
    NULL,                        /* 52 166 FX_MODE_2DSUNRADIATION(未移植) */
    fx2d_mode_coloredbursts,     /* 53 167 FX_MODE_2DCOLOREDBURSTS */
    NULL,                        /* 54 168 FX_MODE_2DJULIA(未移植) */
    NULL,                        /* 55 169 (原生无定义,已移除) */
    NULL,                        /* 56 170 (原生无定义,已移除) */
    NULL,                        /* 57 171 (原生无定义,已移除) */
    NULL,                        /* 58 172 FX_MODE_2DGAMEOFLIFE(未移植) */
    fx2d_mode_tartan,            /* 59 173 FX_MODE_2DTARTAN */
    NULL,                        /* 60 174 FX_MODE_2DPOLARLIGHTS(未移植) */
    NULL,                        /* 61 175 FX_MODE_2DSWIRL(未移植) */
    fx2d_mode_lissajous,         /* 62 176 FX_MODE_2DLISSAJOUS */
    fx2d_mode_frizzles,          /* 63 177 FX_MODE_2DFRIZZLES */
    NULL,                        /* 64 178 FX_MODE_2DPLASMABALL(未移植) */
    NULL,                        /* 65 179 FX_MODE_FLOWSTRIPE(非 2D) */
    fx2d_mode_hiphotic,          /* 66 180 FX_MODE_2DHIPHOTIC */
    fx2d_mode_sindots,           /* 67 181 FX_MODE_2DSINDOTS */
    fx2d_mode_dnaspiral,         /* 68 182 FX_MODE_2DDNASPIRAL */
    fx2d_mode_blackhole,         /* 69 183 FX_MODE_2DBLACKHOLE */
    NULL,                        /* 70 184 FX_MODE_WAVESINS(SR,未移植) */
    NULL,                        /* 71 185 FX_MODE_ROCKTAVES(SR,未移植) */
    NULL,                        /* 72 186 FX_MODE_2DAKEMI(未移植) */
};

const wledc_fx_fn *wledc_fx_2d_table(void)
{
    return fx_2d_slot_table;
}

size_t wledc_fx_2d_count(void)
{
    return WLEDC_FX_2D_SLOT_COUNT;
}

#endif /* WLEDC_ENABLE_2D */