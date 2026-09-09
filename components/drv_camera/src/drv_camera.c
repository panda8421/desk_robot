#include "drv_camera.h"

#include "esp_log.h"

static const char *TAG = "drv_camera";

esp_err_t drv_camera_init(const drv_camera_config_t *config)
{
    (void)config;
    /* TODO: 初始化 DVP 摄像头（esp_cam 驱动），需 PSRAM 做帧缓冲 */
    ESP_LOGI(TAG, "init stub");
    return ESP_OK;
}

esp_err_t drv_camera_start(void)
{
    /* TODO: 启动采集 */
    return ESP_OK;
}

esp_err_t drv_camera_stop(void)
{
    return ESP_OK;
}

esp_err_t drv_camera_deinit(void)
{
    return ESP_OK;
}

void drv_camera_register_console_cmds(void)
{
    /* TODO: 注册拍照/抓图 console 命令 */
}
