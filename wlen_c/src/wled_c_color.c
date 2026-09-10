/**
 * @file wled_c_color.c
 * @brief WLED-C 色彩处理纯函数模块实现
 *
 * 算法行为对齐 WLED 的 colors.cpp / colors.h:
 *  - 双通道位运算(poorman's SIMD,掩码 0x00FF00FF)一次处理 R+B / W+G
 *  - 16 位 hue 的 HSV<->RGB 转换
 *  - 伽马正/逆查找表
 *  - Tanner Helland 色温换色算法
 * 全部以纯 C 重写,不移植第三方源代码,无任何外设依赖。
 */

#include "wled_c/wled_c_color.h"

#include <math.h>

/** 同时选中 R、B 两个字节(取反后选中 W、G)的双通道掩码。 */
#define TWO_CHANNEL_MASK 0x00FF00FFu

/* -------------------------------------------------------------------- */
/* HSV <-> RGB 转换                                                      */
/* -------------------------------------------------------------------- */

/**
 * 频谱(spectrum)分布转换:每个 60 度扇区内一个通道从 v 线性降到 (v*(255-s))/255,
 * 另一个从 (v*(255-s))/255 升到 v,第三个保持 (v*(255-s))/255。
 * WLED 注释:转换速度优化,时间关键代码请优先使用 rainbow 版本。
 */
void wledc_hsv2rgb_spectrum(const wledc_hsv32_t *hsv, uint32_t *rgbw)
{
    unsigned p, q, t;
    unsigned region = ((unsigned)hsv->h * 6u) >> 16; /* h / (65536/6) */
    unsigned remainder = ((unsigned)hsv->h - (region * 10923u)) * 6u;
    unsigned r, g, b;

    /* 零饱和度:灰阶 */
    if (hsv->s == 0) {
        *rgbw = WLEDC_RGBW32(hsv->v, hsv->v, hsv->v, 0);
        return;
    }

    p = ((unsigned)hsv->v * (255u - hsv->s)) >> 8;
    q = ((unsigned)hsv->v * (255u - ((unsigned)hsv->s * remainder) >> 16)) >> 8;
    t = ((unsigned)hsv->v * (255u - ((unsigned)hsv->s * (65535u - remainder)) >> 16)) >> 8;

    switch (region) {
    case 0:  r = hsv->v; g = t; b = p; break; /* 红 -> 黄 */
    case 1:  r = q; g = hsv->v; b = p; break; /* 黄 -> 绿 */
    case 2:  r = p; g = hsv->v; b = t; break; /* 绿 -> 青 */
    case 3:  r = p; g = q; b = hsv->v; break; /* 青 -> 蓝 */
    case 4:  r = t; g = p; b = hsv->v; break; /* 蓝 -> 品红 */
    default: r = hsv->v; g = p; b = q; break; /* 品红 -> 红 */
    }
    *rgbw = WLEDC_RGBW32((uint8_t)r, (uint8_t)g, (uint8_t)b, 0);
}

/**
 * 彩虹(rainbow)分布转换:自研实现。
 * 将色环分为 8 个 45 度子段(每段 8192 个 hue 单位),每段内一个通道为 255、
 * 一个为 0,第三个通道以 1/3(85)、2/3(170)为拐点的分段线性方式过渡,
 * 近似正弦曲线,视觉上比频谱分布更平滑。
 */
