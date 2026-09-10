/**
 * @file wled_c_color.h
 * @brief WLED-C 色彩处理纯函数模块
 *
 * 提供颜色空间转换(HSV<->RGB)、颜色混合/叠加/缩放、
 * 伽马校正查找表以及色温(Kelvin)转换等纯函数工具集。
 * 所有函数均为无外设依赖的纯计算函数,可在主机端编译运行。
 *
 * 颜色统一使用 32 位打包格式 0xWWRRGGBB(参见 wled_c_types.h 中的
 * WLEDC_RGBW32 / WLEDC_R / WLEDC_G / WLEDC_B / WLEDC_W 宏)。
 */

#ifndef WLED_C_COLOR_H
#define WLED_C_COLOR_H

#include <stdint.h>
#include <stdbool.h>
#include "wled_c/wled_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 32 位 HSV 颜色,16 位色相高精度
 *
 * 对应 WLED 的 CHSV32 结构。hue 取值 0-65535(0-360 度),
 * sat/val 取值 0-255。
 */
typedef struct {
    uint16_t h; /**< 色相,16 位 (0-65535,环回) */
    uint8_t  s; /**< 饱和度 (0-255) */
    uint8_t  v; /**< 明度 (0-255) */
} wledc_hsv32_t;

/* -------------------------------------------------------------------- */
/* HSV <-> RGB 转换                                                      */
/* -------------------------------------------------------------------- */

/**
 * @brief HSV(16 位 hue)转 RGB,频谱分布,精确线性算法
 *
 * 对应 WLED 的 hsv2rgb_spectrum(CHSV32, CRGBW)。速度优化的整数实现。
 * 白色通道输出恒为 0。
 *
 * @param hsv  输入 HSV(16 位 hue)
 * @param rgbw 输出 32 位打包 RGBW 颜色(0xWWRRGGBB)
 */
void wledc_hsv2rgb_spectrum(const wledc_hsv32_t *hsv, uint32_t *rgbw);

/**
 * @brief HSV(16 位 hue)转 RGB,彩虹分布,视觉平滑近似算法
 *
 * 对应 WLED/FastLED 的 hsv2rgb_rainbow。本实现为自研重写的分段
 * 线性彩虹近似算法,效果与频谱转换相比色调过渡更自然。
 * 白色通道输出恒为 0。
 *
 * @param hsv  输入 HSV(16 位 hue)
 * @param rgbw 输出 32 位打包 RGBW 颜色(0xWWRRGGBB)
 */
void wledc_hsv2rgb_rainbow(const wledc_hsv32_t *hsv, uint32_t *rgbw);

/**
 * @brief RGB 转 HSV(16 位 hue)
 *
 * 对应 WLED 的 rgb2hsv。不是 100% 颜色精确(整数近似)。
 * 黑色输入返回 h=s=v=0;灰色输入 hue 未定义,置 0。
 *
 * @param rgbw 输入 32 位打包 RGBW 颜色(忽略白色通道)
 * @param hsv  输出 HSV(16 位 hue)
 */
void wledc_rgb2hsv(uint32_t rgbw, wledc_hsv32_t *hsv);

/* -------------------------------------------------------------------- */
/* 颜色混合 / 叠加 / 缩放                                                 */
/* -------------------------------------------------------------------- */

/**
 * @brief 线性混合两个颜色
 *
 * 对应 WLED 的 color_blend。每通道计算:
 * result = (C1*(256-blend) + C2*blend) / 256
 * 使用双通道位运算(poorman's SIMD)一次处理 R+B 与 W+G。
 *
 * @param color1 第一个颜色(0xWWRRGGBB)
 * @param color2 第二个颜色(0xWWRRGGBB)
 * @param blend  混合因子 0-255(0=color1, 255=color2)
 * @return 混合后的颜色
 */
uint32_t wledc_color_blend(uint32_t color1, uint32_t color2, uint8_t blend);

/**
 * @brief 线性混合两个颜色,16 位混合因子
 *
 * 对应 WLED 的 color_blend16,内部将 16 位因子右移 8 位后调用
 * wledc_color_blend()。
 */
uint32_t wledc_color_blend16(uint32_t color1, uint32_t color2, uint16_t blend);

