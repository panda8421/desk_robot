/**
 * @file    drv_display.h
 * @brief   OLED 显示驱动（SSD1306 128x64 I2C）
 * @note    驱动只做"显存 + 绘制 + 刷新"，表情内容由上层 svc_face 决定
 */
#ifndef DRV_DISPLAY_H
#define DRV_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width;
    uint16_t height;
} drv_display_config_t;

/* ---- 生命周期（main 启动编排调用） ---- */
esp_err_t drv_display_init(const drv_display_config_t *config); /* config=NULL 用板级默认 */
esp_err_t drv_display_start(void);      /* 开屏 */
esp_err_t drv_display_stop(void);       /* 关屏（显存保留） */
esp_err_t drv_display_deinit(void);

/* ---- 状态 ---- */
bool     drv_display_ready(void);
uint16_t drv_display_width(void);
uint16_t drv_display_height(void);

/* ---- 绘制（写入 MCU 侧显存，调 drv_display_flush 后才上屏） ---- */
void drv_display_clear(void);
void drv_display_set_px(int x, int y, bool on);
void drv_display_fill_rect(int x, int y, int w, int h, bool on);
void drv_display_rect_frame(int x, int y, int w, int h, bool on);

/* ---- 显存整帧上屏（I2C 一次全帧传输） ---- */
void drv_display_flush(void);

/* ---- 显示属性 ---- */
void drv_display_set_contrast(uint8_t contrast);    /* 0~255 亮度 */
void drv_display_invert(bool invert);               /* 反色显示 */

void drv_display_register_console_cmds(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_DISPLAY_H */