void wledc_hsv2rgb_rainbow(const wledc_hsv32_t *hsv, uint32_t *rgbw)
{
    /* 每个子段 8192 个 hue 单位(65536/8);offset 为段内偏移(0-8191) */
    const unsigned SUB_BITS   = 13u;                   /* log2(8192) */
    const unsigned SUB_UNITS  = 1u << SUB_BITS;        /* 8192 */
    const unsigned SUBS_TOTAL = 65536u / SUB_UNITS;    /* 8 个子段 */
    unsigned seg    = ((unsigned)hsv->h) / SUB_UNITS;  /* 0..7 */
    unsigned offset = ((unsigned)hsv->h) & (SUB_UNITS - 1u);
    unsigned third16    = offset * 21846u;             /* 段内 1/3<<16 */
    unsigned third      = third16 >> 21;               /* 0..85 */
    unsigned twothirds  = third16 >> 20;               /* 0..170 */
    unsigned r, g, b;

    if (seg >= SUBS_TOTAL) seg = SUBS_TOTAL - 1u;      /* 防御:h==65535+ */

    switch (seg) {
    case 0:  r = 255u - third; g = third;        b = 0;                break;
    case 1:  r = 171u;          g = 85u + third;  b = 0;                break;
    case 2:  r = 171u - twothirds; g = 170u + third; b = 0;              break;
    case 3:  r = 0;             g = 255u - third; b = third;             break;
    case 4:  r = 0;             g = 171u - twothirds; b = 85u + twothirds; break;
    case 5:  r = third;         g = 0;            b = 255u - third;      break;
    case 6:  r = 85u + third;   g = 0;            b = 171u - third;      break;
    default: r = 170u + third;  g = 0;            b = 85u - third;       break;
    }

    /* 降饱和:同时向白色靠拢并压低彩度 */
    if (hsv->s != 255u) {
        if (hsv->s == 0u) {
            r = 255u; g = 255u; b = 255u;
        } else {
            unsigned desat = 255u - hsv->s;
            desat *= desat;                       /* desat^2,近似视觉曲线 */
            unsigned brightness_floor = desat >> 8;
            unsigned satscale = 0xFFFFu - desat;
            if (r) r = (r * satscale) >> 16;
            if (g) g = (g * satscale) >> 16;
            if (b) b = (b * satscale) >> 16;
            r += brightness_floor;
            g += brightness_floor;
            b += brightness_floor;
        }
    }

    /* 明度缩放:val^2 近似视觉曲线,并保证非零输出不低于 1 */
    if (hsv->v != 255u) {
        if (hsv->v == 0u) {
            r = 0u; g = 0u; b = 0u;
        } else {
            unsigned val = (unsigned)hsv->v * hsv->v + 512u;
            if (r) r = ((r * val) >> 16) + 1u;
            if (g) g = ((g * val) >> 16) + 1u;
            if (b) b = ((b * val) >> 16) + 1u;
        }
    }

    *rgbw = WLEDC_RGBW32((uint8_t)r, (uint8_t)g, (uint8_t)b, 0);
}

/**
 * RGB 转 HSV(16 位 hue)。整数近似,非 100% 颜色精确。
 * 黑色返回 0;灰色 hue 未定义,置 0。
 */
void wledc_rgb2hsv(uint32_t rgbw, wledc_hsv32_t *hsv)
{
    int32_t r = WLEDC_R(rgbw); /* 32 位变量比 8 位更快(编译器优化空间) */
    int32_t g = WLEDC_G(rgbw);
    int32_t b = WLEDC_B(rgbw);
    uint32_t minval, maxval;
    int32_t delta;

    /* 求 min/max:比调用 min/max 函数更快 */
    maxval = (r > g) ? ((r > b) ? (uint32_t)r : (uint32_t)b)
                     : ((g > b) ? (uint32_t)g : (uint32_t)b);
    if (maxval == 0u) {
        hsv->h = 0; hsv->s = 0; hsv->v = 0;
        return; /* 黑色,避免除零 */
    }
    minval = (r < g) ? ((r < b) ? (uint32_t)r : (uint32_t)b)
                     : ((g < b) ? (uint32_t)g : (uint32_t)b);
    hsv->v = (uint8_t)maxval;
    delta = (int32_t)maxval - (int32_t)minval;
    if (delta != 0) {
        hsv->s = (uint8_t)((255 * delta) / maxval);
        /* 10923 = 65536/6;省略 s==0 早退可提升速度(灰阶极少使用) */
        if (maxval == (uint32_t)r)      hsv->h = (uint16_t)((10923 * (g - b)) / delta);
        else if (maxval == (uint32_t)g) hsv->h = (uint16_t)(21845 + (10923 * (b - r)) / delta);
        else                            hsv->h = (uint16_t)(43690 + (10923 * (r - g)) / delta);
    } else {
        hsv->s = 0;
        hsv->h = 0; /* 灰色,hue 未定义,为一致性置 0 */
    }
}

