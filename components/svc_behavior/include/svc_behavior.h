/**
 * @file    svc_behavior.h
 * @brief   行为服务：把对话状态/LLM 动作标记翻译成云台拟人动画
 * @note    事件驱动，与 conversation 平级监听，不侵入状态机；
 *          动画为关键帧序列，新动作直接抢占旧动作
 */
#ifndef SVC_BEHAVIOR_H
#define SVC_BEHAVIOR_H

#include "esp_err.h"
#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 API ---- */

esp_err_t svc_behavior_init(void);
esp_err_t svc_behavior_start(void);
esp_err_t svc_behavior_stop(void);
esp_err_t svc_behavior_deinit(void);

/**
 * @brief  播放指定动作（立即抢占当前动画）
 * @param  g  动作 ID（GESTURE_NOD/SHAKE/TILT_HEAD/LISTEN/HOME）
 */
esp_err_t svc_behavior_play(gesture_event_id_t g);

/**
 * @brief  停止当前动画（云台保持当前位置，由 svc_pan_tilt 插值余量走完）
 */
void svc_behavior_abort(void);

/**
 * @brief  注册控制台命令（gesture <nod|shake|tilt|listen|home>）
 */
void svc_behavior_register_console_cmds(void);

#ifdef __cplusplus
}
#endif

#endif /* SVC_BEHAVIOR_H */
