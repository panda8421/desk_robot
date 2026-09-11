/**
 * @file    drv_xl9555.c
 * @brief   XL9555 IO 扩展芯片驱动实现
 * @note    寄存器映射与 PCA9555 兼容；I2C0 总线由 board 层提供并互斥
 */
#include "drv_xl9555.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "board.h"
#include "app_events.h"
#include "console_cmd.h"

static const char *TAG = "drv_xl9555";

#if CONFIG_DRV_XL9555_ENABLE

/* ============== XL9555（PCA9555 兼容）寄存器地址 ============== */
#define REG_INPUT_PORT0         0x00    /* P0 口输入状态（只读） */
#define REG_INPUT_PORT1         0x01    /* P1 口输入状态（只读） */
#define REG_OUTPUT_PORT0        0x02    /* P0 口输出锁存 */
#define REG_OUTPUT_PORT1        0x03    /* P1 口输出锁存 */
#define REG_POLARITY_PORT0      0x04    /* P0 口极性反转 */
#define REG_POLARITY_PORT1      0x05    /* P1 口极性反转 */
#define REG_CONFIG_PORT0        0x06    /* P0 口方向：1=输入 0=输出 */
#define REG_CONFIG_PORT1        0x07    /* P1 口方向：1=输入 0=输出 */

#define I2C_TIMEOUT_MS          100
#define KEY_NUM                 4       /* KEY0~KEY3，对应 P04~P07 */
#define KEY_DEBOUNCE_TICKS      2       /* 连续 N 次采样一致才确认状态翻转 */

/* ---- 私有状态 ---- */
typedef struct {
    bool                     initialized;
    bool                     running;
    i2c_master_dev_handle_t  dev;
    uint8_t                  out_cache[2];      /* 输出锁存缓存：[0]=P0 [1]=P1 */
    uint8_t                  key_stable;        /* 消抖后的按键位图（bit0~3，1=按下） */
    uint8_t                  key_cnt[KEY_NUM];  /* 各按键电平变化计数器 */
    TaskHandle_t             scan_task;
} drv_xl9555_ctx_t;

static drv_xl9555_ctx_t s_ctx;

/* ============== 底层寄存器访问（复合事务，需持 I2C 总线锁） ============== */

/* 写寄存器：一次事务完成（reg+val 连发） */
static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_ctx.dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

/* 读寄存器：写地址 + 读数据两段事务，必须持总线锁保证原子性 */
static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    board_i2c_lock();
    esp_err_t ret = i2c_master_transmit_receive(s_ctx.dev, &reg, 1, val, 1,
                                                I2C_TIMEOUT_MS);
    board_i2c_unlock();
    return ret;
}

/* ============== 输出引脚操作 ============== */

esp_err_t drv_xl9555_write_pin(uint8_t pin, bool level)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(pin < 16, ESP_ERR_INVALID_ARG, TAG, "pin out of range");

    uint8_t port = pin / 8;             /* 0=P0 口 1=P1 口 */
    uint8_t bit  = pin % 8;

    board_i2c_lock();
    if (level) {
        s_ctx.out_cache[port] |= (1u << bit);
    } else {
        s_ctx.out_cache[port] &= ~(1u << bit);
    }
    esp_err_t ret = reg_write(port == 0 ? REG_OUTPUT_PORT0 : REG_OUTPUT_PORT1,
                              s_ctx.out_cache[port]);
    board_i2c_unlock();
    ESP_RETURN_ON_ERROR(ret, TAG, "write pin %u failed", pin);
    return ESP_OK;
}

esp_err_t drv_xl9555_beep(bool on)
{
    /* BEEP 低电平鸣叫（实测+正点原子官方例程确认），故取反输出 */
    return drv_xl9555_write_pin(BOARD_XL9555_PIN_BEEP, !on);
}

