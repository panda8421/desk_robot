/**
 * @file    conversation.h
 * @brief   对话状态机（App 层业务编排）：监听事件、调度 Service、超时兜底
 * @note    仅调用 Service 层 API（svc_audio / svc_ai_chat / svc_status），
 *          不直接依赖 Driver 层
 */
#ifndef CONVERSATION_H
#define CONVERSATION_H

#include <stdint.h>
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* chat_state_t 已上移至 app_types.h（CHAT_STATE_CHANGED 事件载荷需要跨组件可见） */

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
