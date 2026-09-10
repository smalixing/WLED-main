/**
 * @file wled_c_rmt.h
 * @brief RMT WS2812 输出驱动(ESP-IDF v5 新版 RMT TX + Encoder)。
 *
 * 数据链最底层模块(依赖规则:rmt_ws2812 ← bus ← strip)。
 * 接收已按目标色序重排、已完成亮度缩放的字节流,由 RMT 外设
 * 转换为 WS2812 时序输出。RGB(3 字节/像素)与 RGBW(4 字节/像素)
 * 对本层透明 —— 仅体现为字节流长度不同。
 *
 * 上层总线管理器(wled_c_bus,任务 5)负责颜色顺序重排、总线级
 * 亮度缩放与像素路由,并持有本驱动实例。
 *
 * 与 design.md 3.2 的签名差异:wledc_rmt_init 使用本层自有的
 * wledc_rmt_config_t 而非 wledc_bus_config_t —— 后者属于上层总线
 * 模块(任务 5 交付),底层驱动不应反向依赖上层类型;总线层将
 * 自行完成 wledc_bus_config_t → wledc_rmt_config_t 的映射。
 */
#ifndef WLED_C_RMT_H
#define WLED_C_RMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** RMT 计数器分辨率:12.5MHz → 1 tick = 80ns(design.md 3.2)。 */
#define WLEDC_RMT_RESOLUTION_HZ 12500000

/* ---- WS2812 时序(tick = 80ns) ---- */
#define WLEDC_RMT_T0H_TICKS    5    /*!< bit0 高电平 400ns */
#define WLEDC_RMT_T0L_TICKS    11   /*!< bit0 低电平 880ns(规格 850ns,容差内) */
#define WLEDC_RMT_T1H_TICKS    10   /*!< bit1 高电平 800ns */
#define WLEDC_RMT_T1L_TICKS    6    /*!< bit1 低电平 480ns(规格 450ns,容差内) */
/*!< 两种位宽均为 16 tick = 1.28us,与 WS2812 1.25us±600ns 周期兼容 */
#define WLEDC_RMT_RESET_TICKS  3750 /*!< 复位码低电平 ≥300us(3750 × 80ns) */

/** RMT 驱动配置(由总线层从 wledc_bus_config_t 映射生成)。 */
typedef struct {
  gpio_num_t gpio;   /*!< 输出引脚 */
  uint16_t   length; /*!< 像素数(记录用;实际传输长度由 show() 的 len 决定) */
  bool       rgbw;   /*!< true:4 字节/像素(仅记录,不影响驱动行为) */
} wledc_rmt_config_t;

/** RMT 驱动实例(由总线层持有并管理生命周期)。 */
typedef struct {
  rmt_channel_handle_t channel; /*!< RMT TX 通道句柄 */
  rmt_encoder_handle_t encoder; /*!< WS2812 组合编码器(bytes + reset) */
  wledc_rmt_config_t   config;  /*!< 创建时的配置快照 */
} wledc_rmt_t;

/**
 * @brief 初始化 RMT TX 通道并创建 WS2812 编码器。
 *
 * 创建 12.5MHz 分辨率的 TX 通道 + 组合编码器(bytes_encoder 承载
 * 数据位,copy_encoder 承载复位码),并使能通道。
 *
 * @param dev 驱动实例(由调用方分配)
 * @param cfg 配置(引脚/像素数/是否 RGBW)
 * @return ESP_OK 成功;其他值失败(通道/编码器创建错误)
 */
esp_err_t wledc_rmt_init(wledc_rmt_t *dev, const wledc_rmt_config_t *cfg);

/**
 * @brief 输出一帧像素字节流。
 *
 * 防撕裂策略(design.md 3.2):先查询上一帧(含复位码)是否已完全
 * 移出,未完成则跳过本帧返回 ESP_ERR_TIMEOUT,由上层决定降级。
 * 完成后以 rmt_transmit 后台传输,不阻塞 CPU。
 *
 * @param dev    驱动实例
 * @param pixels 已按色序重排的字节流
 * @param len    字节数(3 或 4 的整数倍)
 * @return ESP_OK;ESP_ERR_TIMEOUT 上一帧未完成跳帧;其他为参数/传输错误
 */
esp_err_t wledc_rmt_show(wledc_rmt_t *dev, const uint8_t *pixels, size_t len);

/**
 * @brief 释放通道与编码器资源,清零实例。
 *
 * @param dev 驱动实例
 * @return ESP_OK;ESP_ERR_INVALID_ARG 参数为空
 */
esp_err_t wledc_rmt_deinit(wledc_rmt_t *dev);

#ifdef __cplusplus
}
#endif
#endif /* WLED_C_RMT_H */