/* -------------------------------------------------------------------- */
/* 颜色混合 / 叠加 / 缩放                                                */
/* -------------------------------------------------------------------- */

/**
 * 线性混合两种颜色。blend=0 返回 color1,blend=255 返回 color2。
 * 等效于 (c1*(256-blend)+c2*blend)/256,双通道并行处理。
 */
uint32_t wledc_color_blend(uint32_t color1, uint32_t color2, uint8_t blend)
{
    uint32_t rb1 = color1 & TWO_CHANNEL_MASK;
    uint32_t wg1 = (color1 >> 8) & TWO_CHANNEL_MASK;
    uint32_t rb2 = color2 & TWO_CHANNEL_MASK;
    uint32_t wg2 = (color2 >> 8) & TWO_CHANNEL_MASK;

    uint32_t rb3 = ((((rb1 << 8) | rb2) + (rb2 * blend) - (rb1 * blend)) >> 8) & TWO_CHANNEL_MASK;
    /* wg 通道已在移位域中,无需再右移 */
    uint32_t wg3 = ((((wg1 << 8) | wg2) + (wg2 * blend) - (wg1 * blend))) & ~TWO_CHANNEL_MASK;

    return rb3 | wg3;
}

/** 16 位精度混合(取 blend 高 8 位)。 */
uint32_t wledc_color_blend16(uint32_t color1, uint32_t color2, uint16_t blend)
{
    return wledc_color_blend(color1, color2, (uint8_t)(blend >> 8));
}

/**
 * 颜色叠加(饱和加法)。
 * preserve_cr 为 true 时保持颜色比例(溢出后等比缩放,而非截断饱和);
 * 为 false 时使用 branchless 截断饱和(更快)。
 */
uint32_t wledc_color_add(uint32_t color1, uint32_t color2, bool preserve_cr)
{
    if (color1 == WLEDC_BLACK) return color2;
    if (color2 == WLEDC_BLACK) return color1;

    uint32_t rb = (color1 & TWO_CHANNEL_MASK) + (color2 & TWO_CHANNEL_MASK);
    uint32_t wg = ((color1 >> 8) & TWO_CHANNEL_MASK) + ((color2 >> 8) & TWO_CHANNEL_MASK);

    if (preserve_cr) {
        /* 等比缩放使最大通道回到 255,保持色彩比例 */
        uint32_t overflow = (rb | wg) & 0x01000100u;
        if (overflow) {
            uint32_t r = rb >> 16;
            uint32_t b = rb & 0xFFFFu;
            uint32_t w = wg >> 16;
            uint32_t g = wg & 0xFFFFu;
            uint32_t maxval = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
            maxval = (w > maxval) ? w : maxval;
            uint32_t scale = (255u << 8) / maxval;
            rb = ((rb * scale) >> 8) & TWO_CHANNEL_MASK;
            wg = (wg * scale) & ~TWO_CHANNEL_MASK;
        } else {
            wg <<= 8; /* 移回原位置 */
        }
    } else {
        /* branchless 截断饱和:溢出(>=256)通道置 255 */
        rb |= ((rb & 0x01000100u) - ((rb >> 8) & 0x00010001u)) & TWO_CHANNEL_MASK;
        wg |= ((wg & 0x01000100u) - ((wg >> 8) & 0x00010001u)) & TWO_CHANNEL_MASK;
        wg <<= 8;
    }

    return rb | wg;
}

/**
 * 颜色按亮度因子缩放。
 * video 为 true 时带四舍五入并保证暗通道不为 0(视频行业惯例,作可见性下限),
 * 且白色通道非零时保留白色;为 false 时为纯比例缩放。
 */
