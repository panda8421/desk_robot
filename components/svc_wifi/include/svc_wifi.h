/**
 * @file    svc_wifi.h
 * @brief   WiFi STA 连接管理服务
 * @note    依赖 esp_wifi；通过 esp_event 发布 WIFI_CONNECTED / DISCONNECTED / GOT_IP
 */
#ifndef SVC_WIFI_H
#define SVC_WIFI_H

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

#ifdef __cplusplus
}
#endif

#endif /* SVC_WIFI_H */
