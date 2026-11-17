/**
 * @file    svc_gait.c
 * @brief   步态服务实现：模拟 GoBLE APP 通过 UART 驱动六足 ATmega328P
 *
 * ## 协议说明（逆向自 AR6 资料包 GoBLE.cpp / AR6-PS2control-code.ino）
 *
 * ATmega 固件原设计由手机 APP 经 CC2541 蓝牙模块透传指令；ESP32 直接连线
 * 顶替"APP + 蓝牙模块"，按相同格式发包即可，ATmega 固件无需改动。
 *
 * GoBLE 包格式（长度 10+N 字节，115200 8N1）：
 *   [0]=0x55  [1]=0xAA  [2]=0x11
 *   [3]=N 按下按钮数(0~5)
 *   [4]=摇杆状态占位(固定 0)
 *   [5..5+N-1]=按下按钮 ID（1上 2右 3下 4左 5SELECT 6START）
 *   [5+N..8+N]=摇杆数据（仅前 2 字节有效：X、Y，0~255，中值 127）
 *   [9+N]=校验和（前序所有字节之和的低 8 位）
 *
 * ATmega 侧指令映射（receiveDataHandler）：
 *   X>190 前进 / X<80 后退 / Y>190 右转 / Y<80 左转 / 其余站定
 *   按钮1~4 切换步态子模式 / 按钮5 进走路模式 / 按钮6 进跳舞模式
 *   开机默认 MODE_WALK，直接发摇杆包即可行走。
 *
 * 安全机制（原厂固件自带）：1s 无有效包自动站定，15s 重置连接。
 * 因此本组件以 20Hz 周期持续发包，空闲发"停止"包维持链路。
 */
#include "svc_gait.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_console.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "console_cmd.h"

static const char *TAG = "svc_gait";

/* ---- 协议常量 ---- */
#define GOBLE_HEADER1        0x55
#define GOBLE_HEADER2        0xAA
#define GOBLE_ADDRESS        0x11
#define GOBLE_JOY_PLACEHOLDER 0        /* [4] 占位字节 */
#define GOBLE_BASE_LEN       10        /* N=0 时的包长 */
#define GOBLE_BTN_SUBMODE_1  1         /* APP 上键 -> 子模式1 */
#define GOBLE_BTN_SUBMODE_2  3         /* APP 下键 -> 子模式2 */
#define GOBLE_BTN_SUBMODE_3  4         /* APP 左键 -> 子模式3 */
#define GOBLE_BTN_SUBMODE_4  2         /* APP 右键 -> 子模式4 */
#define GOBLE_BTN_SELECT     5         /* 进走路模式 */
#define GOBLE_BTN_START      6         /* 进跳舞模式 */
#define BTN_INJECT_CYCLES    3         /* 按键注入持续的发包周期数 */

/* 摇杆阈值（与 ATmega 侧 190/80 保持一致，取值走极端确保跨过阈值） */
#define JOY_MID   127
#define JOY_HIGH  255
#define JOY_LOW   0

/* ---- 私有状态 ---- */
typedef struct {
    bool            initialized;
    bool            running;
    bool            loopback_busy;         /* 回环测试期间暂停周期发送 */
    TaskHandle_t    task_handle;
    volatile uint8_t motion;               /* svc_gait_motion_t */
    volatile uint8_t btn_id;               /* 注入中的按钮 ID（0=无） */
    volatile uint8_t btn_cycles_left;      /* 剩余注入周期数 */
} svc_gait_ctx_t;

static svc_gait_ctx_t s_ctx;

/* ---- GoBLE 组包 ---- */

/**
 * @brief  构造一帧 GoBLE 包
 * @param  buf       输出缓冲（至少 GOBLE_BASE_LEN+5 字节）
 * @param  joy_x     摇杆 X (0~255)
 * @param  joy_y     摇杆 Y (0~255)
 * @param  btn_id    注入按钮 ID（0 表示无按键）
 * @return 包总长度
 */