uint32_t wledc_color_fade(uint32_t color, uint8_t amount, bool video)
{
    if (color == WLEDC_BLACK || amount == 0u) return 0u; /* 黑色或全淡出 */
    if (amount == 255u) return color;                     /* 无变化 */

    uint32_t rb = color & TWO_CHANNEL_MASK;
    uint32_t wg = (color >> 8) & TWO_CHANNEL_MASK;
    uint32_t rb_scaled, wg_scaled;

    if (video) {
        rb_scaled = ((rb * amount + 0x007F007Fu) >> 8) & TWO_CHANNEL_MASK; /* R/B 缩放,加 0.5 舍入 */
        wg_scaled = (wg * amount + 0x007F007Fu) & ~TWO_CHANNEL_MASK;      /* W/G 缩放,加 0.5 舍入 */
        uint8_t r = (uint8_t)(rb >> 16);
        uint8_t g = (uint8_t)wg;
        uint8_t b = (uint8_t)rb;
        uint8_t w = (uint8_t)(wg >> 16); /* wg 为移位域 */
        uint8_t maxc = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b); /* 主通道(保色调) */
        maxc = (uint8_t)(maxc >> 2) + 1u; /* ~25% 阈值,+1 防极暗色被冲成灰 */
        rb_scaled |= r > maxc ? 0x00010000u : 0u;
        wg_scaled |= g > maxc ? 0x00000100u : 0u;
        rb_scaled |= b > maxc ? 0x00000001u : 0u;
        wg_scaled |= w ? 0x01000000u : 0u; /* 白色保留 */
    } else {
        rb_scaled = ((rb * (uint32_t)(amount + 1u)) >> 8) & TWO_CHANNEL_MASK;
        wg_scaled = (wg * (uint32_t)(amount + 1u)) & ~TWO_CHANNEL_MASK;
    }

    return rb_scaled | wg_scaled;
}

/**
 * 快速缩放:color*scale/256,四通道并行,速度优先而非精确。
 * 注意 scale=255 时并非恒等(255*255>>8=254),与 WLED 行为一致。
 */
uint32_t wledc_fast_color_scale(uint32_t c, uint8_t scale)
{
    uint32_t rb = (((c & TWO_CHANNEL_MASK) * scale) >> 8) & TWO_CHANNEL_MASK;
    uint32_t wg = (((c >> 8) & TWO_CHANNEL_MASK) * scale) & ~TWO_CHANNEL_MASK;
    return rb | wg;
}

/* ---- part3:色温与伽马 ---- */

/* -------------------------------------------------------------------- */
/* 色温 (Kelvin) 转换                                                     */
/* -------------------------------------------------------------------- */

/**
 * 由色温计算白光谱 RGB(Tanner Helland 算法,自研实现)。
 * 逐分支复刻 WLED colorKtoRGB 的数值行为:
 *  - temp<=66: r=255,g=round(99.4708025861*ln(temp)-161.1195681661)
 *    temp<=19 时 b=0,否则 b=round(138.5177312231*ln(temp-10)-305.0447927307)
 *  - temp>66: r=round(329.698727446*pow(temp-60,-0.1332047592)),
 *    g=round(288.1221695283*pow(temp-60,-0.0755148492)),b=255
 *  - 各通道 constrain 到 0-255,W 通道恒 0
 */
/* 浮点域钳位到 [0,255] 后四舍五入,规避 logf(0)=-inf 等极端值
 * 转换为 int 时的未定义行为;有效输入范围内与 WLED 的
 * constrain(round(x), 0, 255) 结果一致。 */
static uint8_t wledc_kto_clamp(float x)
{
    if (!(x > 0.0f)) return 0;   /* 负值 / -inf / NaN -> 0 */
    if (x > 255.0f) return 255;
    return (uint8_t)(x + 0.5f);  /* 非负值四舍五入 */
}

