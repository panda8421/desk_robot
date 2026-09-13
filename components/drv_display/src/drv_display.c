/**
 * @file    drv_display.c
 * @brief   SSD1306 OLED 驱动实现（128x64，独立 I2C 总线）
 * @note    显存格式与 SSD1306 一致：纵向 8 点 1 字节（page），horizontal 寻址模式
 *          整帧连续写；I2C 控制字节 0x00=命令流 / 0x40=数据流
 *          独占一路硬件 I2C（不与板载器件共享），无并发访问者，无需加锁
 */
#include "drv_display.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "board.h"
#include "console_cmd.h"

static const char *TAG = "drv_display";

#if CONFIG_DRV_DISPLAY_ENABLE

#define OLED_CMD_STREAM     0x00    /* I2C 控制字节：后续为命令 */
#define OLED_DATA_STREAM    0x40    /* I2C 控制字节：后续为显存数据 */

#define VRAM_BYTES(w, h)    ((w) * (h) / 8)

/* ---- 私有状态 ---- */
typedef struct {
    bool                    initialized;
    bool                    running;        /* 屏幕电源状态（关屏显存仍保留） */
    uint16_t                width;
    uint16_t                height;
    uint8_t                 vram[VRAM_BYTES(BOARD_OLED_WIDTH, BOARD_OLED_HEIGHT)];
    i2c_master_bus_handle_t bus;            /* OLED 独立总线 */
    i2c_master_dev_handle_t dev;
} drv_display_ctx_t;

static drv_display_ctx_t s_ctx;

/* ============== 底层访问 ============== */

/* 命令流：一次可带多条单字节命令（初始化序列 23 字节，缓冲取 32） */
static esp_err_t cmds_write(const uint8_t *cmds, size_t n)
{
    uint8_t buf[32];
    if (n > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = OLED_CMD_STREAM;
    memcpy(&buf[1], cmds, n);
    return i2c_master_transmit(s_ctx.dev, buf, n + 1, 100);
}

/* ============== SSD1306 初始化序列（数据手册 128x64 标准配置） ============== */

static esp_err_t chip_init_seq(void)
{
    static const uint8_t init_cmds[] = {
        0xAE,   /* display off */
        0xD5, 0x80, /* 时钟：默认分频/振荡 */
        0xA8, 0x3F, /* 复用率 63（64 行） */
        0xD3, 0x00, /* 显示偏移 0 */
        0x40,       /* 起始行 0 */
        0x8D, 0x14, /* 内部电荷泵使能（模组无外部 VCC） */
        0x20, 0x00, /* 寻址模式：horizontal（整帧连续写） */
        0xA1,       /* 段重映射：列 127 -> SEG0（正常朝向） */
        0xC8,       /* COM 扫描方向：逆向（与 A1 配合成正常视角） */
        0xDA, 0x12, /* COM 引脚：交替排列（128x64 用） */
        0x81, 0x7F, /* 对比度 */
        0xD9, 0xF1, /* 预充电 */
        0xDB, 0x40, /* VCOMH 电压 */
        0xA4,       /* 恢复 RAM 内容显示 */
        0xA6,       /* 正常（非反色） */
    };
    return cmds_write(init_cmds, sizeof(init_cmds));
}

/* ============== 生命周期 ============== */

esp_err_t drv_display_init(const drv_display_config_t *config)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.width  = (config && config->width)  ? config->width  : BOARD_OLED_WIDTH;
    s_ctx.height = (config && config->height) ? config->height : BOARD_OLED_HEIGHT;
    ESP_RETURN_ON_FALSE(s_ctx.width == BOARD_OLED_WIDTH && s_ctx.height == BOARD_OLED_HEIGHT,
                        ESP_ERR_NOT_SUPPORTED, TAG, "only 128x64 supported");

    /* OLED 独占一路 I2C 控制器，与板载共享总线互不干扰 */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = BOARD_OLED_SDA_GPIO,
        .scl_io_num = BOARD_OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* 模组自带上拉，内部上拉兜底 */
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_ctx.bus), TAG, "new i2c bus failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_OLED_I2C_ADDR,
        .scl_speed_hz = BOARD_OLED_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_ctx.bus, &dev_cfg, &s_ctx.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add i2c device failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* 探测器件应答：地址错了立刻发现 */
    err = i2c_master_probe(s_ctx.bus, BOARD_OLED_I2C_ADDR, 100);
    if (err != ESP_OK) {
        /* 自动扫描 7 位地址空间，打印所有应答器件，便于定位接线/地址问题 */
        ESP_LOGE(TAG, "no device at 0x%02X, scanning bus...", BOARD_OLED_I2C_ADDR);
        int found = 0;
        for (uint8_t a = 0x03; a < 0x78; a++) {
            if (i2c_master_probe(s_ctx.bus, a, 20) == ESP_OK) {
                ESP_LOGW(TAG, "  -> device found at 0x%02X%s", a,
                         (a == 0x3D) ? "  <<< OLED is here, change BOARD_OLED_I2C_ADDR" : "");
                found++;
            }
        }
        if (found == 0) {
            ESP_LOGE(TAG, "  -> nothing answered. Check VCC/GND/SDA(%d)/SCL(%d) wiring",
                     BOARD_OLED_SDA_GPIO, BOARD_OLED_SCL_GPIO);
        }
        goto fail;
    }

    err = chip_init_seq();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init sequence failed: %s", esp_err_to_name(err));
        goto fail;
    }

    memset(s_ctx.vram, 0, sizeof(s_ctx.vram));
    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done (SSD1306 %ux%u @0x%02X, sda=%d scl=%d)",
             s_ctx.width, s_ctx.height, BOARD_OLED_I2C_ADDR,
             BOARD_OLED_SDA_GPIO, BOARD_OLED_SCL_GPIO);
    return ESP_OK;