esp_err_t drv_xl9555_speaker_enable(bool on)
{
    /* SPK_EN 低电平使能功放（正点原子官方例程确认），故取反输出 */
    return drv_xl9555_write_pin(BOARD_XL9555_PIN_SPK_EN, !on);
}

/* ============== 按键扫描（独立任务，20ms 周期） ============== */

esp_err_t drv_xl9555_read_keys(uint8_t *key_mask)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(key_mask != NULL, ESP_ERR_INVALID_ARG, TAG, "null ptr");
    *key_mask = s_ctx.key_stable;
    return ESP_OK;
}

/* 发布按键事件（值传递，esp_event_post 内部拷贝） */
static void key_post(key_event_id_t id, uint8_t key)
{
    esp_event_post(KEY_EVENT, id, &key, sizeof(key), 0);
}

static void key_scan_task(void *arg)
{
    (void)arg;
    /* KEY0~3 在 P1 口（P17~P14），低电平有效；预计算各键在口内的位掩码 */
    static const uint8_t key_bit[KEY_NUM] = {
        (uint8_t)(1u << (BOARD_XL9555_PIN_KEY0 % 8)),
        (uint8_t)(1u << (BOARD_XL9555_PIN_KEY1 % 8)),
        (uint8_t)(1u << (BOARD_XL9555_PIN_KEY2 % 8)),
        (uint8_t)(1u << (BOARD_XL9555_PIN_KEY3 % 8)),
    };
    while (s_ctx.running) {
        uint8_t raw = 0;
        if (reg_read(REG_INPUT_PORT1, &raw) == ESP_OK) {
            /* 整理成 bit0~3 的"按下=1"位图（bit0=KEY0 ... bit3=KEY3） */
            uint8_t now = 0;
            for (uint8_t k = 0; k < KEY_NUM; k++) {
                if (!(raw & key_bit[k])) {
                    now |= (uint8_t)(1u << k);
                }
            }

            for (uint8_t k = 0; k < KEY_NUM; k++) {
                bool now_pressed = (now >> k) & 1u;
                bool was_pressed = (s_ctx.key_stable >> k) & 1u;

                if (now_pressed == was_pressed) {
                    s_ctx.key_cnt[k] = 0;       /* 与稳定态一致，计数清零 */
                    continue;
                }
                /* 电平与稳定态不同，累计确认次数 */
                if (++s_ctx.key_cnt[k] >= KEY_DEBOUNCE_TICKS) {
                    s_ctx.key_cnt[k] = 0;
                    if (now_pressed) {
                        s_ctx.key_stable |= (1u << k);
                        key_post(KEY_PRESSED, k);
                    } else {
                        s_ctx.key_stable &= ~(1u << k);
                        key_post(KEY_RELEASED, k);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DRV_XL9555_SCAN_PERIOD_MS));
    }
    s_ctx.scan_task = NULL;
    vTaskDelete(NULL);
}

/* ============== 生命周期 ============== */

esp_err_t drv_xl9555_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(board_i2c_bus() != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "i2c bus not ready");

    /* 挂载 I2C 设备 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_XL9555_I2C_ADDR,
        .scl_speed_hz = BOARD_I2C0_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(board_i2c_bus(), &dev_cfg, &s_ctx.dev),
        TAG, "add i2c device failed");

    /* 输出锁存初始化：蜂鸣器/功放低电平有效，默认输出高电平（关闭） */
    s_ctx.out_cache[0] = (uint8_t)((1u << BOARD_XL9555_PIN_SPK_EN) |
                                   (1u << BOARD_XL9555_PIN_BEEP));
    s_ctx.out_cache[1] = 0x00;
    board_i2c_lock();
    esp_err_t ret = reg_write(REG_OUTPUT_PORT0, s_ctx.out_cache[0]);
    if (ret == ESP_OK) {
        /* 方向配置：P02 功放、P03 蜂鸣器为输出，其余保持输入（1） */
        ret = reg_write(REG_CONFIG_PORT0,
                        (uint8_t)~((1u << BOARD_XL9555_PIN_SPK_EN) |
                                   (1u << BOARD_XL9555_PIN_BEEP)));
    }
    if (ret == ESP_OK) {
        ret = reg_write(REG_CONFIG_PORT1, 0xFF);    /* P1 口全部输入 */
    }
    board_i2c_unlock();
    ESP_RETURN_ON_ERROR(ret, TAG, "xl9555 config failed");

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done (addr=0x%02X)", BOARD_XL9555_I2C_ADDR);
    return ESP_OK;
}

esp_err_t drv_xl9555_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.running) {
        return ESP_OK;
    }
    s_ctx.running = true;
    BaseType_t ok = xTaskCreate(key_scan_task, "key_scan", 2048, NULL,
                                tskIDLE_PRIORITY + 2, &s_ctx.scan_task);
    ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_FAIL, TAG, "create key_scan task failed");
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t drv_xl9555_stop(void)
{
    if (s_ctx.running) {
        s_ctx.running = false;
        /* 等待扫描任务自行退出（最多一个扫描周期） */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DRV_XL9555_SCAN_PERIOD_MS + 20));
    }
    return ESP_OK;
}

