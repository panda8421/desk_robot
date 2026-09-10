/**
 * @file    conversation.h
 * @brief   对话状态机（App 层业务编排）：监听事件、调度 Service、超时兜底
 * @note    仅调用 Service 层 API（svc_audio / svc_ai_chat / svc_status），
 *          不直接依赖 Driver 层
 */
#ifndef CONVERSATION_H
#define CONVERSATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 对话状态 ---- */
typedef enum {
    CHAT_STATE_BOOT = 0,        /* 上电初始化中 */
    CHAT_STATE_NET_WAIT,        /* 等待 WiFi 连接 */
    CHAT_STATE_STANDBY,         /* 待机（等 KEY0 触发） */
    CHAT_STATE_LISTENING,       /* 录音中 */
    CHAT_STATE_RECOGNIZING,     /* ASR 识别中 */
    CHAT_STATE_THINKING,        /* LLM+TTS 生成中 */
    CHAT_STATE_SPEAKING,        /* 播放回复中 */
    CHAT_STATE_ERROR,           /* 错误态 */
} chat_state_t;

/**
 * @brief  初始化：订阅事件并进入 NET_WAIT 态（在所有 service init 之后调用）
 */
void conversation_init(void);

/**
 * @brief  清理会话回到 STANDBY（需要 WiFi 已就绪）
 */
void conversation_reset(void);

chat_state_t conversation_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* CONVERSATION_H */