static size_t build_packet(uint8_t *buf, uint8_t joy_x, uint8_t joy_y, uint8_t btn_id)
{
    size_t i = 0;
    buf[i++] = GOBLE_HEADER1;
    buf[i++] = GOBLE_HEADER2;
    buf[i++] = GOBLE_ADDRESS;
    buf[i++] = (btn_id > 0) ? 1 : 0;        /* N：同时最多注入一个按钮 */
    buf[i++] = GOBLE_JOY_PLACEHOLDER;
    if (btn_id > 0) {
        buf[i++] = btn_id;
    }
    buf[i++] = joy_x;
    buf[i++] = joy_y;
    buf[i++] = 0;                            /* 摇杆扩展字节，无效 */
    buf[i++] = 0;                            /* 摇杆扩展字节，无效 */
    /* 校验和：前序字节之和（8 位自然溢出） */
    uint8_t sum = 0;
    for (size_t k = 0; k < i; k++) {
        sum += buf[k];
    }
    buf[i++] = sum;
    return i;
}

/* 摇杆值 -> 运动映射 */
static void motion_to_joystick(uint8_t motion, uint8_t *x, uint8_t *y)
{
    switch (motion) {
    case SVC_GAIT_MOTION_FORWARD:    *x = JOY_HIGH; *y = JOY_MID;  break;
    case SVC_GAIT_MOTION_BACKWARD:   *x = JOY_LOW;  *y = JOY_MID;  break;
    case SVC_GAIT_MOTION_TURN_RIGHT: *x = JOY_MID;  *y = JOY_HIGH; break;
    case SVC_GAIT_MOTION_TURN_LEFT:  *x = JOY_MID;  *y = JOY_LOW;  break;
    case SVC_GAIT_MOTION_STOP:
    default:                         *x = JOY_MID;  *y = JOY_MID;  break;
    }
}

/* ---- 周期发送任务 ---- */
static void svc_gait_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(CONFIG_SVC_GAIT_PERIOD_MS);
    uint8_t pkt[GOBLE_BASE_LEN + 1];
    uint8_t rx_discard[64];

    while (s_ctx.running) {
        if (!s_ctx.loopback_busy) {
            uint8_t x, y;
            uint8_t btn = 0;

            /* 按键注入优先（切子模式/模式切换），持续若干周期后自动释放 */
            if (s_ctx.btn_cycles_left > 0) {
                btn = s_ctx.btn_id;
                s_ctx.btn_cycles_left--;
            }

            motion_to_joystick(s_ctx.motion, &x, &y);
            size_t len = build_packet(pkt, x, y, btn);
            uart_write_bytes(CONFIG_SVC_GAIT_UART_NUM, pkt, len);

            /* RX 端会收到 ATmega 的调试打印（旋钮读数等杂讯），读出即丢弃，
             * 防止 RX 缓冲积压； loopback 测试期间不丢弃 */
            if (!s_ctx.loopback_busy) {
                while (uart_read_bytes(CONFIG_SVC_GAIT_UART_NUM, rx_discard,
                                       sizeof(rx_discard), 0) > 0) {
                }
            }
        }
        vTaskDelay(period);
    }
    vTaskDelete(NULL);
}

/* ---- 生命周期 ---- */
esp_err_t svc_gait_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uart_config_t uart_cfg = {
        .baud_rate  = CONFIG_SVC_GAIT_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(CONFIG_SVC_GAIT_UART_NUM, &uart_cfg),
                        TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(CONFIG_SVC_GAIT_UART_NUM,
                                     CONFIG_SVC_GAIT_TX_GPIO,
                                     CONFIG_SVC_GAIT_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");
    /* TX 同步写、RX 留缓冲接 ATmega 调试杂讯 */
    ESP_RETURN_ON_ERROR(uart_driver_install(CONFIG_SVC_GAIT_UART_NUM, 256, 0, 0, NULL, 0),
                        TAG, "uart_driver_install failed");

    s_ctx.motion = SVC_GAIT_MOTION_STOP;
    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done, uart%d tx=IO%d rx=IO%d @%d",
             CONFIG_SVC_GAIT_UART_NUM, CONFIG_SVC_GAIT_TX_GPIO,
             CONFIG_SVC_GAIT_RX_GPIO, CONFIG_SVC_GAIT_BAUD_RATE);
    return ESP_OK;
}

