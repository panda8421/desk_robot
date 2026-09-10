/**
 * @file    drv_xl9555.h
 * @brief   XL9555 IO 扩展芯片驱动：按键扫描、蜂鸣器、喇叭功放使能
 * @note    挂载在 I2C0 共享总线（board 层管理）；PCA9555 兼容寄存器映射
 */
#ifndef DRV_XL9555_H
#define DRV_XL9555_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 API ---- */
esp_err_t drv_xl9555_init(void);
esp_err_t drv_xl9555_start(void);
esp_err_t drv_xl9555_stop(void);
esp_err_t drv_xl9555_deinit(void);
void      drv_xl9555_register_console_cmds(void);

/* ---- 业务 API ---- */

/**
 * @brief  读取 KEY0~KEY3 按键状态（组合值，消抖后）
 * @param  key_mask 输出位图：bit0~bit3 对应 KEY0~KEY3，1=按下
 * @return ESP_OK / 错误码
 */
esp_err_t drv_xl9555_read_keys(uint8_t *key_mask);

/**
 * @brief  写 XL9555 输出引脚（P0/P1 口统一编址 0~15）
 * @param  pin   引脚号 0~15（board.h 中 BOARD_XL9555_PIN_xxx）
 * @param  level 电平：true=高
 */
esp_err_t drv_xl9555_write_pin(uint8_t pin, bool level);

/**
 * @brief  蜂鸣器控制（有源，高电平鸣叫）
 */
esp_err_t drv_xl9555_beep(bool on);

/**
 * @brief  喇叭功放使能（MD8002A 前级，高电平使能）
 * @note   播放前使能、长时间空闲时关闭以省电
 */
esp_err_t drv_xl9555_speaker_enable(bool on);

#ifdef __cplusplus
}
#endif

#endif /* DRV_XL9555_H */
