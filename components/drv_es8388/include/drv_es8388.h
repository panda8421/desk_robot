/**
 * @file    drv_es8388.h
 * @brief   ES8388 音频 Codec 驱动：通路/音量/增益/静音配置
 * @note    仅做 I2C 寄存器配置（挂 board 层 I2C0 总线），
 *          不做任何 I2S 数据操作，与 drv_audio 职责正交
 */
#ifndef DRV_ES8388_H
#define DRV_ES8388_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 API ---- */
esp_err_t drv_es8388_init(void);
esp_err_t drv_es8388_start(void);
esp_err_t drv_es8388_stop(void);
esp_err_t drv_es8388_deinit(void);
void      drv_es8388_register_console_cmds(void);

/* ---- 业务 API ---- */

/**
 * @brief  设置播放音量（DAC 数字音量）
 * @param  vol_pct 0~100，0=静音级，100=0dB
 */
esp_err_t drv_es8388_set_volume(uint8_t vol_pct);

/**
 * @brief  获取当前播放音量
 */
esp_err_t drv_es8388_get_volume(uint8_t *vol_pct);

/**
 * @brief  设置 MIC PGA 增益（左右声道同步）
 * @param  gain_db 0~24，步进 3dB，内部换算为寄存器档位
 */
esp_err_t drv_es8388_set_mic_gain(uint8_t gain_db);

/**
 * @brief  切换输出通路
 * @param  speaker true=喇叭(LOUT2/ROUT2, 经 MD8002A)；false=耳机(LOUT1/ROUT1)
 */
esp_err_t drv_es8388_route_to_speaker(bool speaker);

/**
 * @brief  DAC 静音控制（寄存器级，软静音）
 */
esp_err_t drv_es8388_mute(bool mute);

/**
 * @brief  读寄存器（调试用）
 */
esp_err_t drv_es8388_read_reg(uint8_t reg, uint8_t *val);

#ifdef __cplusplus
}
#endif

#endif /* DRV_ES8388_H */