fail:
    if (s_ctx.dev) {
        i2c_master_bus_rm_device(s_ctx.dev);
        s_ctx.dev = NULL;
    }
    i2c_del_master_bus(s_ctx.bus);
    s_ctx.bus = NULL;
    return err;
}

esp_err_t drv_display_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    esp_err_t err = cmds_write((const uint8_t[]){0xAF}, 1);     /* display on */
    ESP_RETURN_ON_ERROR(err, TAG, "display on failed");
    s_ctx.running = true;
    drv_display_flush();
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t drv_display_stop(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    esp_err_t err = cmds_write((const uint8_t[]){0xAE}, 1);     /* display off */
    ESP_RETURN_ON_ERROR(err, TAG, "display off failed");
    s_ctx.running = false;
    return ESP_OK;
}

esp_err_t drv_display_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (s_ctx.running) {
        drv_display_stop();
    }
    i2c_master_bus_rm_device(s_ctx.dev);
    s_ctx.dev = NULL;
    i2c_del_master_bus(s_ctx.bus);
    s_ctx.bus = NULL;
    s_ctx.initialized = false;
    return ESP_OK;
}

/* ============== 状态 ============== */

bool drv_display_ready(void)        { return s_ctx.initialized && s_ctx.running; }
uint16_t drv_display_width(void)    { return s_ctx.width; }
uint16_t drv_display_height(void)   { return s_ctx.height; }

/* ============== 绘制（只动显存） ============== */

static inline void px_set(int x, int y, bool on)
{
    if (x < 0 || x >= s_ctx.width || y < 0 || y >= s_ctx.height) {
        return;     /* 越界静默忽略，绘制代码不必处处判界 */
    }
    uint8_t *b = &s_ctx.vram[y / 8 * s_ctx.width + x];
    if (on) {
        *b |= (uint8_t)(1u << (y % 8));
    } else {
        *b &= (uint8_t)~(1u << (y % 8));
    }
}

void drv_display_clear(void)
{
    memset(s_ctx.vram, 0, sizeof(s_ctx.vram));
}

void drv_display_set_px(int x, int y, bool on)
{
    px_set(x, y, on);
}

void drv_display_fill_rect(int x, int y, int w, int h, bool on)
{
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            px_set(col, row, on);
        }
    }
}

void drv_display_rect_frame(int x, int y, int w, int h, bool on)
{
    if (w < 2 || h < 2) {
        drv_display_fill_rect(x, y, w, h, on);
        return;
    }
    drv_display_fill_rect(x, y, w, 1, on);
    drv_display_fill_rect(x, y + h - 1, w, 1, on);
    drv_display_fill_rect(x, y, 1, h, on);
    drv_display_fill_rect(x + w - 1, y, 1, h, on);
}

/* ============== 刷新：显存 -> 屏幕（horizontal 模式整帧连写） ============== */

void drv_display_flush(void)
{
    if (!s_ctx.initialized) {
        return;
    }
    esp_err_t err = ESP_OK;
    uint8_t buf[1 + s_ctx.width];
    buf[0] = OLED_DATA_STREAM;
    /* 传输按页宽分块，首块后 RAM 指针自动跨页推进，无需重设地址 */
    for (int off = 0; off < (int)sizeof(s_ctx.vram) && err == ESP_OK; off += s_ctx.width) {
        memcpy(&buf[1], &s_ctx.vram[off], s_ctx.width);
        err = i2c_master_transmit(s_ctx.dev, buf, sizeof(buf), 100);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(err));
    }
}

