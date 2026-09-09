#include "drv_display.h"

#include "esp_log.h"

static const char *TAG = "drv_display";

esp_err_t drv_display_init(const drv_display_config_t *config)
{
    (void)config;
    /* TODO: 根据 CONFIG_DRV_DISPLAY_TYPE 初始化对应 IC 驱动 */
    ESP_LOGI(TAG, "init stub");
    return ESP_OK;
}

esp_err_t drv_display_start(void)
{
    /* TODO: 开启显示刷新 */
    return ESP_OK;
}

esp_err_t drv_display_stop(void)
{
    return ESP_OK;
}

esp_err_t drv_display_deinit(void)
{
    return ESP_OK;
}

void drv_display_register_console_cmds(void)
{
    /* TODO: 注册显示相关 console 命令 */
}
