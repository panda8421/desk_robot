/**
 * @file    console_cmd.h
 * @brief   串口控制台命令注册中心
 * @note    初始化 UART REPL；各组件通过 console_cmd_add() 注册命令
 */
#ifndef CONSOLE_CMD_H
#define CONSOLE_CMD_H

#include "esp_err.h"
#include "esp_console.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化控制台（UART REPL + 基础命令 help/restart/version/free）
 * @return ESP_OK / 错误码
 */
esp_err_t console_cmd_init(void);

/**
 * @brief  注册一条控制台命令
 * @param  cmd  命令配置（command、func、help、argtable）
 * @return ESP_OK / 错误码
 */
esp_err_t console_cmd_add(const esp_console_cmd_t *cmd);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_CMD_H */
