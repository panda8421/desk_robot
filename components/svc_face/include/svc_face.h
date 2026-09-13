/**
 * @file    svc_face.h
 * @brief   表情服务：在 OLED 上渲染呆萌机器人脸（眼睛/眨眼/嘴型）
 * @note    订阅对话状态事件自动切换表情；独立渲染任务驱动，不阻塞事件循环
 */
#ifndef SVC_FACE_H
#define SVC_FACE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t svc_face_init(void);
esp_err_t svc_face_start(void);
esp_err_t svc_face_stop(void);
void      svc_face_register_console_cmds(void);

#ifdef __cplusplus
}
#endif

#endif /* SVC_FACE_H */