esp_err_t svc_gait_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (s_ctx.running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.running = true;
    BaseType_t ret = xTaskCreate(svc_gait_task, "svc_gait",
                                 CONFIG_SVC_GAIT_TASK_STACK_SIZE, NULL,
                                 CONFIG_SVC_GAIT_TASK_PRIORITY,
                                 &s_ctx.task_handle);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t svc_gait_stop(void)
{
    s_ctx.running = false;
    /* 发送停止后 ATmega 1s 内自动站定（原厂安全兜底） */
    return ESP_OK;
}

esp_err_t svc_gait_deinit(void)
{
    s_ctx.initialized = false;
    s_ctx.running = false;
    uart_driver_delete(CONFIG_SVC_GAIT_UART_NUM);
    return ESP_OK;
}

/* ---- 业务 API ---- */
esp_err_t svc_gait_set_motion(svc_gait_motion_t motion)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(motion <= SVC_GAIT_MOTION_TURN_RIGHT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid motion %d", motion);
    s_ctx.motion = (uint8_t)motion;
    return ESP_OK;
}

esp_err_t svc_gait_get_motion(svc_gait_motion_t *motion)
{
    ESP_RETURN_ON_FALSE(motion != NULL, ESP_ERR_INVALID_ARG, TAG, "null ptr");
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    *motion = (svc_gait_motion_t)s_ctx.motion;
    return ESP_OK;
}

esp_err_t svc_gait_set_submode(uint8_t submode)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(submode >= 1 && submode <= 4,
                        ESP_ERR_INVALID_ARG, TAG, "submode %d out of 1~4", submode);
    static const uint8_t btn_map[5] = {
        0, GOBLE_BTN_SUBMODE_1, GOBLE_BTN_SUBMODE_2, GOBLE_BTN_SUBMODE_3, GOBLE_BTN_SUBMODE_4
    };
    s_ctx.btn_id = btn_map[submode];
    s_ctx.btn_cycles_left = BTN_INJECT_CYCLES;
    return ESP_OK;
}

esp_err_t svc_gait_enter_walk(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    s_ctx.btn_id = GOBLE_BTN_SELECT;
    s_ctx.btn_cycles_left = BTN_INJECT_CYCLES;
    s_ctx.motion = SVC_GAIT_MOTION_STOP;
    return ESP_OK;
}

esp_err_t svc_gait_enter_dance(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    s_ctx.btn_id = GOBLE_BTN_START;
    s_ctx.btn_cycles_left = BTN_INJECT_CYCLES;
    s_ctx.motion = SVC_GAIT_MOTION_STOP;
    return ESP_OK;
}

/* ---- 控制台命令 ---- */

static const char *motion_name(uint8_t motion)
{
    switch (motion) {
    case SVC_GAIT_MOTION_FORWARD:    return "forward";
    case SVC_GAIT_MOTION_BACKWARD:   return "backward";
    case SVC_GAIT_MOTION_TURN_LEFT:  return "turn_left";
    case SVC_GAIT_MOTION_TURN_RIGHT: return "turn_right";
    case SVC_GAIT_MOTION_STOP:       return "stop";
    default:                         return "?";
    }
}

/**
 * @brief  回环自测：TX(GPIO6) 用杜邦线短接 RX(GPIO7)，自发自收核对字节。
 *         无需六足在场，即可验证 UART 配置与组包格式正确。
 */
static int cmd_gait_loopback(void)
{
    if (!s_ctx.initialized) {
        printf("service not ready\n");
        return 1;
    }

    s_ctx.loopback_busy = true;
    vTaskDelay(pdMS_TO_TICKS(2 * CONFIG_SVC_GAIT_PERIOD_MS)); /* 等任务跳过一个周期 */
    uart_flush_input(CONFIG_SVC_GAIT_UART_NUM);

    uint8_t tx[GOBLE_BASE_LEN + 1];
    size_t tx_len = build_packet(tx, 42, 200, 0);   /* 特征值便于肉眼核对 */
    uart_write_bytes(CONFIG_SVC_GAIT_UART_NUM, tx, tx_len);

    uint8_t rx[GOBLE_BASE_LEN + 1] = {0};
    int rx_len = uart_read_bytes(CONFIG_SVC_GAIT_UART_NUM, rx, tx_len,
                                 pdMS_TO_TICKS(500));
    s_ctx.loopback_busy = false;

    printf("tx(%u):", (unsigned)tx_len);
    for (size_t i = 0; i < tx_len; i++) printf(" %02X", tx[i]);
    printf("\n");
    if (rx_len <= 0) {
        printf("rx: nothing (check jumper between IO%d and IO%d)\n",
               CONFIG_SVC_GAIT_TX_GPIO, CONFIG_SVC_GAIT_RX_GPIO);
        return 1;
    }
    printf("rx(%d):", rx_len);
    for (int i = 0; i < rx_len; i++) printf(" %02X", rx[i]);
    printf("\n");
    if (rx_len == (int)tx_len && memcmp(tx, rx, tx_len) == 0) {
        printf("LOOPBACK OK\n");
        return 0;
    }
    printf("LOOPBACK MISMATCH\n");
    return 1;
}

