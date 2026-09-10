/**
 * @file    svc_ai_chat.h
 * @brief   云端对话服务（DashScope 客户端）：ASR → LLM → TTS 串行流水线（P1）
 * @note    所有请求为异步排队执行，完成后发布 CHAT_EVENT；
 *          API Key 存 NVS，经控制台命令写入
 */
#ifndef SVC_AI_CHAT_H
#define SVC_AI_CHAT_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 API ---- */
esp_err_t svc_ai_chat_init(void);
esp_err_t svc_ai_chat_start(void);
esp_err_t svc_ai_chat_stop(void);
esp_err_t svc_ai_chat_deinit(void);
void      svc_ai_chat_register_console_cmds(void);

/* ---- 业务 API（由对话状态机调用；非阻塞，结果经 CHAT_EVENT 上抛） ---- */

/**
 * @brief  语音识别请求（排队）
 * @param  pcm     16kHz/16bit/mono PCM（PSRAM，指向 svc_audio 录音缓冲）
 * @param  samples 采样点数；0 视为无有效语音，直接回错误事件
 * @note   完成后发布 CHAT_ASR_RESULT 或 CHAT_ERROR('a')
 */
esp_err_t svc_ai_chat_recognize(const int16_t *pcm, size_t samples);

/**
 * @brief  对话请求（排队）：LLM 生成回复 → TTS 合成音频
 * @note   完成后依次发布 CHAT_LLM_REPLY、CHAT_TTS_READY 或 CHAT_ERROR('l'/'t')
 */
esp_err_t svc_ai_chat_ask(const char *user_text);

/**
 * @brief  获取最近一次 TTS 合成的音频（16kHz/16bit/mono，内部 PSRAM 缓冲）
 * @note   在下一次 TTS 请求前有效
 */
esp_err_t svc_ai_chat_get_tts_data(const int16_t **pcm, size_t *samples);

/**
 * @brief  写入 DashScope API Key（存 NVS，掉电保持）
 */
esp_err_t svc_ai_chat_set_api_key(const char *key);

/**
 * @brief  清空多轮对话上下文
 */
esp_err_t svc_ai_chat_reset_context(void);

#ifdef __cplusplus
}
#endif

#endif /* SVC_AI_CHAT_H */
