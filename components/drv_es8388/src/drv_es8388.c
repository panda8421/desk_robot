/**
 * @file    drv_es8388.c
 * @brief   ES8388 音频 Codec 驱动实现
 * @note    初始化序列取自乐鑫 ESP-ADF 官方驱动（MIT），适配 16kHz/16bit/I2S 从机模式；
 *          板载 MIC 需 MICBIAS 偏置，ADCPOWER 用 0x00（开 MICBIAS）
 */
#include "drv_es8388.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_console.h"
#include "driver/i2c_master.h"
#include "board.h"
#include "console_cmd.h"

static const char *TAG = "drv_es8388";

#if CONFIG_DRV_ES8388_ENABLE

#define I2C_TIMEOUT_MS          100

/* ============== ES8388 寄存器地址（ESP-ADF 映射） ============== */
#define ES8388_CONTROL1         0x00    /* Enref/播放&录音模式 */
#define ES8388_CONTROL2         0x01    /* 模拟偏置/VREF */
#define ES8388_CHIPPOWER        0x02    /* 芯片电源状态机 */
#define ES8388_ADCPOWER         0x03    /* ADC/MICBIAS 电源 */
#define ES8388_DACPOWER         0x04    /* DAC/输出通路电源 */
#define ES8388_MASTERMODE       0x08    /* 主从模式 */
#define ES8388_ADCCONTROL1      0x09    /* MIC PGA 增益（左右半字节） */
#define ES8388_ADCCONTROL2      0x0A    /* 输入选择：LIN1/RIN1 */
#define ES8388_ADCCONTROL3      0x0B    /* 输入第二选择 */
#define ES8388_ADCCONTROL4      0x0C    /* ADC I2S 格式/位长 */
#define ES8388_ADCCONTROL5      0x0D    /* ADC 采样率分频（MCLK/LRCK=256） */
#define ES8388_ADCCONTROL8      0x10    /* ADC 左数字音量 */
#define ES8388_ADCCONTROL9      0x11    /* ADC 右数字音量 */
#define ES8388_DACCONTROL1      0x17    /* DAC I2S 格式/位长 */
#define ES8388_DACCONTROL2      0x18    /* DAC 采样率分频（MCLK/LRCK=256） */
#define ES8388_DACCONTROL3      0x19    /* DAC 静音 */
#define ES8388_DACCONTROL4      0x1A    /* DAC 左数字音量 */
#define ES8388_DACCONTROL5      0x1B    /* DAC 右数字音量 */
#define ES8388_DACCONTROL16     0x26    /* 混音器输入选择 */
#define ES8388_DACCONTROL17     0x27    /* 左 DAC→左混音器 */
#define ES8388_DACCONTROL20     0x2A    /* 右 DAC→右混音器 */
#define ES8388_DACCONTROL21     0x2B    /* ADC/DAC LRCK 共用选择 */
#define ES8388_DACCONTROL23     0x2D    /* VROI */
#define ES8388_DACCONTROL24     0x2E    /* LOUT1 音量（耳机） */
#define ES8388_DACCONTROL25     0x2F    /* ROUT1 音量（耳机） */
#define ES8388_DACCONTROL26     0x30    /* LOUT2 音量（喇叭） */
#define ES8388_DACCONTROL27     0x31    /* ROUT2 音量（喇叭） */

/* DACPOWER 输出通路位（esp-adf 定义） */
#define DAC_OUT_LOUT1           0x04    /* 耳机左 */
#define DAC_OUT_ROUT1           0x08    /* 耳机右 */
#define DAC_OUT_LOUT2           0x10    /* 喇叭左 */
#define DAC_OUT_ROUT2           0x20    /* 喇叭右 */
#define DAC_POWER_DOWN          0xC0    /* 关闭全部输出 */

/* ---- 私有状态 ---- */
typedef struct {
    bool                    initialized;
    bool                    running;
    i2c_master_dev_handle_t dev;
    uint8_t                 volume_pct;
    bool                    muted;
    bool                    speaker_mode;
} drv_es8388_ctx_t;

static drv_es8388_ctx_t s_ctx;

/* ============== 底层寄存器访问（复合事务持 I2C 总线锁） ============== */

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_ctx.dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t drv_es8388_read_reg(uint8_t reg, uint8_t *val)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(val != NULL, ESP_ERR_INVALID_ARG, TAG, "null ptr");
    board_i2c_lock();
    esp_err_t ret = i2c_master_transmit_receive(s_ctx.dev, &reg, 1, val, 1,
                                                I2C_TIMEOUT_MS);
    board_i2c_unlock();
    return ret;
}

/* ============== 初始化序列（源自 ESP-ADF es8388_init） ============== */

