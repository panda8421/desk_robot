/**
 * @file    svc_status.h
 * @brief   设备状态指示服务：决定"灯以什么方式表达系统状态"
 * @note    只定义行为策略（常亮/常灭/闪烁），底层亮灭由 drv_led 完成；
 *          默认以心跳闪烁表示系统正常运行。将来可订阅 WiFi 等事件切换模式
 */
#ifndef SVC_STATUS_H
#define SVC_STATUS_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LED 工作模式 */
typedef enum {
    SVC_LED_OFF = 0,    /* 常灭 */
    SVC_LED_ON,         /* 常亮 */
    SVC_LED_BLINK,      /* 周期闪烁（心跳） */
} svc_status_led_mode_t;

/* ---- 生命周期 API ---- */

/**
 * @brief  初始化状态服务（内部初始化 drv_led，不启动闪烁）
 */
esp_err_t svc_status_init(void);

/**
 * @brief  启动状态服务（按默认心跳周期开始闪烁）
 */
esp_err_t svc_status_start(void);

/**
 * @brief  停止状态服务（熄灭 LED、停止定时器）
 */
esp_err_t svc_status_stop(void);

/**
 * @brief  反初始化状态服务
 */
esp_err_t svc_status_deinit(void);

/**
 * @brief  注册状态服务的控制台命令（led on/off/blink [周期ms]）
 */
void svc_status_register_console_cmds(void);

/* ---- 业务 API ---- */

/**
 * @brief  设置 LED 指示模式
 * @param  mode       SVC_LED_OFF / SVC_LED_ON / SVC_LED_BLINK
 * @param  period_ms  闪烁半周期（亮/灭各持续的时间），0 表示沿用当前周期
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t svc_status_set_led_mode(svc_status_led_mode_t mode, uint32_t period_ms);

#ifdef __cplusplus
}
#endif

#endif /* SVC_STATUS_H */
