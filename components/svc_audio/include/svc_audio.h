/**
 * @file    svc_audio.h
 * @brief   音频前端服务：VAD 断句、录音/播放调度、提示音（P1 形态）
 * @note    P1 不含 esp-sr（唤醒词为 P2 预留）；依赖 drv_audio/drv_es8388/drv_xl9555
 */
#ifndef SVC_AUDIO_H
#define SVC_AUDIO_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 提示音 ID ---- */
typedef enum {
    SVC_TONE_WAKE = 0,      /* 唤醒/开始对话：双音上扬 */
    SVC_TONE_OK,            /* 确认（如开始录音）：短促单音 */
    SVC_TONE_END,           /* 结束（如录音结束）：低音 */
    SVC_TONE_ERROR,         /* 错误：低沉长音 */
} svc_tone_id_t;

/* ---- 生命周期 API ---- */
esp_err_t svc_audio_init(void);
esp_err_t svc_audio_start(void);        /* 使能功放、启动 I2S 与录音任务 */
esp_err_t svc_audio_stop(void);
esp_err_t svc_audio_deinit(void);
void      svc_audio_register_console_cmds(void);

/* ---- 业务 API（由对话状态机调用） ---- */

/**
 * @brief  开始一次录音会话（异步）
 * @note   内部录音任务持续采集，VAD 断句或 stop_recording 后结束，
 *         结束时发布 AUDIO_VAD_SPEECH_END 事件
 */
esp_err_t svc_audio_start_recording(void);

/**
 * @brief  手动结束当前录音会话（同样发布 AUDIO_VAD_SPEECH_END）
 */
esp_err_t svc_audio_stop_recording(void);

/**
 * @brief  获取最近一次会话的录音数据（VAD 修剪后，PSRAM 内部缓冲）
 * @param  pcm     输出缓冲指针（内部所有，调用方不得释放）
 * @param  samples 输出采样点数（mono），无有效语音时为 0
 * @note   数据在下一次 start_recording 前有效
 */
esp_err_t svc_audio_get_record_data(const int16_t **pcm, size_t *samples);

/**
 * @brief  播放 PCM（阻塞式，mono 16bit 16kHz，可在任意任务上下文调用）
 * @note   播放完成（或被打断）后发布 AUDIO_PLAYBACK_DONE
 */
esp_err_t svc_audio_play_pcm(const int16_t *pcm, size_t samples);

/**
 * @brief  停止当前播放（打断），清空 DMA 残留
 */
esp_err_t svc_audio_stop_play(void);

/**
 * @brief  播放内置提示音（阻塞式，时长 < 500ms）
 */
esp_err_t svc_audio_play_tone(svc_tone_id_t id);

/* ---- 调试接口 ---- */
void svc_audio_set_vad_threshold(uint32_t thr);     /* 设置 VAD 能量门限 */

#ifdef __cplusplus
}
#endif

#endif /* SVC_AUDIO_H */