/* ============== 显示属性 ============== */

void drv_display_set_contrast(uint8_t contrast)
{
    if (!s_ctx.initialized) {
        return;
    }
    cmds_write((const uint8_t[]){0x81, contrast}, 2);
}

void drv_display_invert(bool invert)
{
    if (!s_ctx.initialized) {
        return;
    }
    cmds_write((const uint8_t[]){invert ? 0xA7u : 0xA6u}, 1);
}

/* ============== 控制台命令：disp <info|clear|on|off|invert|contrast|test> ============== */

static void disp_test_pattern(void)
{
    drv_display_clear();
    /* 外边框：验证四边像素寻址 */
    drv_display_rect_frame(0, 0, s_ctx.width, s_ctx.height, true);
    /* 对角线：验证跨页寻址连续性 */
    for (int i = 0; i < s_ctx.width; i++) {
        drv_display_set_px(i, i * s_ctx.height / s_ctx.width, true);
        drv_display_set_px(i, s_ctx.height - 1 - i * s_ctx.height / s_ctx.width, true);
    }
    /* 一对实心"眼睛"：验证块状填充（后续 svc_face 的雏形） */
    drv_display_fill_rect(24, 20, 16, 16, true);
    drv_display_fill_rect(88, 20, 16, 16, true);
    /* 嘴：一条横线 */
    drv_display_fill_rect(48, 50, 32, 2, true);
    drv_display_flush();
    printf("test pattern flushed\n");
}

static int cmd_disp(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: disp <info|clear|on|off|invert|contrast <0-255>|test>\n");
        return 1;
    }
    if (strcmp(argv[1], "info") == 0) {
        printf("SSD1306 %ux%u @0x%02X, %s\n", s_ctx.width, s_ctx.height,
               BOARD_OLED_I2C_ADDR, s_ctx.running ? "on" : "off");
    } else if (strcmp(argv[1], "clear") == 0) {
        drv_display_clear();
        drv_display_flush();
    } else if (strcmp(argv[1], "on") == 0) {
        ESP_ERROR_CHECK(drv_display_start());
    } else if (strcmp(argv[1], "off") == 0) {
        ESP_ERROR_CHECK(drv_display_stop());
    } else if (strcmp(argv[1], "invert") == 0) {
        static bool inv = false;
        inv = !inv;
        drv_display_invert(inv);
        printf("invert=%d\n", inv);
    } else if (strcmp(argv[1], "contrast") == 0 && argc >= 3) {
        drv_display_set_contrast((uint8_t)atoi(argv[2]));
    } else if (strcmp(argv[1], "test") == 0) {
        disp_test_pattern();
    } else {
        printf("unknown sub-command\n");
        return 1;
    }
    return 0;
}

void drv_display_register_console_cmds(void)
{
    const esp_console_cmd_t cmd = {
        .command = "disp",
        .help = "OLED display: disp <info|clear|on|off|invert|contrast|test>",
        .func = cmd_disp,
    };
    if (console_cmd_add(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "register 'disp' cmd failed");
    }
}

#else /* CONFIG_DRV_DISPLAY_ENABLE */

esp_err_t drv_display_init(const drv_display_config_t *config)  { (void)config; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_display_start(void)       { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_display_stop(void)        { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_display_deinit(void)      { return ESP_ERR_NOT_SUPPORTED; }
bool     drv_display_ready(void)        { return false; }
uint16_t drv_display_width(void)        { return 0; }
uint16_t drv_display_height(void)       { return 0; }
void     drv_display_clear(void)        {}
void     drv_display_set_px(int x, int y, bool on)              { (void)x; (void)y; (void)on; }
void     drv_display_fill_rect(int x, int y, int w, int h, bool on) { (void)x; (void)y; (void)w; (void)h; (void)on; }
void     drv_display_rect_frame(int x, int y, int w, int h, bool on){ (void)x; (void)y; (void)w; (void)h; (void)on; }
void     drv_display_flush(void)        {}
void     drv_display_set_contrast(uint8_t contrast)             { (void)contrast; }
void     drv_display_invert(bool invert)                        { (void)invert; }
void     drv_display_register_console_cmds(void)        {}

#endif /* CONFIG_DRV_DISPLAY_ENABLE */
