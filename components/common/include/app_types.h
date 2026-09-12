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

/* ---- 对话文本消息载荷（esp_event 值传递） ---- */
#define CHAT_TEXT_MAX_LEN   256
typedef struct {
    char text[CHAT_TEXT_MAX_LEN];   /* UTF-8 文本（ASR 识别结果 / LLM 回复） */
} chat_text_t;

/* ---- 云端链路错误载荷（esp_event 值传递） ---- */
typedef struct {
    int32_t code;                   /* esp_err_t 或 HTTP 状态码（负值） */
    char    stage;                  /* 出错环节：'a'=ASR 'l'=LLM 't'=TTS */
} chat_err_info_t;

/* ---- 对话状态机状态（CHAT_STATE_CHANGED 事件载荷） ---- */
typedef enum {
    CHAT_STATE_BOOT = 0,        /* 上电初始化中 */
    CHAT_STATE_NET_WAIT,        /* 等待 WiFi 连接 */
    CHAT_STATE_STANDBY,         /* 待机（等 KEY0 触发） */
    CHAT_STATE_LISTENING,       /* 录音中 */
    CHAT_STATE_RECOGNIZING,     /* ASR 识别中 */
    CHAT_STATE_THINKING,        /* LLM+TTS 生成中 */
    CHAT_STATE_SPEAKING,        /* 播放回复中 */
    CHAT_STATE_ERROR,           /* 错误态 */
} chat_state_t;

#ifdef __cplusplus
}
#endif

#endif /* APP_TYPES_H */
