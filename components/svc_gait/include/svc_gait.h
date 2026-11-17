/**
 * @file    svc_gait.h
 * @brief   步态服务：向六足 ATmega328P 控制板发送 GoBLE 格式运动指令
 * @note    ATmega 固件（Vorpal 系）为黑盒，协议细节见 svc_gait.c 头部注释；
 *          1s 收不到有效包 ATmega 自动站定（原厂安全兜底）
 */
#ifndef SVC_GAIT_H
#define SVC_GAIT_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  运动指令（映射到 GoBLE 摇杆值）
 */
typedef enum {
    SVC_GAIT_MOTION_STOP = 0,   /**< 站定（摇杆回中） */
    SVC_GAIT_MOTION_FORWARD,    /**< 前进 */
    SVC_GAIT_MOTION_BACKWARD,   /**< 后退 */
    SVC_GAIT_MOTION_TURN_LEFT,  /**< 左转 */
    SVC_GAIT_MOTION_TURN_RIGHT, /**< 右转 */
} svc_gait_motion_t;

/* ---- 生命周期 API ---- */

/**
 * @brief  初始化步态服务（安装 UART 驱动，不启动发送任务）
 * @return ESP_OK / 错误码
 */
esp_err_t svc_gait_init(void);

/**
 * @brief  启动周期发送任务（按 CONFIG_SVC_GAIT_PERIOD_MS 周期发包）
 */
esp_err_t svc_gait_start(void);

/**
 * @brief  停止周期发送任务
 * @note   停止后 ATmega 在 1s 内自动站定，无需显式停止指令
 */
esp_err_t svc_gait_stop(void);

/**
 * @brief  反初始化步态服务
 */
esp_err_t svc_gait_deinit(void);

/**
 * @brief  注册步态相关控制台命令（gait fwd/stop/dance/...）
 */
void svc_gait_register_console_cmds(void);

/* ---- 业务 API ---- */

/**
 * @brief  设置当前运动指令（异步，下一发包周期生效）
 * @param  motion  目标运动
 * @return ESP_OK / ESP_ERR_INVALID_STATE（未初始化）
 */
esp_err_t svc_gait_set_motion(svc_gait_motion_t motion);

/**
 * @brief  获取当前运动指令
 * @param  motion  输出当前运动
 */
esp_err_t svc_gait_get_motion(svc_gait_motion_t *motion);

/**
 * @brief  切换步态子模式（对应 APP 上/下/左/右键，1~4）
 * @param  submode  1~4
 * @note   通过注入按键包实现，随后自动恢复摇杆包
 */
esp_err_t svc_gait_set_submode(uint8_t submode);

/**
 * @brief  进入走路模式（注入 APP SELECT 键）
 */
esp_err_t svc_gait_enter_walk(void);

/**
 * @brief  进入跳舞模式（注入 APP START 键）
 */
esp_err_t svc_gait_enter_dance(void);

#ifdef __cplusplus
}
#endif

#endif /* SVC_GAIT_H */
