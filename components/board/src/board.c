/**
 * @file    board.c
 * @brief   板级初始化：NVS、默认事件循环、I2C0 共享总线
 * @note    仅依赖 ESP-IDF，不依赖任何项目组件
 */
#include "board.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "board";

/* ============== I2C0 共享总线私有状态 ============== */
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static SemaphoreHandle_t s_i2c_mutex = NULL;

/* ============== I2C 总线恢复（GPIO 级） ============== */

/**
 * @brief  I2C 总线恢复：释放 SDA，翻转 SCL 最多 9 个时钟直到从机松开 SDA，
 *         最后补一个 STOP 条件
 * @note   必须在 i2c_new_master_bus 之前调用：IDF v5.2 i2c_master 驱动的
 *         ISR 里有 while(i2c_ll_is_bus_busy()) 无限自旋（i2c_master.c:520），
 *         若 SDA 被从机拉低，中断不返回会触发 Interrupt WDT panic
 * @return ESP_OK：总线已恢复空闲；ESP_ERR_INVALID_STATE：9 个时钟后 SDA 仍为低
 */
static esp_err_t i2c_bus_recovery(void)
{
    const gpio_num_t sda = BOARD_I2C0_SDA_GPIO;
    const gpio_num_t scl = BOARD_I2C0_SCL_GPIO;
    const int half_period_us = 5;   /* 100kHz 半周期 */

    /* SCL 配为开漏输出（可拉低），SDA 释放为输入上拉（只观察） */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << scl),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    io_conf.pin_bit_mask = (1ULL << sda);
    io_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&io_conf);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(half_period_us);

    /* 从机卡总线时 SDA 被拉低：逐个时钟脉冲释放，最多 9 个（一字节+ACK） */
    bool freed = (gpio_get_level(sda) == 1);
    for (int i = 0; !freed && i < 9; i++) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(half_period_us);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(half_period_us);
        freed = (gpio_get_level(sda) == 1);
    }
    if (!freed) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 产生 STOP：SCL 高电平期间 SDA 由低到高 */
    gpio_set_level(scl, 0);
    gpio_set_direction(sda, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(sda, 0);
    esp_rom_delay_us(half_period_us);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(half_period_us);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(half_period_us);
    return ESP_OK;
}

esp_err_t board_i2c_init(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 总线恢复：若上电时从机卡住 SDA（如 codec 复位时序异常），先解开，
     * 否则 i2c 驱动中断会自旋挂死（Interrupt WDT） */
    esp_err_t rec = i2c_bus_recovery();
    ESP_RETURN_ON_ERROR(rec, TAG,
                        "i2c bus stuck: SDA low after 9 clocks, check codec wiring/power");

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

void board_i2c_bus_check(esp_err_t err)
{
    static uint32_t fail_cnt = 0;
    static int64_t last_recover_us = 0;

    if (err == ESP_OK) {
        fail_cnt = 0;
        return;
    }
    if (err == ESP_ERR_INVALID_STATE || s_i2c_bus == NULL) {
        return;                         /* 总线未就绪等，与总线健康无关 */
    }
    fail_cnt++;
    int64_t now = esp_timer_get_time();
    if (fail_cnt < 3 || now - last_recover_us < 2000000) {
        return;                         /* 偶发丢事务 / 节流期内 */
    }
    last_recover_us = now;
    fail_cnt = 0;

    /* 双保险恢复（持锁序列化，确保无事务在途）：
     * 1) GPIO 级 9 时钟解卡 —— i2c_master_bus_reset 内部的解卡对
     *    "从机死锁拉低 SDA"无效（实测 reset 返回成功但事务仍失败）；
     * 2) i2c_master_bus_reset 把引脚从 GPIO 模式重挂回外设并复位 FSM */
    board_i2c_lock();
    esp_err_t rec = i2c_bus_recovery();
    esp_err_t rst = i2c_master_bus_reset(s_i2c_bus);
    board_i2c_unlock();

    if (rec != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus STUCK: SDA low after 9 clocks, "
                      "check slave power/wiring, power cycle recommended");
    } else if (rst != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus reset failed: %s", esp_err_to_name(rst));
    } else {
        ESP_LOGI(TAG, "i2c bus recovered");
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
