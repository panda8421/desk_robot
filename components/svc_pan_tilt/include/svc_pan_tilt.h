/**
 * @file    svc_pan_tilt.h
 * @brief   Pan-Tilt 云台动作编排服务
 * @note    依赖 drv_servo_pwm；通过 esp_event 对外发布角度变化事件
 */
#ifndef SVC_PAN_TILT_H
#define SVC_PAN_TILT_H

#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 API ---- */

/**
 * @brief  初始化云台服务（创建资源，不启动任务）
 * @return ESP_OK / 错误码
 */
esp_err_t svc_pan_tilt_init(void);

/**
 * @brief  启动云台插值任务
 */
esp_err_t svc_pan_tilt_start(void);

/**
 * @brief  停止云台插值任务
 */
esp_err_t svc_pan_tilt_stop(void);

/**
 * @brief  反初始化云台服务
 */
esp_err_t svc_pan_tilt_deinit(void);

/**
 * @brief  注册云台相关控制台命令
 */
void svc_pan_tilt_register_console_cmds(void);

/* ---- 业务 API ---- */

/**
 * @brief  设置云台目标角度（异步平滑插值到目标）
 * @param  pan   水平目标角度（度）
 * @param  tilt  俯仰目标角度（度）
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t svc_pan_tilt_set_target(int16_t pan, int16_t tilt);

/**
 * @brief  获取云台当前角度
 * @param  angle  输出当前 pan/tilt 角度
 * @return ESP_OK / 错误码
 */
esp_err_t svc_pan_tilt_get_current(pan_tilt_angle_t *angle);

#ifdef __cplusplus
}
#endif

#endif /* SVC_PAN_TILT_H */
