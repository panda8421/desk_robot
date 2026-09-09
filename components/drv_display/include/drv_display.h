/**
 * @file    drv_display.h
 * @brief   显示屏驱动（占位，确定 IC 型号后填充实现）
 * @note    Kconfig DRV_DISPLAY_ENABLE 默认关闭
 */
#ifndef DRV_DISPLAY_H
#define DRV_DISPLAY_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width;
    uint16_t height;
} drv_display_config_t;

esp_err_t drv_display_init(const drv_display_config_t *config);
esp_err_t drv_display_start(void);
esp_err_t drv_display_stop(void);
esp_err_t drv_display_deinit(void);
void      drv_display_register_console_cmds(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_DISPLAY_H */
