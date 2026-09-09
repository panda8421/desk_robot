/**
 * @file    evt_log.h
 * @brief   事件黑匣子：关键事件环形持久化到 NVS
 * @note    仅记录白名单关键事件，跨重启保留
 */
#ifndef EVT_LOG_H
#define EVT_LOG_H

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 API ---- */
esp_err_t evt_log_init(void);
esp_err_t evt_log_start(void);
esp_err_t evt_log_stop(void);
esp_err_t evt_log_deinit(void);
void      evt_log_register_console_cmds(void);

/* ---- 业务 API ---- */

/**
 * @brief  记录一条关键事件到黑匣子
 * @param  base  事件 base
 * @param  id    事件 ID
 * @param  data  事件数据（可为 NULL）
 * @param  len   数据长度
 * @return ESP_OK / 错误码
 */
esp_err_t evt_log_record(esp_event_base_t base, int32_t id,
                         const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* EVT_LOG_H */
