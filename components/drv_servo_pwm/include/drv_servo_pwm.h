/**
 * @file    drv_servo_pwm.h
 * @brief   基于 MCPWM 的 PWM 舵机驱动
 * @note    仅硬件操作，不含业务逻辑；角度范围 0~180 度
 */
#ifndef DRV_SERVO_PWM_H
#define DRV_SERVO_PWM_H

#include <stdint.h>
#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 公共类型 ---- */
typedef struct {
    int16_t min_angle;      /* 最小角度限制（度） */
    int16_t max_angle;      /* 最大角度限制（度） */
} drv_servo_pwm_config_t;

/* ---- 生命周期 API ---- */

/**
 * @brief  初始化 MCPWM 外设，配置两路舵机 PWM 输出
 * @param  config  角度限制配置，传 NULL 使用默认值 (0~180)
 * @return ESP_OK / 错误码
 */
esp_err_t drv_servo_pwm_init(const drv_servo_pwm_config_t *config);

/**
 * @brief  启动舵机驱动（输出使能）
 */
esp_err_t drv_servo_pwm_start(void);

/**
 * @brief  停止舵机驱动（关闭输出，保留资源）
 */
esp_err_t drv_servo_pwm_stop(void);

/**
 * @brief  反初始化，释放 MCPWM 资源
 */
esp_err_t drv_servo_pwm_deinit(void);

/**
 * @brief  注册舵机驱动的控制台命令
 */
void drv_servo_pwm_register_console_cmds(void);

/* ---- 业务 API ---- */

/**
 * @brief  设置指定通道舵机角度
 * @param  channel  舵机通道（PAN / TILT）
 * @param  angle    目标角度（度）
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE
 */
esp_err_t drv_servo_pwm_set_angle(servo_channel_t channel, int16_t angle);

/**
 * @brief  获取指定通道当前角度
 * @param  channel  舵机通道
 * @param  angle    输出当前角度
 * @return ESP_OK / 错误码
 */
esp_err_t drv_servo_pwm_get_angle(servo_channel_t channel, int16_t *angle);

#ifdef __cplusplus
}
#endif

#endif /* DRV_SERVO_PWM_H */