static esp_err_t chip_init(void)
{
    esp_err_t res = ESP_OK;

    /* DAC 先静音，避免初始化过程出现杂音 */
    res |= reg_write(ES8388_DACCONTROL3, 0x04);
    /* 芯片上电 */
    res |= reg_write(ES8388_CONTROL2, 0x50);
    res |= reg_write(ES8388_CHIPPOWER, 0x00);
    /* 关闭内部 DLL，改善 8kHz 采样率表现 */
    res |= reg_write(0x35, 0xA0);
    res |= reg_write(0x37, 0xD0);
    res |= reg_write(0x39, 0xD0);
    /* Codec 为 I2S 从机 */
    res |= reg_write(ES8388_MASTERMODE, 0x00);

    /* ---- DAC 通路 ---- */
    res |= reg_write(ES8388_DACPOWER, DAC_POWER_DOWN);      /* 先关输出 */
    res |= reg_write(ES8388_CONTROL1, 0x12);                /* 播放&录音模式 */
    res |= reg_write(ES8388_DACCONTROL1, 0x18);             /* I2S 格式，16bit */
    res |= reg_write(ES8388_DACCONTROL2, 0x02);             /* 单速，MCLK/LRCK=256 */
    res |= reg_write(ES8388_DACCONTROL16, 0x00);            /* 混音器输入 LIN1/RIN1 */
    res |= reg_write(ES8388_DACCONTROL17, 0x90);            /* 左 DAC→左混音 0dB */
    res |= reg_write(ES8388_DACCONTROL20, 0x90);            /* 右 DAC→右混音 0dB */
    res |= reg_write(ES8388_DACCONTROL21, 0x80);            /* ADC/DAC 共用 LRCK */
    res |= reg_write(ES8388_DACCONTROL23, 0x00);
    /* 模拟输出音量：0x1E=0dB（耳机 L1/R1），喇叭 L2/R2 由功放放大，保守 0dB */
    res |= reg_write(ES8388_DACCONTROL24, 0x1E);
    res |= reg_write(ES8388_DACCONTROL25, 0x1E);
    res |= reg_write(ES8388_DACCONTROL26, 0x1E);
    res |= reg_write(ES8388_DACCONTROL27, 0x1E);
    /* 打开 DAC 与全部输出通路 */
    res |= reg_write(ES8388_DACPOWER,
                     DAC_OUT_LOUT1 | DAC_OUT_ROUT1 | DAC_OUT_LOUT2 | DAC_OUT_ROUT2);

    /* ---- ADC 通路 ---- */
    res |= reg_write(ES8388_ADCPOWER, 0xFF);                /* 先关 ADC */
    res |= reg_write(ES8388_ADCCONTROL1, 0x88);             /* PGA 增益 24dB（板载咪头偏小） */
    res |= reg_write(ES8388_ADCCONTROL2, 0x00);             /* 输入选 LIN1/RIN1 */
    res |= reg_write(ES8388_ADCCONTROL3, 0x02);
    res |= reg_write(ES8388_ADCCONTROL4, 0x0C);             /* I2S 格式，16bit */
    res |= reg_write(ES8388_ADCCONTROL5, 0x02);             /* 单速，MCLK/LRCK=256 */
    res |= reg_write(ES8388_ADCCONTROL8, 0x00);             /* ADC 数字音量 0dB */
    res |= reg_write(ES8388_ADCCONTROL9, 0x00);
    /* 开 ADC、开 LIN/RIN、开 MICBIAS（板载咪头需要偏置） */
    res |= reg_write(ES8388_ADCPOWER, 0x00);

    return res;
}

/* ============== 生命周期 ============== */

esp_err_t drv_es8388_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(board_i2c_bus() != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "i2c bus not ready");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_ES8388_I2C_ADDR,
        .scl_speed_hz = BOARD_I2C0_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(board_i2c_bus(), &dev_cfg, &s_ctx.dev),
        TAG, "add i2c device failed");

    s_ctx.volume_pct = 80;
    s_ctx.muted = false;
    s_ctx.speaker_mode = true;

    esp_err_t ret = chip_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "es8388 chip init failed");

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done (addr=0x%02X)", BOARD_ES8388_I2C_ADDR);
    return ESP_OK;
}

esp_err_t drv_es8388_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 状态机重启一次，确保寄存器生效；然后解除静音 */
    ESP_RETURN_ON_ERROR(reg_write(ES8388_CHIPPOWER, 0xF0), TAG, "power seq failed");
    ESP_RETURN_ON_ERROR(reg_write(ES8388_CHIPPOWER, 0x00), TAG, "power seq failed");
    ESP_RETURN_ON_ERROR(drv_es8388_set_volume(s_ctx.volume_pct), TAG, "set volume failed");
    ESP_RETURN_ON_ERROR(drv_es8388_mute(s_ctx.muted), TAG, "mute failed");
    s_ctx.running = true;
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t drv_es8388_stop(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(drv_es8388_mute(true), TAG, "mute failed");
    ESP_RETURN_ON_ERROR(reg_write(ES8388_DACPOWER, DAC_POWER_DOWN), TAG,
                        "power down failed");
    s_ctx.running = false;
    return ESP_OK;
}

esp_err_t drv_es8388_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    reg_write(ES8388_CHIPPOWER, 0xFF);      /* 复位并停止 */
    i2c_master_bus_rm_device(s_ctx.dev);
    s_ctx.dev = NULL;
    s_ctx.initialized = false;
    s_ctx.running = false;
    return ESP_OK;
}

/* ============== 业务 API ============== */

