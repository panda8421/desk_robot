/**
 * @file    app_types.h
 * @brief   跨组件共享的数据类型定义
 * @note    所有层均可依赖本头文件，本文件不得 include 任何项目内部头文件
 */
#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 舵机通道标识 ---- */
typedef enum {
    SERVO_CH_PAN = 0,   /* 水平方向舵机 */
    SERVO_CH_TILT,      /* 俯仰方向舵机 */
    SERVO_CH_MAX,
} servo_channel_t;

/* ---- 云台角度 ---- */
typedef struct {
    int16_t pan;        /* 水平角度，单位：度 */
    int16_t tilt;       /* 俯仰角度，单位：度 */
} pan_tilt_angle_t;

/* ---- 通用结果码（ESP-IDF 已提供 esp_err_t，此处仅补充业务语义枚举） ---- */

#ifdef __cplusplus
}
#endif

#endif /* APP_TYPES_H */
