/**
 * @file    board.h
 * @brief   板级抽象：引脚映射、板级资源句柄、板级初始化
 * @note    仅依赖 ESP-IDF，不依赖任何项目组件
 */
#ifndef BOARD_H
#define BOARD_H

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============== 舵机引脚映射（Pan-Tilt 云台） ============== */
#define BOARD_SERVO_PAN_GPIO        GPIO_NUM_4   /* 水平舵机 PWM 输出 */
#define BOARD_SERVO_TILT_GPIO       GPIO_NUM_5   /* 俯仰舵机 PWM 输出 */

/* ============== 状态指示灯（红色 LED，接 IO1，低电平点亮） ============== */
#define BOARD_LED_STATUS_GPIO       GPIO_NUM_1

/* ============== 预留：显示屏引脚（后续填充） ============== */
/* #define BOARD_DISPLAY_SCLK_GPIO    GPIO_NUM_XX */
/* #define BOARD_DISPLAY_MOSI_GPIO    GPIO_NUM_XX */
/* #define BOARD_DISPLAY_CS_GPIO      GPIO_NUM_XX */
/* #define BOARD_DISPLAY_DC_GPIO      GPIO_NUM_XX */
/* #define BOARD_DISPLAY_RST_GPIO     GPIO_NUM_XX */

/* ============== 预留：摄像头引脚（后续填充） ============== */
/* DVP 摄像头引脚较多，确定型号后在此定义 */

/**
 * @brief  板级初始化：NVS、默认事件循环、基础时钟
 * @return ESP_OK / 错误码
 */
esp_err_t board_init(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_H */
