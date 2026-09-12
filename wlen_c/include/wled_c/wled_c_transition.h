/**
 * @file wled_c_transition.h
 * @brief 段过渡(segment transition)—— WLED FX_fcn.cpp Transition 的纯 C 化。
 *
 * 对齐 WLED 语义(需求 7:效果/参数切换时平滑过渡):
 *   - start:捕获当前 colors[3]/opacity/cct/调色板(16x3 entries)快照;
 *   - update:progress = (now - start) * 0xFFFF / duration(0-65535),
 *     到 0xFFFF 时自动结束;
 *   - 取值:current_* 系列按 progress 在"起点快照"与"当前段参数"间
 *     插值(wledc_color_blend16),供绘制/输出层消费(任务 8/9/11);
 *   - 压缩重启:过渡中再次 start 时,快照先压缩为当前视觉混合值
 *     (color_blend16 避免跳变),再重启计时(WLED startTransition)。
 *
 * 与 WLED 的有意偏差(注释于实现处):
 *   - WLED _oldSegment 为段深拷贝(含像素,供非 FADE 的裁剪式混合
 *     风格使用);本组件过渡仅做 FADE 式参数插值,故仅保存轻量参数
 *     快照,无像素拷贝(裁剪式 blendingStyle 属任务 10/11 范畴);
 *   - WLED 调色板混合采用 nblendPaletteTowardPalette 渐进 255 遍,
 *     其数学极限即逐字节线性插值,此处直接线性插值,视觉等价;
 *   - 时间由调用方传入(wledc_time_ms_t),本模块不依赖 esp_timer,
 *     便于宿主测试注入时间轴。
 */
#ifndef WLED_C_TRANSITION_H
#define WLED_C_TRANSITION_H

#include <stdint.h>
#include <stdbool.h>

#include "wled_c_types.h"
#include "wled_c_palette.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 段类型前置声明(定义于 wled_c_segment.h;本结构内嵌其中)。 */
typedef struct wledc_segment_s wledc_segment_t;

/**
 * @brief 段过渡子结构(内嵌于 wledc_segment_t,对应 WLED Segment::_t)。
 *
 * 快照字段记录 start 时刻的段参数;progress 由 update 推进。
 */
typedef struct {
    bool active;             /**< 过渡进行中标志。 */
    uint16_t duration;       /**< 过渡时长(ms,> 0)。 */
    wledc_time_ms_t start;   /**< 过渡起始时刻(ms)。 */
    uint16_t progress;       /**< 进度 0-65535(0xFFFF = 完成)。 */
    uint32_t colors[WLEDC_NUM_COLORS]; /**< 起始主/次/三级颜色快照。 */
    uint8_t bri;             /**< 起始亮度(on ? opacity : 0)。 */
    uint8_t cct;             /**< 起始色温。 */
    uint8_t pal_entries[WLEDC_PALETTE_ENTRIES_BYTES]; /**< 起始调色板快照。 */
} wledc_transition_t;

#ifdef WLEDC_ENABLE_TRANSITIONS

/**
 * @brief 启动(或重启)段过渡,捕获当前参数为混合起点。
 *
 * 对齐 WLED startTransition:
 *   - duration 为 0 或段未开(SEG_OPT_ON)时,若正在过渡则立即结束;
 *   - 已在过渡中:快照压缩为当前视觉混合值(避免跳变)后重启计时;
 *   - 未在过渡:捕获 colors[3]/brightness/cct/调色板 entries 快照。
 *
 * @param seg 段指针(wledc_segment_get 返回值;NULL 安全)
 * @param duration_ms 过渡时长(ms;0 = 立即结束过渡)
 * @param now 当前毫秒时间轴
 */
void wledc_segment_start_transition(wledc_segment_t *seg,
                                    uint16_t duration_ms,
                                    wledc_time_ms_t now);

/**
 * @brief 立即结束段过渡(后续 current_* 直接返回当前段参数)。
 */
void wledc_segment_stop_transition(wledc_segment_t *seg);

/** 段是否处于过渡中(seg 为 NULL 时 false)。 */
bool wledc_segment_in_transition(const wledc_segment_t *seg);

/**
 * @brief 按时间轴推进过渡进度;达到 0xFFFF 时自动结束。
 *
 * 每帧由服务层(任务 11)调用一次;now 回绕安全(无符号差)。
 */
void wledc_segment_update_transition_progress(wledc_segment_t *seg,
                                              wledc_time_ms_t now);

/**
 * @brief 段第 idx 层的当前视觉颜色(过渡起点 ↔ 当前值按 progress 混合)。
 * @param idx 颜色层(0-2;越界返回当前层 0)
 * @return 未在过渡时直接返回段当前颜色
 */
uint32_t wledc_segment_current_color(const wledc_segment_t *seg, int idx);

/**
 * @brief 段当前视觉亮度(progress 加权:起点亮度 ↔ 当前亮度)。
 * @return 未在过渡时返回 on ? opacity : 0
 */
uint8_t wledc_segment_current_bri(const wledc_segment_t *seg);

/** 段当前视觉色温(混合语义同 current_bri)。 */
uint8_t wledc_segment_current_cct(const wledc_segment_t *seg);

/**
 * @brief 段当前视觉调色板(过渡起点 entries 与当前段调色板 entries
 *        逐字节线性插值)。
 *
 * @param out 输出 16x3 entries(WLEDC_PALETTE_ENTRIES_BYTES 字节)
 * @return true 成功装载;false 参数非法或调色板 id 无效(此时 out
 *         保持全 0,视觉为黑,与 WLED 降级语义一致)
 */
bool wledc_segment_current_palette(const wledc_segment_t *seg,
                                    uint8_t out[WLEDC_PALETTE_ENTRIES_BYTES]);

#endif /* WLEDC_ENABLE_TRANSITIONS */

#ifdef __cplusplus
}
#endif

#endif /* WLED_C_TRANSITION_H */