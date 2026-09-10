/**
 * @file wled_c_rmt.c
 * @brief RMT WS2812 输出驱动实现(ESP-IDF v5 新版 RMT TX + Encoder)。
 *
 * 采用编码器组合模式(参考 ESP-IDF 官方 RMT 编码器架构):
 *   - bytes_encoder:数据字节流 → WS2812 位时序(bit0/bit1 常量见头文件)
 *   - copy_encoder :发送单个复位符号(≥300us 低电平)
 *   - 组合编码器跨 encode 回调维护两阶段状态(数据 → 复位)
 *
 * 帧防撕裂(design.md 3.2):show 前以超时 0 查询 rmt_tx_wait_all_done,
 * 上一帧未完全移出(含复位码)则跳过本帧。
 */
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "wled_c/wled_c_rmt.h"

static const char *TAG = "wled_c";

/* ===================== WS2812 组合编码器 ===================== */

/**
 * @brief WS2812 组合编码器:bytes_encoder 编码数据字节流,
 *        copy_encoder 发送复位码;state 跨 encode 回调延续。
 */
typedef struct {
  rmt_encoder_t     base;          /*!< 编码器基类(继承 rmt_encoder_t) */
  rmt_encoder_t    *bytes_encoder; /*!< 数据位编码器 */
  rmt_encoder_t    *copy_encoder;  /*!< 符号复制编码器 */
  int               state;         /*!< 0:数据阶段 1:复位阶段 */
  rmt_symbol_word_t reset_code;    /*!< 复位符号 */
} wledc_led_encoder_t;

static size_t wledc_led_encoder_encode(rmt_encoder_t *encoder,
                                       rmt_channel_handle_t channel,
                                       const void *primary_data, size_t data_size,
                                       rmt_encode_state_t *ret_state)
{
  wledc_led_encoder_t *enc = __containerof(encoder, wledc_led_encoder_t, base);
  rmt_encode_state_t session_state = RMT_ENCODING_RESET;
  rmt_encode_state_t state = RMT_ENCODING_RESET;
  size_t encoded_symbols = 0;

  switch (enc->state) {
  case 0: /* 阶段一:数据字节流 */
    encoded_symbols += enc->bytes_encoder->encode(enc->bytes_encoder, channel,
                                                  primary_data, data_size,
                                                  &session_state);
    if (session_state & RMT_ENCODING_COMPLETE) {
      enc->state = 1; /* 数据编码完成,转入复位阶段 */
    }
    if (session_state & RMT_ENCODING_MEM_FULL) {
      /* 符号空间不足,让出;下次回调从数据断点继续 */
      state |= RMT_ENCODING_MEM_FULL;
      goto out;
    }
    /* fall-through:数据编完后立即尝试复位码,减少交互次数 */
    /* fall through */
  case 1: /* 阶段二:复位码(≥300us 低电平) */
    encoded_symbols += enc->copy_encoder->encode(enc->copy_encoder, channel,
                                                 &enc->reset_code,
                                                 sizeof(enc->reset_code),
                                                 &session_state);
    if (session_state & RMT_ENCODING_COMPLETE) {
      enc->state = 0; /* 回到初始状态,服务下一帧 */
      state |= RMT_ENCODING_COMPLETE;
    }
    if (session_state & RMT_ENCODING_MEM_FULL) {
      state |= RMT_ENCODING_MEM_FULL;
      goto out;
    }
    break;
  default:
    break;
  }
out:
  *ret_state = state;
  return encoded_symbols;
}

static esp_err_t wledc_led_encoder_del(rmt_encoder_t *encoder)
{
  wledc_led_encoder_t *enc = __containerof(encoder, wledc_led_encoder_t, base);
  rmt_del_encoder(enc->bytes_encoder);
  rmt_del_encoder(enc->copy_encoder);
  free(enc);
  return ESP_OK;
}

static esp_err_t wledc_led_encoder_reset(rmt_encoder_t *encoder)
{
  wledc_led_encoder_t *enc = __containerof(encoder, wledc_led_encoder_t, base);
  rmt_encoder_reset(enc->bytes_encoder);
  rmt_encoder_reset(enc->copy_encoder);
  enc->state = 0;
  return ESP_OK;
}

