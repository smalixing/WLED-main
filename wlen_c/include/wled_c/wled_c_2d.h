/**
 * @file wled_c_2d.h
 * @brief 2D 段虚拟寻址与坐标变换 —— WLED Segment 2D 几何的纯 C 化。
 *
 * 对齐 WLED 语义(考证:FX.h L470-540):
 *   - XY 寻址:pixels[x + y * vWidth],vWidth 为虚拟宽度;
 *   - virtualWidth  = (transpose ? height : width  + groupLen-1) / groupLen
 *     (grouping/spacing 折叠属任务 8/9,本模块仅折 transpose/mirror);
 *   - virtualHeight = transpose ? virtualWidth : height(mirrorY 折半);
 *   - transpose/mirrorY 为持久几何(持久区变换,寻址期生效),
 *     reverseY/reverseX 为显示期变换(WLED 在输出期处理,任务 11)。
 *
 * 与 WLED 的有意偏差:
 *   - reverseX/reverseY 坐标变换提供为独立辅助函数,不在本模块
 *     强制生效(WLED 在服务层 show 期统一处理,归属任务 11);
 *   - grouping/spacing 的虚拟折叠留待任务 8/9(效果引擎统一处理)。
 *
 * 全部 API 以 #ifdef WLEDC_ENABLE_2D 包裹;关闭时返回
 * WLEDC_ERR_NOT_2D,消费方据此降级为 1D。
 */
#ifndef WLED_C_2D_H
#define WLED_C_2D_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "wled_c_types.h"
#include "wled_c_segment.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef WLEDC_ENABLE_2D

/**
 * @brief 判断段是否为 2D 段。
 *
 * WLED 语义:stopY > startY 即 2D。
 */
bool wledc_segment_is_2d(const wledc_segment_t *seg);

/**
 * @brief 取段虚拟宽度(WLED virtualWidth,无 grouping 折叠)。
 *
 * transpose 时取物理高度,否则取物理宽度;SEG_OPT_MIRROR(X 镜像
 * 复制半幅)时折半 —— 对应 WLED MIRRORED 位(vWidth=(vWidth+1)/2)。
 *
 * @param seg 段
 * @param out_vw 输出虚拟宽度(像素数)
 * @return ESP_OK;WLEDC_ERR_NOT_2D 非 2D 段
 */
esp_err_t wledc_segment_virtual_width(const wledc_segment_t *seg,
                                      uint16_t *out_vw);

/**
 * @brief 取段虚拟高度(WLED virtualHeight,无 grouping 折叠)。
 *
 * 物理高度经 SEG_OPT_MIRROR_Y 折半((h+1)/2);transpose 时返回
 * virtual_width 的值(对应 WLED:TRANSPOSE 下 vHeight = vWidth)。
 *
 * @param seg 段
 * @param out_vh 输出虚拟高度(像素数)
 * @return ESP_OK;WLEDC_ERR_NOT_2D 非 2D 段
 */
esp_err_t wledc_segment_virtual_height(const wledc_segment_t *seg,
                                        uint16_t *out_vh);

/**
 * @brief 取 2D 虚拟坐标系内 (x,y) 的像素缓冲索引。
 *
 * 纯索引计算:pixels[x + y * vWidth],不做越界检查(供内部
 * 热路径使用;公开 set/get 系列做无符号越界防御)。
 */
static inline uint32_t wledc_2d_xy_index(uint16_t x, uint16_t y,
                                         uint16_t v_width)
{
    return (uint32_t)x + (uint32_t)y * (uint32_t)v_width;
}

/**
 * @brief 2D 写像素(虚拟坐标)。
 *
 * 越界(x >= vWidth 或 y >= vHeight)静默丢弃(WLED 行为:越界
 * setPixelColorXY 无操作);段 off 时不写但返回 ESP_OK。
 *
 * @return ESP_OK;WLEDC_ERR_NOT_2D 非 2D 段
 */
esp_err_t wledc_segment_set_pixel_color_xy(wledc_segment_t *seg,
                                           uint16_t x, uint16_t y,
                                           uint32_t color);

/**
 * @brief 2D 读像素(虚拟坐标);越界返回 0。
 */
uint32_t wledc_segment_get_pixel_color_xy(const wledc_segment_t *seg,
                                          uint16_t x, uint16_t y);

/**
 * @brief 显示期坐标变换:reverseY(上下翻转行)。
 *
 * WLED 在输出期统一处理(reverse 选项不参与效果寻址),
 * y' = vHeight - 1 - y。归任务 11 调用,此处提供纯函数。
 */
static inline uint16_t wledc_2d_reverse_y(uint16_t y, uint16_t v_height)
{
    return (uint16_t)(v_height - 1u - y);
}

/**
 * @brief 显示期坐标变换:reverseX(左右翻转列)。
 *
 * x' = vWidth - 1 - x;同 reverseY 为输出期纯函数。
 */
static inline uint16_t wledc_2d_reverse_x(uint16_t x, uint16_t v_width)
{
    return (uint16_t)(v_width - 1u - x);
}

#endif /* WLEDC_ENABLE_2D */

#ifdef __cplusplus
}
#endif

#endif /* WLED_C_2D_H */