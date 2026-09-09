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

#if CONFIG_DRV_DISPLAY_ENABLE
#include "drv_display.h"
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
 * @brief  应用层启动编排
 * @note   严格按依赖顺序初始化各层组件
 */
static void app_init(void)
{
    /* 1. Board/HAL 层：NVS + 默认事件循环 */
    ESP_ERROR_CHECK(board_init());

    /* 2. 基础设施 */
    ESP_ERROR_CHECK(evt_log_init());
    ESP_ERROR_CHECK(evt_log_start());

    ESP_ERROR_CHECK(diag_init());
    ESP_ERROR_CHECK(diag_start());

    ESP_ERROR_CHECK(console_cmd_init());

    /* 3. Service 层：init + start（按依赖顺序） */
    ESP_ERROR_CHECK(svc_pan_tilt_init());
    ESP_ERROR_CHECK(svc_pan_tilt_start());

#if CONFIG_SVC_WIFI_ENABLE
    ESP_ERROR_CHECK(svc_wifi_init());
    ESP_ERROR_CHECK(svc_wifi_start());
#endif

    /* 4. 可选外设 */
#if CONFIG_DRV_DISPLAY_ENABLE
    drv_display_init(NULL);
    drv_display_start();
#endif
#if CONFIG_DRV_CAMERA_ENABLE
    drv_camera_init(NULL);
    drv_camera_start();
#endif

    /* 5. 注册所有组件的控制台命令 */
    svc_pan_tilt_register_console_cmds();
    svc_wifi_register_console_cmds();
#if CONFIG_DRV_DISPLAY_ENABLE
    drv_display_register_console_cmds();
#endif
#if CONFIG_DRV_CAMERA_ENABLE
    drv_camera_register_console_cmds();
#endif
    evt_log_register_console_cmds();
    diag_register_console_cmds();

    ESP_LOGI(TAG, "app init done");
}

void app_main(void)
{
    print_chip_info();
    app_init();
    /* 启动后控制权交给 console REPL 任务 + 各 service 任务 */
}
