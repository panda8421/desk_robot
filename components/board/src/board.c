/**
 * @file    board.c
 * @brief   板级初始化：NVS、默认事件循环、I2C0 共享总线
 * @note    仅依赖 ESP-IDF，不依赖任何项目组件
 */
#include "board.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "board";

/* ============== I2C0 共享总线私有状态 ============== */
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static SemaphoreHandle_t s_i2c_mutex = NULL;

esp_err_t board_i2c_init(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 创建 I2C0 主机总线（新驱动 i2c_master API） */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C0_SDA_GPIO,
        .scl_io_num = BOARD_I2C0_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG,
                        "i2c_new_master_bus failed");

    /* 事务级互斥锁：保护"写寄存器地址+读数据"复合序列 */
    s_i2c_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_i2c_mutex != NULL, ESP_FAIL, TAG, "create i2c mutex failed");

    ESP_LOGI(TAG, "i2c0 bus ready (sda=%d, scl=%d, %dHz)",
             BOARD_I2C0_SDA_GPIO, BOARD_I2C0_SCL_GPIO, BOARD_I2C0_FREQ_HZ);
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c_bus;
}

void board_i2c_lock(void)
{
    if (s_i2c_mutex != NULL) {
        xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    }
}

void board_i2c_unlock(void)
{
    if (s_i2c_mutex != NULL) {
        xSemaphoreGive(s_i2c_mutex);
    }
}

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

    /* 3. I2C0 共享总线（音频 Codec / IO 扩展等挂载其上） */
    ESP_ERROR_CHECK(board_i2c_init());

    ESP_LOGI(TAG, "board init done");
    return ESP_OK;
}