void wledc_color_kto_rgb(uint16_t kelvin, uint8_t rgb[4])
{
    float r, g, b;
    float temp = (float)kelvin / 100.0f;
    if (temp <= 66.0f) {
        r = 255.0f;
        g = 99.4708025861f * logf(temp) - 161.1195681661f;
        if (temp <= 19.0f) {
            b = 0.0f;
        } else {
            b = 138.5177312231f * logf(temp - 10.0f) - 305.0447927307f;
        }
    } else {
        r = 329.698727446f * powf(temp - 60.0f, -0.1332047592f);
        g = 288.1221695283f * powf(temp - 60.0f, -0.0755148492f);
        b = 255.0f;
    }
    rgb[0] = wledc_kto_clamp(r);
    rgb[1] = wledc_kto_clamp(g);
    rgb[2] = wledc_kto_clamp(b);
    rgb[3] = 0;
}

/**
 * 按色温校正 RGB。缓存上次色温的计算结果以避免每次 setPixelColor
 * 都运行较慢的 wledc_color_kto_rgb()。
 * 注意:内部含静态缓存,非可重入(与 WLED 原实现一致)。
 */
uint32_t wledc_color_balance_from_kelvin(uint16_t kelvin, uint32_t rgb)
{
    static uint8_t correction_rgb[4] = {0, 0, 0, 0};
    static uint16_t last_kelvin = 0;
    if (last_kelvin != kelvin) {
        wledc_color_kto_rgb(kelvin, correction_rgb);
    }
    last_kelvin = kelvin;

    uint8_t r = (uint8_t)(((uint16_t)correction_rgb[0] * WLEDC_R(rgb)) / 255);
    uint8_t g = (uint8_t)(((uint16_t)correction_rgb[1] * WLEDC_G(rgb)) / 255);
    uint8_t b = (uint8_t)(((uint16_t)correction_rgb[2] * WLEDC_B(rgb)) / 255);
    uint8_t w = WLEDC_W(rgb); /* 白色通道原样保留 */
    return WLEDC_RGBW32(r, g, b, w);
}

/* -------------------------------------------------------------------- */
/* 伽马校正                                                              */
/* -------------------------------------------------------------------- */

/* 伽马查找表(首次使用前须调用 wledc_calc_gamma_table 填充) */
static uint8_t gamma_t[256];
static uint8_t gamma_t_inv[256];

/* 伽马校正开关(对应 WLED 全局 gammaCorrectCol,默认启用) */
static bool gamma_correction = true;

/**
 * 重算并填充伽马查找表。
 * 正向:gamma_t[i]   = (i/255)^gamma * 255 + 0.5
 * 逆向:gamma_t_inv[i] = ((i-0.5)/255)^(1/gamma) * 255 + 0.5
 * 索引 0 恒为 0(避免极暗值产生非零底亮)。
 */
void wledc_calc_gamma_table(float gamma)
{
    float gamma_inv = 1.0f / gamma;
    for (int i = 1; i < 256; i++) {
        gamma_t[i]     = (uint8_t)(powf((float)i / 255.0f, gamma) * 255.0f + 0.5f);
        gamma_t_inv[i] = (uint8_t)(powf(((float)i - 0.5f) / 255.0f, gamma_inv) * 255.0f + 0.5f);
    }
    gamma_t[0] = 0;
    gamma_t_inv[0] = 0;
}

/** 伽马校正 8 位值(正向表),关闭时原样返回。 */
uint8_t wledc_gamma_correct(uint8_t value)
{
    if (!gamma_correction) return value;
    return gamma_t[value];
}

/** 反向伽马校正 32 位颜色(逆表),关闭时原样返回。 */
uint32_t wledc_inverse_gamma32(uint32_t color)
{
    if (!gamma_correction) return color;
    uint8_t w = gamma_t_inv[WLEDC_W(color)];
    uint8_t r = gamma_t_inv[WLEDC_R(color)];
    uint8_t g = gamma_t_inv[WLEDC_G(color)];
    uint8_t b = gamma_t_inv[WLEDC_B(color)];
    return WLEDC_RGBW32(r, g, b, w);
}

void wledc_set_gamma_correction(bool enable)
{
    gamma_correction = enable;
}

bool wledc_get_gamma_correction(void)
{
    return gamma_correction;
}