esp_err_t drv_xl9555_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    drv_xl9555_stop();
    i2c_master_bus_rm_device(s_ctx.dev);
    s_ctx.dev = NULL;
    s_ctx.initialized = false;
    return ESP_OK;
}

/* ============== 控制台命令：key | beep ---- */
static int cmd_key(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint8_t mask = 0;
    if (drv_xl9555_read_keys(&mask) != ESP_OK) {
        printf("xl9555 not ready\n");
        return 1;
    }
    /* 原始输入口电平：1=高（未按下），按键按下时对应位变 0，便于核对映射 */
    uint8_t in0 = 0, in1 = 0;
    reg_read(REG_INPUT_PORT0, &in0);
    reg_read(REG_INPUT_PORT1, &in1);
    printf("KEY0~3 pressed mask: 0x%X\n", mask);
    printf("raw input: P0=0x%02X P1=0x%02X\n", in0, in1);
    return 0;
}

static int cmd_beep(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: beep <on|off>\n");
        return 1;
    }
    bool on = (strcmp(argv[1], "on") == 0);
    if (drv_xl9555_beep(on) != ESP_OK) {
        printf("xl9555 not ready\n");
        return 1;
    }
    printf("beep %s\n", on ? "on" : "off");
    return 0;
}

void drv_xl9555_register_console_cmds(void)
{
    const esp_console_cmd_t cmd_key_entry = {
        .command = "key",
        .help = "Read KEY0~3 state",
        .func = cmd_key,
    };
    const esp_console_cmd_t cmd_beep_entry = {
        .command = "beep",
        .help = "Control onboard beeper: beep <on|off>",
        .func = cmd_beep,
    };
    if (console_cmd_add(&cmd_key_entry) != ESP_OK ||
        console_cmd_add(&cmd_beep_entry) != ESP_OK) {
        ESP_LOGW(TAG, "register console cmds failed");
    }
}

#else /* CONFIG_DRV_XL9555_ENABLE */

/* 驱动被裁剪时提供空实现，保证链接通过 */
esp_err_t drv_xl9555_init(void)                     { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_xl9555_start(void)                    { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_xl9555_stop(void)                     { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_xl9555_deinit(void)                   { return ESP_ERR_NOT_SUPPORTED; }
void      drv_xl9555_register_console_cmds(void)    {}
esp_err_t drv_xl9555_read_keys(uint8_t *key_mask)
{
    (void)key_mask; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t drv_xl9555_write_pin(uint8_t pin, bool level)
{
    (void)pin; (void)level; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t drv_xl9555_beep(bool on)                  { (void)on; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_xl9555_speaker_enable(bool on)        { (void)on; return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_DRV_XL9555_ENABLE */
