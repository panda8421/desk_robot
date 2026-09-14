/**
 * @file    svc_wifi.h
 * @brief   WiFi STA 连接管理服务
 * @note    依赖 esp_wifi；通过 esp_event 发布 WIFI_CONNECTED / DISCONNECTED / GOT_IP
 */
#ifndef SVC_WIFI_H
#define SVC_WIFI_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 API ---- */
esp_err_t svc_wifi_init(void);
esp_err_t svc_wifi_start(void);
esp_err_t svc_wifi_stop(void);
esp_err_t svc_wifi_deinit(void);
void      svc_wifi_register_console_cmds(void);

/* ---- 状态查询 ---- */
/**
 * @brief  查询当前是否已获取 IP（基于事件缓存）
 * @note   事件订阅者若晚于 GOT_IP 注册（启动竞态），可用本接口主动补齐状态
 */
bool svc_wifi_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* SVC_WIFI_H */
