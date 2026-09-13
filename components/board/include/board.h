/**
 * @file    board.h
 * @brief   板级抽象：引脚映射、板级资源句柄、板级初始化
 * @note    仅依赖 ESP-IDF，不依赖任何项目组件
 */
#ifndef BOARD_H
#define BOARD_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============== 舵机引脚映射（Pan-Tilt 云台） ============== */
#define BOARD_SERVO_PAN_GPIO        GPIO_NUM_4   /* 水平舵机 PWM 输出 */
#define BOARD_SERVO_TILT_GPIO       GPIO_NUM_5   /* 俯仰舵机 PWM 输出 */

/* ============== 状态指示灯（红色 LED，接 IO1，低电平点亮） ============== */
#define BOARD_LED_STATUS_GPIO       GPIO_NUM_1

/* ============== I2C0 共享总线（ES8388 / XL9555 / AP3216C / 24C02） ============== */
#define BOARD_I2C0_SDA_GPIO         GPIO_NUM_41
#define BOARD_I2C0_SCL_GPIO         GPIO_NUM_42
#define BOARD_I2C0_FREQ_HZ          100000       /* 共享总线保守速率 */

/* ============== XL9555 IO 扩展芯片（P0/P1 口分配，见硬件手册） ============== */
#define BOARD_XL9555_I2C_ADDR       0x20         /* 7 位从机地址 A2A1A0=000 */
#define BOARD_XL9555_PIN_SPK_EN     2            /* P02: 喇叭功放使能，低电平有效（官方例程映射） */
#define BOARD_XL9555_PIN_USB_SEL    1            /* P01: QMA6100P 中断（本工程不使用） */
#define BOARD_XL9555_PIN_BEEP       3            /* P03: 有源蜂鸣器，低电平鸣叫（实测+官方例程确认） */
#define BOARD_XL9555_PIN_KEY0       15           /* P17: KEY0，低电平有效（官方例程映射，实测确认） */
#define BOARD_XL9555_PIN_KEY1       14           /* P16: KEY1，低电平有效 */
#define BOARD_XL9555_PIN_KEY2       13           /* P15: KEY2，低电平有效 */
#define BOARD_XL9555_PIN_KEY3       12           /* P14: KEY3，低电平有效 */

/* ============== I2S 音频（ES8388 Codec，全双工） ============== */
#define BOARD_I2S_MCLK_GPIO         GPIO_NUM_3   /* 主时钟 */
#define BOARD_I2S_SCK_GPIO          GPIO_NUM_46  /* 位时钟 */
#define BOARD_I2S_LRCK_GPIO         GPIO_NUM_9   /* 帧时钟 */
#define BOARD_I2S_SDIN_GPIO         GPIO_NUM_14  /* 录音数据（ES8388 ASDOUT -> S3） */
#define BOARD_I2S_SDOUT_GPIO        GPIO_NUM_10  /* 播放数据（S3 DSDIN -> ES8388 DAC） */
#define BOARD_ES8388_I2C_ADDR       0x10         /* ES8388 7 位从机地址 */

/* ============== OLED 显示屏（0.96" SSD1306 128x64，独立 I2C，P1 排针引出） ============== */
#define BOARD_OLED_SDA_GPIO         GPIO_NUM_47  /* P1 排针 pin14 */
#define BOARD_OLED_SCL_GPIO         GPIO_NUM_48  /* P1 排针 pin13 */
#define BOARD_OLED_I2C_FREQ_HZ      400000       /* 独立总线，可全速 */
#define BOARD_OLED_I2C_ADDR         0x3C         /* 7 位地址（SA0=0，个别模组为 0x3D） */
#define BOARD_OLED_WIDTH            128
#define BOARD_OLED_HEIGHT           64

/* ============== 预留：摄像头引脚（后续填充） ============== */
/* DVP 摄像头引脚较多，确定型号后在此定义 */

/**
 * @brief  板级初始化：NVS、默认事件循环、基础时钟
 * @return ESP_OK / 错误码
 */
esp_err_t board_init(void);

/* ============== I2C0 共享总线管理（多设备共用，事务级互斥） ============== */
/**
 * @brief  初始化 I2C0 总线与互斥锁（board_init 内部自动调用）
 * @return ESP_OK / 错误码
 */
esp_err_t board_i2c_init(void);

/**
 * @brief  获取 I2C0 总线句柄（各驱动用它挂载自己的设备）
 * @return 总线句柄，未初始化返回 NULL
 */
i2c_master_bus_handle_t board_i2c_bus(void);

/**
 * @brief  加锁/解锁：保护"写寄存器地址+读数据"这类多事务原子序列
 * @note   单次 i2c_master 事务驱动内部已串行化，复合序列必须持锁
 */
void board_i2c_lock(void);
void board_i2c_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_H */
