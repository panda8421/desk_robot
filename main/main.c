/**
 * @file    main.c
 * @brief   应用层入口：仅做启动编排，不包含业务逻辑
 * @note    按 Board -> 基础设施 -> Service 顺序初始化
 */
#include <stdio.h>

#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"

#include "board.h"
#include "evt_log.h"
#include "diag.h"
#include "console_cmd.h"
#include "svc_pan_tilt.h"
#include "svc_wifi.h"
#include "svc_status.h"
#include "svc_audio.h"
#include "svc_ai_chat.h"
#include "svc_behavior.h"
#include "drv_xl9555.h"
#include "drv_es8388.h"
#include "drv_audio.h"
#include "conversation.h"

#if CONFIG_DRV_DISPLAY_ENABLE
#include "drv_display.h"
#endif
#if CONFIG_SVC_FACE_ENABLE
#include "svc_face.h"
#endif
#if CONFIG_DRV_CAMERA_ENABLE
#include "drv_camera.h"
#endif

static const char *TAG = "main";

/**
 * @brief  打印芯片与内存信息（启动时调用一次，便于确认硬件环境）
 */
static void print_chip_info(void)
{
    /* 获取芯片信息：型号、核心数、修订版本等 */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    /* CONFIG_IDF_TARGET 由构建系统注入，例如 "esp32s3" */
    ESP_LOGI(TAG, "chip: %s, cores=%d, rev=%d",
             CONFIG_IDF_TARGET, chip.cores, chip.revision);
    /* 打印当前可用堆内存，用于早期排查内存不足问题 */
    ESP_LOGI(TAG, "free heap: %u bytes", (unsigned)esp_get_free_heap_size());
}

/**
 * @brief  基础设施初始化：日志/诊断/控制台（一切输出的地基，最先就位）
 */
static void infra_init(void)
{
    ESP_ERROR_CHECK(evt_log_init());
    ESP_ERROR_CHECK(evt_log_start());

    ESP_ERROR_CHECK(diag_init());
    ESP_ERROR_CHECK(diag_start());

    ESP_ERROR_CHECK(console_cmd_init());
}

/**
 * @brief  驱动层启动：init + start 成对执行
 * @note   XL9555 是板级共享资源（功放/蜂鸣器/按键），须先于依赖它的服务启动
 */
static void drivers_init(void)
{
#if CONFIG_DRV_XL9555_ENABLE
    /* 板级 IO 扩展：按键扫描任务在此启动 */
    ESP_ERROR_CHECK(drv_xl9555_init());
    ESP_ERROR_CHECK(drv_xl9555_start());
#endif
#if CONFIG_DRV_DISPLAY_ENABLE
    drv_display_init(NULL);
    drv_display_start();
#endif
#if CONFIG_DRV_CAMERA_ENABLE
    drv_camera_init(NULL);
    drv_camera_start();
#endif
}

/**
 * @brief  服务层启动：init + start 成对执行，按依赖顺序
 * @note   WiFi 放在驱动之后：其连接为异步重试，晚启动无影响；
 *         svc_audio 依赖已就绪的 XL9555（功放使能）
 */
static void services_init(void)
{
    /* 状态灯最先启动：开始心跳闪烁，表示固件已进入业务初始化阶段 */
#if CONFIG_SVC_STATUS_ENABLE
    ESP_ERROR_CHECK(svc_status_init());
    ESP_ERROR_CHECK(svc_status_start());
#endif

    ESP_ERROR_CHECK(svc_pan_tilt_init());
    ESP_ERROR_CHECK(svc_pan_tilt_start());

#if CONFIG_SVC_BEHAVIOR_ENABLE
    /* 行为层：订阅对话状态/动作事件，驱动云台拟人动画 */
    ESP_ERROR_CHECK(svc_behavior_init());
    ESP_ERROR_CHECK(svc_behavior_start());
#endif

#if CONFIG_SVC_FACE_ENABLE
    /* 表情层：订阅对话状态事件，OLED 渲染呆萌脸 */
    ESP_ERROR_CHECK(svc_face_init());
    ESP_ERROR_CHECK(svc_face_start());
#endif

#if CONFIG_SVC_WIFI_ENABLE
    ESP_ERROR_CHECK(svc_wifi_init());
    ESP_ERROR_CHECK(svc_wifi_start());
#endif

#if CONFIG_SVC_AUDIO_ENABLE
    ESP_ERROR_CHECK(svc_audio_init());
    ESP_ERROR_CHECK(svc_audio_start());
#endif
#if CONFIG_SVC_AI_CHAT_ENABLE
    ESP_ERROR_CHECK(svc_ai_chat_init());
    ESP_ERROR_CHECK(svc_ai_chat_start());
#endif
}

/**
 * @brief  注册所有组件的控制台命令（横切汇总，与启动顺序无关）
 * @note   以后新增组件时只需在此函数追加一行
 */
static void console_cmds_register(void)
{
#if CONFIG_SVC_STATUS_ENABLE
    svc_status_register_console_cmds();
#endif
    svc_pan_tilt_register_console_cmds();
#if CONFIG_SVC_BEHAVIOR_ENABLE
    svc_behavior_register_console_cmds();
#endif
#if CONFIG_SVC_FACE_ENABLE
    svc_face_register_console_cmds();
#endif
    svc_wifi_register_console_cmds();
#if CONFIG_DRV_XL9555_ENABLE
    drv_xl9555_register_console_cmds();
#endif
#if CONFIG_DRV_DISPLAY_ENABLE
    drv_display_register_console_cmds();
#endif
#if CONFIG_DRV_CAMERA_ENABLE
    drv_camera_register_console_cmds();
#endif
#if CONFIG_SVC_AUDIO_ENABLE
    svc_audio_register_console_cmds();
    drv_es8388_register_console_cmds();
    drv_audio_register_console_cmds();
#endif
#if CONFIG_SVC_AI_CHAT_ENABLE
    svc_ai_chat_register_console_cmds();
#endif
    evt_log_register_console_cmds();
    diag_register_console_cmds();
}

/**
 * @brief  应用层启动编排
 * @note   严格按依赖顺序：板级 -> 基础设施 -> 驱动层 -> 服务层 -> 控制台命令 -> 对话状态机
 */
static void app_init(void)
{
    /* 1. Board/HAL 层：NVS + 默认事件循环 */
    ESP_ERROR_CHECK(board_init());

    /* 2. 基础设施 */
    infra_init();

    /* 3. 驱动层（init + start） */
    drivers_init();

    /* 4. 服务层（init + start，依赖驱动层就绪） */
    services_init();

    /* 5. 控制台命令注册 */
    console_cmds_register();

    /* 6. 启动对话状态机（订阅按键/音频/对话事件，驱动流程） */
    conversation_init();

    ESP_LOGI(TAG, "app init done");
}

void app_main(void)
{
    print_chip_info();
    app_init();
    /* 启动后控制权交给 console REPL 任务 + 各 service 任务 */
}
