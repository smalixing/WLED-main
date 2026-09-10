/**
 * @file wled_c_palette.h
 * @brief WLED 调色板模块(纯 C 实现)
 *
 * 对应 WLED 的 palettes.cpp(数据源)、colors.cpp 的 ColorFromPalette()、
 * fastled_slim.h 的 loadDynamicGradientPalette() 与 fill_gradient_RGB()。
 *
 * 调色板 ID 体系与 WLED const.h 保持一致:
 *  - ID 0-5    : 动态调色板(依赖运行时主色/副色状态,由上层效果模块实现)
 *  - ID 6-12   : FastLED / gc22 调色板(内部存储为 16x3 字节)
 *  - ID 13-71  : 渐变调色板(内部存储为 4 字节停色点: 位置,R,G,B)
 *  - ID 72-200 : 自定义调色板(wledc_palette_register() 注册,从 200 向下生长)
 *
 * 渐变调色板装载路径与 WLED 完全一致(FX_fcn.cpp Segment::setPalette):
 * 4 字节停色点数组经 wledc_load_gradient_palette() 转换为 16x3 entries 后,
 * 再由 wledc_color_from_palette() 取色。
 */

#ifndef WLED_C_PALETTE_H
#define WLED_C_PALETTE_H

#include <stdint.h>
#include <stdbool.h>

#include "wled_c/wled_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* 调色板 ID 常量(对应 WLED const.h)                                        */
/* ------------------------------------------------------------------------ */

#define WLEDC_PALETTE_DYNAMIC_COUNT    6   /*!< 动态调色板数(ID 0-5) */
#define WLEDC_PALETTE_FASTLED_COUNT    7   /*!< FastLED/gc22 调色板数(ID 6-12) */
#define WLEDC_PALETTE_GRADIENT_COUNT   59  /*!< 渐变调色板数(ID 13-71) */
#define WLEDC_PALETTE_FIXED_COUNT      72  /*!< 固定调色板总数(ID 0-71) */
#define WLEDC_PALETTE_CUSTOM_ID_BASE   200 /*!< 自定义调色板 ID 基数(向下生长) */
#define WLEDC_PALETTE_CUSTOM_MAX       129 /*!< 最多 129 个自定义调色板(ID 72-200) */

/*! 渐变停色点数组最大字节数(18 项 x 4 字节,与 WLED memcpy_P 72 字节一致) */
#define WLEDC_GRADIENT_STOPS_MAX_BYTES 72

/*! 16x3 entries 调色板字节数 */
#define WLEDC_PALETTE_ENTRIES_BYTES    48

/* ------------------------------------------------------------------------ */
/* 混合类型(对应 FastLED TBlendType)                                        */
/* ------------------------------------------------------------------------ */

typedef enum {
    WLEDC_BLEND_NONE           = 0, /*!< 无插值,直接取整槽颜色 */
    WLEDC_BLEND_LINEAR         = 1, /*!< 线性插值(末尾回绕) */
    WLEDC_BLEND_LINEAR_NOWRAP  = 2, /*!< 线性插值(重映射防末尾回绕) */
} wledc_blend_t;

/* ------------------------------------------------------------------------ */
/* 调色板存储格式                                                            */
/* ------------------------------------------------------------------------ */

typedef enum {
    WLEDC_PALETTE_FMT_NONE     = 0, /*!< 无效/未注册 */
    WLEDC_PALETTE_FMT_16X3     = 1, /*!< 16x3 字节 entries(FastLED/gc22) */
    WLEDC_PALETTE_FMT_GRADIENT = 2, /*!< 4 字节停色点(位置,R,G,B;末项位置=255) */
} wledc_palette_fmt_t;

/* ------------------------------------------------------------------------ */
/* 取色与装载                                                                */
/* ------------------------------------------------------------------------ */

/**
 * @brief 从 16x3 entries 调色板取色
 *
 * 对应 WLED colors.cpp 的 ColorFromPalette()。
 *
 * @param pal16x3   16x3 字节调色板 entries(可由 wledc_palette_get_entries() 获得)
 * @param index     取色索引(0-255)
 * @param brightness 亮度缩放(255 = 不缩放)
 * @param blend     混合类型
 * @return uint32_t WLEDC_RGBW32 格式颜色(W=0)
 */
