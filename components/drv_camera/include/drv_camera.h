/**
 * @file    drv_camera.h
 * @brief   DVP 摄像头驱动（占位，确定 sensor 型号后填充实现）
 * @note    Kconfig DRV_CAMERA_ENABLE 默认关闭，需 PSRAM
 */
#ifndef DRV_CAMERA_H
#define DRV_CAMERA_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  jpeg_quality;
} drv_camera_config_t;

esp_err_t drv_camera_init(const drv_camera_config_t *config);
esp_err_t drv_camera_start(void);
esp_err_t drv_camera_stop(void);
esp_err_t drv_camera_deinit(void);
void      drv_camera_register_console_cmds(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_CAMERA_H */
