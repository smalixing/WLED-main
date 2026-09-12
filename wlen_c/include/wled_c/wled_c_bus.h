/**
 * @file wled_c_bus.h
 * @brief 总线抽象与 BusManager(任务 5,design.md 3.3)。
 *
 * 总线层位于 RMT 驱动(任务 4)之上、灯带核心(任务 6 之后)之下,
 * 负责三件事:
 *  1. 像素路由 —— 按各总线 config.start/length 将全局像素索引映射
 *     到对应总线的本地缓冲(含 reversed 反转);
 *  2. 颜色顺序重排 —— show() 路径批量按 color_order(12 种排列)
 *     重排字节;
 *  3. 总线级亮度缩放 —— brightness(0-255)对每通道线性缩放。
 *
 * RMT/引脚细节对上层完全隐藏:上层只见 wledc_bus_config_t 与
 * set_pixel_color / get_pixel_color / set_brightness / show 接口。
 * 内部持有任务 4 交付的 wledc_rmt_t 实例并完成
 * wledc_bus_config_t → wledc_rmt_config_t 的映射。
 *
 * 总线像素以逻辑 0xWWRRGGBB(wled_c_types.h)存储;show() 输出前才
 * 做重排与缩放,写入路径零转换开销。
 *
 * 线程安全:暂不加锁(任务 12 统一互斥锁)。
 */
#ifndef WLED_C_BUS_H
#define WLED_C_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hal/gpio_types.h"

#include "wled_c_types.h"
#include "wled_c_rmt.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 颜色排列总数:6 种 RGB + 6 种 RGBW(wledc_color_order_t)。 */
#define WLEDC_COLOR_ORDER_COUNT 12

/** 总线类型(当前仅 RMT 驱动的 WS2812 家族,后续任务可扩展)。 */
typedef enum {
    WLEDC_BUS_WS2812      = 0, /**< 3 通道/像素,默认色序 GRB。 */
    WLEDC_BUS_WS2812_RGBW = 1, /**< SK6812 风格,4 通道/像素。 */
} wledc_bus_type_t;

/** 总线配置(design.md 3.3)。 */
typedef struct {
    wledc_bus_type_t type;    /**< 总线类型(决定 3/4 通道)。 */
    gpio_num_t pin;           /**< 数据输出引脚。 */
    uint16_t start;           /**< 该总线覆盖的全局起始像素。 */
    uint16_t length;          /**< 像素数(1..WLEDC_MAX_LEDS_PER_BUS)。 */
    bool reversed;            /**< true:本总线像素首尾反转路由。 */
    uint8_t color_order;      /**< wledc_color_order_t,0..11。 */
    uint8_t brightness;       /**< 总线级亮度 0-255(255 = 不缩放)。 */
} wledc_bus_config_t;

/** 总线运行时实例(RMT 细节由本模块内部管理)。 */
typedef struct {
    wledc_bus_config_t config; /**< 创建时的配置快照。 */
    bool active;               /**< 槽位占用标志。 */
    uint8_t channels;          /**< 3(RGB)或 4(RGBW)。 */
    uint32_t *pixels;          /**< length 项逻辑像素(0xWWRRGGBB)。 */
    uint8_t *out;              /**< show() 临时字节缓冲(length*channels)。 */
    wledc_rmt_t rmt;           /**< 底层 RMT 驱动实例(任务 4)。 */
} wledc_bus_t;

/* ------------------------------------------------------------------ */
/* BusManager:总线生命周期与管理(最多 WLEDC_MAX_BUSES = 4)            */
/* ------------------------------------------------------------------ */

/**
 * @brief 添加一条总线:校验配置、分配缓冲、初始化 RMT 通道。
 *
 * color_order 约束:RGB 总线须用 0..5,RGBW 总线须用 6..11
 * (RGBW 总线若配 RGB 序将丢失白色通道,故显式拒绝)。
 *
 * @param cfg    总线配置(内容拷贝保存)
 * @param out_id 输出分配的总线 ID(可 NULL,仅注册不关心 ID)
 * @return ESP_OK;ESP_ERR_INVALID_ARG 配置非法;
 *         WLEDC_ERR_NO_BUS 无空闲槽位;
 *         ESP_ERR_NO_MEM 缓冲分配失败;其他为 RMT 初始化错误
 */
esp_err_t wledc_bus_add(const wledc_bus_config_t *cfg, wledc_bus_id_t *out_id);

/**
 * @brief 移除总线:释放 RMT 资源与缓冲,槽位置为空闲。
 * @param id 总线 ID
 * @return ESP_OK;WLEDC_ERR_NO_BUS ID 不存在
 */
esp_err_t wledc_bus_remove(wledc_bus_id_t id);

/**
 * @brief 按越界安全的 ID 获取总线实例(仅活跃总线)。
 * @return 总线指针;ID 非法/未注册时 NULL
 */
wledc_bus_t *wledc_bus_get(wledc_bus_id_t id);

/**
 * @brief 获取全部活跃总线(用于上层遍历)。
 * @param count 输出活跃总线数(可 NULL)
 * @return 内部静态数组首地址,长度为 WLEDC_MAX_BUSES,
 *         活跃项以 bus->active 标记,请勿长期持有
 */
wledc_bus_t *wledc_bus_get_all(uint8_t *count);

/** 所有活跃总线覆盖的最大全局像素数(start+length 的最大值)。 */
uint16_t wledc_bus_total_pixels(void);

/* ------------------------------------------------------------------ */
/* 像素接口:全局索引路由(上层 Strip 直接使用)                        */
/* ------------------------------------------------------------------ */

/**
 * @brief 写入一个全局像素(按各总线 start/length 路由)。
 *
 * 落在多条总线范围内的像素会同时写入(WLED 同语义);
 * 不落在任何总线内则静默忽略(上层无感)。
 *
 * @param id    总线 ID
 * @param index 全局像素索引
 * @param color 逻辑颜色 0xWWRRGGBB
 * @return ESP_OK;ESP_ERR_INVALID_ARG 越出该总线范围;
 *         WLEDC_ERR_NO_BUS ID 不存在
 */
esp_err_t wledc_bus_set_pixel_color(wledc_bus_id_t id, uint16_t index, uint32_t color);

/**
 * @brief 读取一个全局像素的逻辑颜色(未缩放、未重排)。
 * @return 颜色值;越出范围或 ID 非法时返回 WLEDC_BLACK
 */
uint32_t wledc_bus_get_pixel_color(wledc_bus_id_t id, uint16_t index);

/** 设置总线级亮度(0-255;255 = 不缩放)。 */
esp_err_t wledc_bus_set_brightness(wledc_bus_id_t id, uint8_t brightness);

/** 将总线全部像素填为指定逻辑颜色(不输出)。 */
esp_err_t wledc_bus_clear(wledc_bus_id_t id, uint32_t color);

/* ------------------------------------------------------------------ */
/* 输出                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief 刷新所有活跃总线:批量完成亮度缩放与色序重排后交 RMT 输出。
 *
 * 逐总线:先在 out 缓冲内完成 length*channels 字节的重排/缩放,
 * 再调用 wledc_rmt_show。单总线上一帧未完成时该总线跳帧
 * (ESP_ERR_TIMEOUT),不影响其他总线。
 *
 * @return ESP_OK 全部成功(或跳帧);
 *         否则返回第一个失败总线的错误码
 */
esp_err_t wledc_bus_show(void);

#ifdef __cplusplus
}
#endif
#endif /* WLED_C_BUS_H */