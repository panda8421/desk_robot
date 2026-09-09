/**
 * @file    drv_led.h
 * @brief   状态指示灯 GPIO 驱动
 * @note    仅负责单个 LED 的亮/灭/翻转硬件操作，不含任何业务策略；
 *          LED 极性（是否低电平点亮）由 Kconfig 处理，上层只感知"亮/灭"
 */
#ifndef DRV_LED_H
#define DRV_LED_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 API ---- */

/**
 * @brief  初始化状态灯 GPIO（配置为输出并默认熄灭）
 * @return ESP_OK / 错误码
 */
esp_err_t drv_led_init(void);

/**
 * @brief  启动状态灯（无额外动作，保留以符合五段式生命周期）
 */
esp_err_t drv_led_start(void);

/**
 * @brief  停止状态灯（熄灭）
 */
esp_err_t drv_led_stop(void);

/**
 * @brief  反初始化，释放 GPIO
 */
esp_err_t drv_led_deinit(void);

/**
 * @brief  注册 LED 驱动的控制台命令（预留，业务命令在 svc_status 注册）
 */
void drv_led_register_console_cmds(void);

/* ---- 业务 API ---- */

/**
 * @brief  设置 LED 亮灭（内部自动处理 active-low 极性）
 * @param  on  true=亮  false=灭
 * @return ESP_OK / ESP_ERR_INVALID_STATE
 */
esp_err_t drv_led_set(bool on);

/**
 * @brief  读取 LED 当前逻辑状态
 * @param  on  输出当前状态（true=亮）
 * @return ESP_OK / 错误码
 */
esp_err_t drv_led_get(bool *on);

/**
 * @brief  翻转 LED，返回翻转后的状态
 * @return ESP_OK / 错误码
 */
esp_err_t drv_led_toggle(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_LED_H */
