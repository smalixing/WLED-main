/**
 * @file wled_c_bri.h
 * @brief 亮度状态机与夜灯模块（对齐 wled00/led.cpp）
 *
 * 移植自 WLED led.cpp 的核心亮度语义：
 *  - bri / bri_old / bri_t / bri_last 四值状态机
 *  - toggle_on_off()：开关记忆（briLast）
 *  - set_brightness()：状态更新（过渡启动/压缩重启/夜灯重锚定/timebase 重置）
 *  - handle_transitions()：亮度线性过渡
 *  - handle_nightlight()：夜灯 FADE / COLORFADE / SET 三模式（SUN 模式不移植）
 *
 * 时间轴由调用方注入（now 参数），本模块不直接读取系统时钟。
 * 与 strip 模块协作：restart_runtime / set_transition_mode / reset_timebase /
 * strip_trigger 由 wled_c_strip.h 提供。
 */

#ifndef WLED_C_BRI_H
#define WLED_C_BRI_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "wled_c/wled_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 亮度状态 ---------------- */

/** @brief 当前目标亮度（用户设定值） */
uint8_t wledc_bri(void);

/** @brief 过渡起始亮度快照 */
uint8_t wledc_bri_old(void);

/** @brief 过渡中的瞬时亮度（实际施加值） */
uint8_t wledc_bri_t(void);

/** @brief 熄灯前记忆亮度（toggle 恢复用） */
uint8_t wledc_bri_last(void);

/** @brief 亮度是否已开启（bri > 0） */
bool wledc_bri_on(void);

/* ---------------- 核心操作 ---------------- */

/**
 * @brief 切换开/关（对齐 toggleOnOff()）。
 *
 * bri == 0 时恢复 bri_last 并重启效果运行时；
 * 否则记忆 bri_last 后将 bri 置 0。
 */
void wledc_toggle_on_off(void);

/**
 * @brief 设置目标亮度并启动过渡（stateUpdated() 精简版核心）。
 * @param brightness 新目标亮度（0-255）
 * @param now 当前时间戳（ms）
 *
 * 语义链：
 *  - bri != bri_old || state_changed → 启动过渡（进行中则 bri_old = bri_t 压缩重启）
 *  - bri_t == 0 时重置 strip timebase（效果从头开始）
 *  - bri > 0 时更新 bri_last
 *  - 夜灯 FADE/COLORFADE 激活时重锚定夜灯时间
 *  - transition == 0 时直接 apply_final_bri
 */
void wledc_set_brightness(uint8_t brightness, wledc_time_ms_t now);

/**
 * @brief 每帧调用：推进亮度过渡（对齐 handleTransitions()）。
 * @param now 当前时间戳（ms）
 */
void wledc_handle_transitions(wledc_time_ms_t now);

/**
 * @brief 每帧调用：推进夜灯进程（对齐 handleNightlight()，去除 SUN 模式）。
 * @param now 当前时间戳（ms）
 */
void wledc_handle_nightlight(wledc_time_ms_t now);

/* ---------------- 过渡参数 ---------------- */

/** @brief 设置全局过渡时长（ms）；0 表示无过渡 */
void wledc_set_transition(uint16_t duration_ms);

/** @brief 读取全局过渡时长（ms） */
uint16_t wledc_transition(void);

/* ---------------- 亮度倍率 ---------------- */

/**
 * @brief 设置亮度倍率（百分比，100 = 不缩放）。
 * @param percent 1-100
 * @return ESP_OK 或 ESP_ERR_INVALID_ARG
 */
esp_err_t wledc_set_bri_multiplier(uint8_t percent);

/** @brief 读取亮度倍率（百分比） */
uint8_t wledc_bri_multiplier(void);

/**
 * @brief 缩放亮度（对齐 scaledBri()）：in * multiplier / 100，封顶 255。
 */
uint8_t wledc_scaled_bri(uint8_t in);

/* ---------------- 夜灯配置 ---------------- */

/** 夜灯模式（SUN 模式不移植） */
typedef enum {
    WLEDC_NL_MODE_SET       = 0, /**< 结束时直接设为目标亮度 */
    WLEDC_NL_MODE_FADE      = 1, /**< 亮度渐变到目标 */
    WLEDC_NL_MODE_COLORFADE = 2, /**< 亮度与主色同时渐变到目标 */
} wledc_nl_mode_t;

/**
 * @brief 激活夜灯。
 * @param mode 模式（SET/FADE/COLORFADE）
 * @param delay_min 延迟分钟数（> 0）
 * @param target_bri 目标亮度（0-255）
 * @return ESP_OK 或 ESP_ERR_INVALID_ARG
 */
esp_err_t wledc_nightlight_begin(wledc_nl_mode_t mode, uint8_t delay_min,
                                 uint8_t target_bri);

/** @brief 夜灯是否激活 */
bool wledc_nightlight_active(void);

/* ---------------- 状态标志 ---------------- */

/** @brief 内部 state_changed 标志置位（供外部状态变更通知） */
void wledc_bri_mark_state_changed(void);

#ifdef __cplusplus
}
#endif

#endif /* WLED_C_BRI_H */