/* 用法：gait fwd|back|left|right|stop|walk|dance|status
 *       gait sub <1-4>    切步态子模式
 *       gait send <x> <y> 发原始摇杆值（0~255，调试协议用）
 *       gait loopback     TX-RX 短接自测（无需六足） */
static void print_usage(void)
{
    printf("usage:\n"
           "  gait fwd|back|left|right|stop\n"
           "  gait walk|dance          switch ATmega mode\n"
           "  gait sub <1-4>           set gait submode\n"
           "  gait send <x> <y>        raw joystick values (0-255)\n"
           "  gait status              show current motion\n"
           "  gait loopback            tx-rx jumper self test\n");
}

static int cmd_gait(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "fwd") == 0) {
        ESP_ERROR_CHECK(svc_gait_set_motion(SVC_GAIT_MOTION_FORWARD));
        printf("ok, forward\n");
        return 0;
    }
    if (strcmp(argv[1], "back") == 0) {
        ESP_ERROR_CHECK(svc_gait_set_motion(SVC_GAIT_MOTION_BACKWARD));
        printf("ok, backward\n");
        return 0;
    }
    if (strcmp(argv[1], "left") == 0) {
        ESP_ERROR_CHECK(svc_gait_set_motion(SVC_GAIT_MOTION_TURN_LEFT));
        printf("ok, turn left\n");
        return 0;
    }
    if (strcmp(argv[1], "right") == 0) {
        ESP_ERROR_CHECK(svc_gait_set_motion(SVC_GAIT_MOTION_TURN_RIGHT));
        printf("ok, turn right\n");
        return 0;
    }
    if (strcmp(argv[1], "stop") == 0) {
        ESP_ERROR_CHECK(svc_gait_set_motion(SVC_GAIT_MOTION_STOP));
        printf("ok, stop\n");
        return 0;
    }
    if (strcmp(argv[1], "walk") == 0) {
        ESP_ERROR_CHECK(svc_gait_enter_walk());
        printf("ok, enter walk mode\n");
        return 0;
    }
    if (strcmp(argv[1], "dance") == 0) {
        ESP_ERROR_CHECK(svc_gait_enter_dance());
        printf("ok, enter dance mode\n");
        return 0;
    }
    if (strcmp(argv[1], "sub") == 0) {
        if (argc != 3) {
            print_usage();
            return 1;
        }
        int sub = atoi(argv[2]);
        esp_err_t err = svc_gait_set_submode((uint8_t)sub);
        if (err != ESP_OK) {
            printf("sub failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("ok, submode %d\n", sub);
        return 0;
    }
    if (strcmp(argv[1], "send") == 0) {
        if (argc != 4) {
            print_usage();
            return 1;
        }
        int x = atoi(argv[2]);
        int y = atoi(argv[3]);
        if (x < 0 || x > 255 || y < 0 || y > 255) {
            printf("x/y must be 0-255\n");
            return 1;
        }
        uint8_t pkt[GOBLE_BASE_LEN + 1];
        size_t len = build_packet(pkt, (uint8_t)x, (uint8_t)y, 0);
        uart_write_bytes(CONFIG_SVC_GAIT_UART_NUM, pkt, len);
        printf("sent x=%d y=%d (%u bytes)\n", x, y, (unsigned)len);
        return 0;
    }
    if (strcmp(argv[1], "status") == 0) {
        svc_gait_motion_t m;
        if (svc_gait_get_motion(&m) != ESP_OK) {
            printf("service not ready\n");
            return 1;
        }
        printf("motion: %s%s\n", motion_name((uint8_t)m),
               s_ctx.btn_cycles_left > 0 ? " (btn injecting)" : "");
        return 0;
    }
    if (strcmp(argv[1], "loopback") == 0) {
        return cmd_gait_loopback();
    }

    print_usage();
    return 1;
}

void svc_gait_register_console_cmds(void)
{
    const esp_console_cmd_t cmd = {
        .command = "gait",
        .help = "Hexapod gait control: gait fwd|back|left|right|stop|walk|dance|sub|send|status|loopback",
        .func = cmd_gait,
    };
    if (console_cmd_add(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "register 'gait' cmd failed");
    }
}
