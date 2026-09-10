/**
 * @file    drv_audio.h
 * @brief   I2S 全双工音频数据通路驱动：PCM 帧读/写
 * @note    仅操作 I2S 外设，Codec 寄存器配置在 drv_es8388；
 *          音频规格统一 16kHz / 16bit / mono（见 VOICE_CHAT_DESIGN.md）
 */
#ifndef DRV_AUDIO_H
#define DRV_AUDIO_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 全局统一音频规格 */
#define DRV_AUDIO_SAMPLE_RATE       16000
#define DRV_AUDIO_BIT_WIDTH         16
#define DRV_AUDIO_CHANNELS          1

/* ---- 生命周期 API ---- */

/* 音频配置（当前规格固定，字段预留扩展） */
typedef struct {
    uint32_t sample_rate;       /* 采样率，统一 16000 */
    uint8_t  dma_desc_num;      /* DMA 描述符数量 */
    uint8_t  dma_frame_num;     /* 每描述符帧数（mono 帧数） */
} drv_audio_cfg_t;

esp_err_t drv_audio_init(const drv_audio_cfg_t *cfg);
esp_err_t drv_audio_start(void);
esp_err_t drv_audio_stop(void);
esp_err_t drv_audio_deinit(void);
void      drv_audio_register_console_cmds(void);

/* ---- 业务 API（阻塞式帧读写，调度由 svc_audio 负责） ---- */

/**
 * @brief  读取录音 PCM 帧（mono，int16，阻塞直到读满或超时）
 * @param  buf        输出缓冲（单位：采样点）
 * @param  samples    期望读取的采样点数
 * @param  timeout_ms 超时
 * @return ESP_OK / ESP_ERR_TIMEOUT / 错误码
 */
esp_err_t drv_audio_read(int16_t *buf, size_t samples, uint32_t timeout_ms);

/**
 * @brief  写播放 PCM 帧（mono，int16，阻塞直到写完或超时；mono 数据双声道复制）
 */
esp_err_t drv_audio_write(const int16_t *buf, size_t samples, uint32_t timeout_ms);

/**
 * @brief  设置录音软件数字增益（1.0 = 不放大）
 * @note   饱和处理，与 ES8388 PGA 模拟增益配合使用
 */
void drv_audio_set_input_gain(float gain);

#ifdef __cplusplus
}
#endif

#endif /* DRV_AUDIO_H */