esp_err_t drv_es8388_set_volume(uint8_t vol_pct)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (vol_pct > 100) {
        vol_pct = 100;
    }
    /* 数字音量寄存器换算（esp-adf）：reg = (-dB)<<1，0x00=0dB，0xC0=-96dB */
    uint8_t reg = (uint8_t)((100 - vol_pct) * 96 / 100 * 2);
    board_i2c_lock();
    esp_err_t ret = reg_write(ES8388_DACCONTROL4, reg);
    if (ret == ESP_OK) {
        ret = reg_write(ES8388_DACCONTROL5, reg);
    }
    board_i2c_unlock();
    ESP_RETURN_ON_ERROR(ret, TAG, "set volume failed");
    s_ctx.volume_pct = vol_pct;
    return ESP_OK;
}

esp_err_t drv_es8388_get_volume(uint8_t *vol_pct)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(vol_pct != NULL, ESP_ERR_INVALID_ARG, TAG, "null ptr");
    *vol_pct = s_ctx.volume_pct;
    return ESP_OK;
}

esp_err_t drv_es8388_set_mic_gain(uint8_t gain_db)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    /* 增益档位 0~8，每档 3dB，左右声道各占一个半字节 */
    uint8_t nib = gain_db / 3;
    if (nib > 8) {
        nib = 8;
    }
    uint8_t reg = (uint8_t)((nib << 4) | nib);
    ESP_RETURN_ON_ERROR(reg_write(ES8388_ADCCONTROL1, reg), TAG, "set gain failed");
    ESP_LOGI(TAG, "mic gain set %udB (reg 0x%02X)", nib * 3, reg);
    return ESP_OK;
}

esp_err_t drv_es8388_route_to_speaker(bool speaker)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    uint8_t power = speaker
                    ? (DAC_OUT_LOUT2 | DAC_OUT_ROUT2)               /* 喇叭 */
                    : (DAC_OUT_LOUT1 | DAC_OUT_ROUT1);              /* 耳机 */
    ESP_RETURN_ON_ERROR(reg_write(ES8388_DACPOWER, power), TAG, "route failed");
    s_ctx.speaker_mode = speaker;
    ESP_LOGI(TAG, "route -> %s", speaker ? "speaker" : "headphone");
    return ESP_OK;
}

esp_err_t drv_es8388_mute(bool mute)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_ERROR(reg_write(ES8388_DACCONTROL3, mute ? 0x04 : 0x00), TAG,
                        "mute failed");
    s_ctx.muted = mute;
    return ESP_OK;
}

/* ============== 控制台命令：es8388 dump | vol | gain | route ---- */
static int cmd_es8388(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: es8388 <dump|vol <0-100>|gain <0-24>|route <spk|hp>>\n");
        return 1;
    }
    if (strcmp(argv[1], "dump") == 0) {
        for (uint8_t r = 0; r < 0x35; r++) {
            uint8_t v = 0;
            if (drv_es8388_read_reg(r, &v) == ESP_OK) {
                printf("reg 0x%02X = 0x%02X\n", r, v);
            }
        }
    } else if (strcmp(argv[1], "vol") == 0 && argc >= 3) {
        uint8_t vol = (uint8_t)atoi(argv[2]);
        ESP_ERROR_CHECK(drv_es8388_set_volume(vol));
        printf("volume = %u\n", vol);
    } else if (strcmp(argv[1], "gain") == 0 && argc >= 3) {
        uint8_t gain = (uint8_t)atoi(argv[2]);
        ESP_ERROR_CHECK(drv_es8388_set_mic_gain(gain));
    } else if (strcmp(argv[1], "route") == 0 && argc >= 3) {
        bool spk = (strcmp(argv[2], "spk") == 0);
        ESP_ERROR_CHECK(drv_es8388_route_to_speaker(spk));
    } else {
        printf("unknown sub-command\n");
        return 1;
    }
    return 0;
}

void drv_es8388_register_console_cmds(void)
{
    const esp_console_cmd_t cmd = {
        .command = "es8388",
        .help = "ES8388 codec control: es8388 <dump|vol|gain|route>",
        .func = cmd_es8388,
    };
    if (console_cmd_add(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "register 'es8388' cmd failed");
    }
}

#else /* CONFIG_DRV_ES8388_ENABLE */

/* 驱动被裁剪时提供空实现，保证链接通过 */
esp_err_t drv_es8388_init(void)                     { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_es8388_start(void)                    { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_es8388_stop(void)                     { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_es8388_deinit(void)                   { return ESP_ERR_NOT_SUPPORTED; }
void      drv_es8388_register_console_cmds(void)    {}
esp_err_t drv_es8388_set_volume(uint8_t vol_pct)    { (void)vol_pct; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_es8388_get_volume(uint8_t *vol_pct)   { (void)vol_pct; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_es8388_set_mic_gain(uint8_t gain_db)  { (void)gain_db; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_es8388_route_to_speaker(bool speaker) { (void)speaker; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_es8388_mute(bool mute)                { (void)mute; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_es8388_read_reg(uint8_t reg, uint8_t *val)
{
    (void)reg; (void)val; return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_DRV_ES8388_ENABLE */
