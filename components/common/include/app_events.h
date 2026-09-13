/**
 * @file    app_events.h
 * @brief   自定义事件 base 与 event ID 集中定义
 * @note    所有跨组件通信必须经 esp_event，事件载荷所有权见各 ID 注释
 */
#ifndef APP_EVENTS_H
#define APP_EVENTS_H

#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============== 事件 base 声明 ============== */
ESP_EVENT_DECLARE_BASE(PAN_TILT_EVENT);
ESP_EVENT_DECLARE_BASE(APP_WIFI_EVENT);
ESP_EVENT_DECLARE_BASE(SERVO_EVENT);
ESP_EVENT_DECLARE_BASE(KEY_EVENT);
ESP_EVENT_DECLARE_BASE(AUDIO_EVENT);   /* svc_audio 发布 */
ESP_EVENT_DECLARE_BASE(CHAT_EVENT);    /* svc_ai_chat 发布 */
ESP_EVENT_DECLARE_BASE(GESTURE_EVENT); /* svc_ai_chat/控制台 → svc_behavior */

/* ============== PAN_TILT_EVENT ============== */
typedef enum {
    /* arg: pan_tilt_angle_t* ，值传递（esp_event_post 内部拷贝） */
    PAN_TILT_ANGLE_CHANGED,

    /* arg: servo_channel_t* ，值传递，表示触发限位的通道 */
    PAN_TILT_LIMIT_HIT,

    /* arg: NULL —— 控制台手动命令（pt set/home），行为层收到后抢占动画 */
    PAN_TILT_MANUAL_CMD,
} pan_tilt_event_id_t;

/* ============== WIFI_EVENT ============== */
typedef enum {
    /* arg: NULL */
    WIFI_CONNECTED,

    /* arg: NULL */
    WIFI_DISCONNECTED,

    /* arg: ip_event_got_ip_t* ，值传递 */
    WIFI_GOT_IP,
} wifi_event_id_t;

/* ============== SERVO_EVENT ============== */
typedef enum {
    /* arg: NULL，舵机驱动初始化完成 */
    SERVO_READY,
} servo_event_id_t;

/* ============== KEY_EVENT（drv_xl9555 发布，按键消抖后触发） ============== */
typedef enum {
    /* arg: uint8_t*（按键号 0~3，值传递） */
    KEY_PRESSED,

    /* arg: uint8_t*（按键号 0~3，值传递） */
    KEY_RELEASED,
} key_event_id_t;

/* ============== AUDIO_EVENT（svc_audio → 对话状态机） ============== */
typedef enum {
    /* arg: NULL —— 唤醒词命中（P2 预留，P1 不发布） */
    AUDIO_WAKE_WORD_DETECTED,

    /* arg: NULL —— VAD 检测到开始说话（录音中） */
    AUDIO_VAD_SPEECH_START,

    /* arg: NULL —— VAD 断句（一段话说完），录音数据可通过 API 获取 */
    AUDIO_VAD_SPEECH_END,

    /* arg: NULL —— 一段音频播放完成（含被打断后清空） */
    AUDIO_PLAYBACK_DONE,

    /* arg: int32_t*（错误码，值传递） —— 音频子系统错误 */
    AUDIO_ERROR,
} audio_event_id_t;

/* ============== CHAT_EVENT（svc_ai_chat → 对话状态机） ============== */
typedef enum {
    /* arg: chat_text_t*（识别文本，值传递） */
    CHAT_ASR_RESULT,

    /* arg: chat_text_t*（LLM 完整回复文本，值传递） */
    CHAT_LLM_REPLY,

    /* arg: NULL —— TTS 音频已就绪/开始可播（P1 中 TTS 完成后发布） */
    CHAT_TTS_READY,

    /* arg: chat_err_info_t*（值传递） —— 云端链路错误 */
    CHAT_ERROR,

    /* arg: int32_t*（chat_state_t 值拷贝） —— 对话状态机状态变化广播 */
    CHAT_STATE_CHANGED,

    /* arg: int32_t*（emotion_id_t 值拷贝） —— LLM/控制台情绪标记 → svc_face/svc_behavior */
    CHAT_EMOTION,
} chat_event_id_t;

/* ============== GESTURE_EVENT（动作指令 → svc_behavior） ============== */
typedef enum {
    /* arg: NULL，各动作均为即发即忘，新动作抢占旧动作 */
    GESTURE_NOD,        /* 点头 */
    GESTURE_SHAKE,      /* 摇头 */
    GESTURE_TILT_HEAD,  /* 歪头 */
    GESTURE_LISTEN,     /* 倾听位（侧头） */
    GESTURE_HOME,       /* 回正 */
    GESTURE_SAD,        /* 低头委屈 */
    GESTURE_SURPRISED,  /* 仰头惊讶 */
    GESTURE_SLEEPY,     /* 打瞌睡（缓慢垂头） */
} gesture_event_id_t;

#ifdef __cplusplus
}
#endif

#endif /* APP_EVENTS_H */
