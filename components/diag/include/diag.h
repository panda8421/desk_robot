/**
 * @file    diag.h
 * @brief   运行时诊断：内存/任务栈高水位/统计采集
 * @note    提供周期性快照与 console 手动触发
 */
#ifndef DIAG_H
#define DIAG_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 API ---- */
esp_err_t diag_init(void);
esp_err_t diag_start(void);
esp_err_t diag_stop(void);
esp_err_t diag_deinit(void);
void      diag_register_console_cmds(void);

/* ---- 业务 API ---- */

/**
 * @brief  立即打印一次内存/任务快照
 */
void diag_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif /* DIAG_H */