/** 创建 WS2812 组合编码器(时序常量见 wled_c_rmt.h,80ns/tick)。 */
static esp_err_t wledc_led_encoder_new(rmt_encoder_handle_t *ret_encoder)
{
  esp_err_t err;
  wledc_led_encoder_t *enc = calloc(1, sizeof(wledc_led_encoder_t));
  if (!enc) {
    return ESP_ERR_NO_MEM;
  }

  enc->base.encode = wledc_led_encoder_encode;
  enc->base.del = wledc_led_encoder_del;
  enc->base.reset = wledc_led_encoder_reset;

  /* 数据位编码器:WS2812 位时序,每字节高位先行(msb_first) */
  rmt_bytes_encoder_config_t bytes_cfg = {
    .bit0 = { .level0 = 1, .duration0 = WLEDC_RMT_T0H_TICKS,
              .level1 = 0, .duration1 = WLEDC_RMT_T0L_TICKS },
    .bit1 = { .level0 = 1, .duration0 = WLEDC_RMT_T1H_TICKS,
              .level1 = 0, .duration1 = WLEDC_RMT_T1L_TICKS },
    .flags = { .msb_first = 1 },
  };
  err = rmt_new_bytes_encoder(&bytes_cfg, &enc->bytes_encoder);
  if (err != ESP_OK) {
    goto err_out;
  }

  /* 复制编码器:承载复位符号 */
  rmt_copy_encoder_config_t copy_cfg = { 0 };
  err = rmt_new_copy_encoder(&copy_cfg, &enc->copy_encoder);
  if (err != ESP_OK) {
    goto err_out;
  }

  /* 复位码:两半符号全低电平,合计 3750 tick = 300us */
  enc->reset_code = (rmt_symbol_word_t){
    .level0 = 0, .duration0 = WLEDC_RMT_RESET_TICKS / 2,
    .level1 = 0, .duration1 = WLEDC_RMT_RESET_TICKS / 2,
  };

  *ret_encoder = &enc->base;
  return ESP_OK;

err_out:
  if (enc->bytes_encoder) {
    rmt_del_encoder(enc->bytes_encoder);
  }
  free(enc);
  return err;
}

/* ===================== 公共接口 ===================== */

esp_err_t wledc_rmt_init(wledc_rmt_t *dev, const wledc_rmt_config_t *cfg)
{
  if (!dev || !cfg) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(dev, 0, sizeof(*dev));

  /* RMT TX 通道:12.5MHz 计数分辨率。mem_block_symbols 固定 64
   *(单个硬件符号块)—— 编码器为流式回调填充,RMT 逐块续传,
   *无需按像素数预分配;trans_queue_depth=4 允许排队传输。 */
  rmt_tx_channel_config_t chan_cfg = {
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .gpio_num = cfg->gpio,
    .mem_block_symbols = 64,
    .resolution_hz = WLEDC_RMT_RESOLUTION_HZ,
    .trans_queue_depth = 4,
    .flags = { .invert_out = false, .with_dma = false },
  };
  esp_err_t err = rmt_new_tx_channel(&chan_cfg, &dev->channel);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
    return err;
  }

  err = wledc_led_encoder_new(&dev->encoder);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "led encoder create failed: %s", esp_err_to_name(err));
    rmt_del_channel(dev->channel);
    dev->channel = NULL;
    return err;
  }

  err = rmt_enable(dev->channel);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
    rmt_del_encoder(dev->encoder);
    rmt_del_channel(dev->channel);
    dev->encoder = NULL;
    dev->channel = NULL;
    return err;
  }

  dev->config = *cfg;
  return ESP_OK;
}

esp_err_t wledc_rmt_show(wledc_rmt_t *dev, const uint8_t *pixels, size_t len)
{
  if (!dev || !dev->channel || !pixels || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  /* 防撕裂:超时 0 仅查询不等待 —— 上一帧(含复位码)尚未完全
   * 移出时跳过本帧,返回超时由上层决定降级策略。 */
  esp_err_t err = rmt_tx_wait_all_done(dev->channel, 0);
  if (err != ESP_OK) {
    return ESP_ERR_TIMEOUT;
  }

  /* 后台传输,不阻塞 CPU;编码器在 RMT 中断中流式填充符号 */
  return rmt_transmit(dev->channel, dev->encoder, pixels, len);
}

esp_err_t wledc_rmt_deinit(wledc_rmt_t *dev)
{
  if (!dev) {
    return ESP_ERR_INVALID_ARG;
  }

  if (dev->channel) {
    rmt_disable(dev->channel);
    rmt_del_channel(dev->channel);
    dev->channel = NULL;
  }
  if (dev->encoder) {
    rmt_del_encoder(dev->encoder);
    dev->encoder = NULL;
  }

  memset(dev, 0, sizeof(*dev));
  return ESP_OK;
}