/**
 * @brief 叠加两个颜色(饱和加法)
 *
 * 对应 WLED 的 color_add。
 *
 * @param c1         第一个颜色(0xWWRRGGBB)
 * @param c2         第二个颜色(0xWWRRGGBB)
 * @param preserve_cr true: 溢出时按最大通道值等比缩放以保持色彩比例;
 *                    false: 每通道独立饱和到 255
 * @return 叠加后的颜色
 */
uint32_t wledc_color_add(uint32_t c1, uint32_t c2, bool preserve_cr);

/**
 * @brief 将颜色向黑色衰减
 *
 * 对应 WLED 的 color_fade。
 *
 * @param color   输入颜色(0xWWRRGGBB)
 * @param amount  衰减系数 0-255(0=全黑, 255=不变)
 * @param video   true: video 模式,非零通道保持不低于 1(~25% 阈值
 *                内的通道保持值,防止暗色被洗白为灰);
 *                false: 普通缩放 ((amount+1)>>8)
 * @return 衰减后的颜色
 */
uint32_t wledc_color_fade(uint32_t color, uint8_t amount, bool video);

/**
 * @brief 快速四通道缩放 c*scale/256
 *
 * 对应 WLED 的 fast_color_scale。速度优先于精度。
 */
uint32_t wledc_fast_color_scale(uint32_t color, uint8_t scale);

/* -------------------------------------------------------------------- */
/* 色温 (Kelvin) 转换                                                     */
/* -------------------------------------------------------------------- */

/**
 * @brief 由色温(K)计算白光谱 RGB
 *
 * 对应 WLED 的 colorKtoRGB,基于 Tanner Helland 的温度换色算法。
 *
 * @param kelvin 色温(0-65535 K,有效范围约 1000-40000)
 * @param rgb    输出数组:rgb[0]=R, rgb[1]=G, rgb[2]=B, rgb[3]=W(恒 0)
 */
void wledc_color_kto_rgb(uint16_t kelvin, uint8_t rgb[4]);

/**
 * @brief 按色温(K)校正 RGB 颜色
 *
 * 对应 WLED 的 colorBalanceFromKelvin。将输入颜色的 RGB 通道乘以
 * 该色温对应的白光谱系数。内部缓存上次色温结果以避免重复计算。
 * 注意:内部含静态缓存,非可重入。
 *
 * @param kelvin 色温(有效范围 2800-10200)
 * @param rgb    输入 32 位打包颜色(0xWWRRGGBB)
 * @return 校正后的颜色(白色通道原样保留)
 */
uint32_t wledc_color_balance_from_kelvin(uint16_t kelvin, uint32_t rgb);

/* -------------------------------------------------------------------- */
/* 伽马校正                                                              */
/* -------------------------------------------------------------------- */

/**
 * @brief 重算并填充伽马查找表
 *
 * 对应 WLED 的 NeoGammaWLEDMethod::calcGammaTable。
 * 首次使用前必须调用(如 gamma=2.8)。
 *
 * @param gamma 伽马值(如 2.8)
 */
void wledc_calc_gamma_table(float gamma);

/**
 * @brief 伽马校正 8 位值(正向表)
 *
 * 对应 WLED 的 NeoGammaWLEDMethod::Correct。
 * 若伽马校正被 wledc_set_gamma_correction(false) 关闭则原样返回。
 *
 * @param value 输入值 0-255
 * @return 校正后的值 0-255
 */
uint8_t wledc_gamma_correct(uint8_t value);

/**
 * @brief 反向伽马校正 32 位颜色(逆表)
 *
 * 对应 WLED 的 NeoGammaWLEDMethod::inverseGamma32。
 * 若伽马校正被关闭则原样返回。
 *
 * @param color 输入颜色(0xWWRRGGBB)
 * @return 反向校正后的颜色
 */
uint32_t wledc_inverse_gamma32(uint32_t color);

/**
 * @brief 启用/禁用伽马校正(默认启用)
 */
void wledc_set_gamma_correction(bool enable);

/**
 * @brief 查询伽马校正是否启用
 */
bool wledc_get_gamma_correction(void);

#ifdef __cplusplus
}
#endif

#endif /* WLED_C_COLOR_H */