uint32_t wledc_color_from_palette(const uint8_t *pal16x3, uint8_t index,
                                 uint8_t brightness, wledc_blend_t blend);

/**
 * @brief 将 4 字节停色点渐变调色板装载为 16x3 entries
 *
 * 对应 WLED fastled_slim.h 的 loadDynamicGradientPalette() +
 * fill_gradient_RGB()(含槽位防复用与 16.16 定点插值)。
 *
 * @param stops  4 字节停色点数组(位置,R,G,B;末项位置必须为 255)
 * @param out    输出缓冲区(48 字节)
 * @return true 装载成功;false 参数为空
 */
bool wledc_load_gradient_palette(const uint8_t *stops, uint8_t out[WLEDC_PALETTE_ENTRIES_BYTES]);

/* ------------------------------------------------------------------------ */
/* 固定调色板查询                                                            */
/* ------------------------------------------------------------------------ */

/**
 * @brief 查询调色板存储格式
 *
 * @param id 调色板 ID
 * @return 存储格式;固定 ID 越界或未注册自定义 ID 返回 WLEDC_PALETTE_FMT_NONE
 */
wledc_palette_fmt_t wledc_palette_get_format(wledc_palette_id_t id);

/**
 * @brief 获取固定调色板原始数据指针
 *
 * @param id 调色板 ID(6-71 固定)
 * @return 原始数据指针(FastLED/gc22 为 16x3,渐变为 4 字节停色点);
 *         ID 0-5(WLED 动态区间)与非法 ID 回退到 PartyColors(ID 6)
 */
const uint8_t *wledc_palette_get_data(wledc_palette_id_t id);

/**
 * @brief 获取任意 ID 调色板的 16x3 entries(供取色前一次性装载)
 *
 * FastLED/gc22 调色板直接拷贝 48 字节;渐变调色板经
 * wledc_load_gradient_palette() 转换。
 *
 * @param id  调色板 ID(固定或自定义)
 * @param out 输出缓冲区(48 字节)
 * @return true 成功;false ID 无效
 */
bool wledc_palette_get_entries(wledc_palette_id_t id,
                               uint8_t out[WLEDC_PALETTE_ENTRIES_BYTES]);

/**
 * @brief 获取调色板名称(0-71 与 WLED JSON_palette_names 一致)
 *
 * 注意:名称表 22"Beach"/26"Beech" 与数据表的互换是 WLED 原生错位,
 * 本模块保持同样行为以保真;ID 与数据的映射以数组顺序为权威。
 *
 * @param id 调色板 ID
 * @return 名称字符串;自定义调色板返回 "Custom",非法 ID 返回 "Party"
 */
const char *wledc_palette_name(wledc_palette_id_t id);

/* ------------------------------------------------------------------------ */
/* 自定义调色板注册                                                          */
/* ------------------------------------------------------------------------ */

/**
 * @brief 注册自定义渐变调色板
 *
 * 对应 WLED 的 JSON API 自定义调色板注册(FX_fcn.cpp customPalettes)。
 * ID 从 200 向下生长,最多 WLEDC_PALETTE_CUSTOM_MAX 个。
 *
 * @param stops16 4 字节停色点数组(位置,R,G,B;末项位置=255),
 *                生命周期须覆盖注销前(编译期常量或静态存储)
 * @return int 分配的调色板 ID(72-200);
 *         已满或参数为空时返回 WLEDC_ERR_NO_PALETTE(负值判断用 < 0)
 */
int wledc_palette_register(const uint8_t *stops16);

/**
 * @brief 注销最近注册的自定义调色板(按注册序号)
 *
 * @return true 成功;false 无已注册调色板
 */
bool wledc_palette_unregister_last(void);

/**
 * @brief 获取已注册自定义调色板数量
 *
 * @return uint8_t 数量(0-129)
 */
uint8_t wledc_palette_custom_count(void);

#ifdef __cplusplus
}
#endif

#endif /* WLED_C_PALETTE_H */