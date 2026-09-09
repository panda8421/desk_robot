#include "board.h"

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"

static const char *TAG = "board";

esp_err_t board_init(void)
{
    /* 1. NVS 初始化 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. 默认事件循环（全局唯一，供所有组件使用） */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "board init done");
    return ESP_OK;
}
