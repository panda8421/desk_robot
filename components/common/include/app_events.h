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

/* ============== PAN_TILT_EVENT ============== */
typedef enum {
    /* arg: pan_tilt_angle_t* ，值传递（esp_event_post 内部拷贝） */
    PAN_TILT_ANGLE_CHANGED,

    /* arg: servo_channel_t* ，值传递，表示触发限位的通道 */
    PAN_TILT_LIMIT_HIT,
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

#ifdef __cplusplus
}
#endif

#endif /* APP_EVENTS_H */
