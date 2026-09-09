#include "console_cmd.h"

#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "argtable3/argtable3.h"
#include "driver/uart.h"

static const char *TAG = "console_cmd";

static esp_console_repl_t *s_repl = NULL;

/* ---- 基础命令实现 ---- */
static int cmd_free(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("free heap: %u bytes\n", (unsigned)esp_get_free_heap_size());
    printf("min free heap: %u bytes\n", (unsigned)esp_get_minimum_free_heap_size());
    return 0;
}

static int cmd_version(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("IDF version: %s\n", esp_get_idf_version());
    return 0;
}

static int cmd_restart(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Restarting...\n");
    esp_restart();
    return 0;
}

esp_err_t console_cmd_init(void)
{
    /* 创建 USB-Serial-JTAG REPL（内部会调用 esp_console_init 初始化 console 模块）
     * 注意：本板仅引出原生 USB，REPL 输入输出均走 USB-Serial-JTAG；
     *       不要在外部重复调用 esp_console_init，否则返回 ESP_ERR_INVALID_STATE */
    esp_console_dev_usb_serial_jtag_config_t usb_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "robot> ";
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb_cfg, &repl_config, &s_repl));

    /* console 模块就绪后注册基础命令 */
    ESP_ERROR_CHECK(esp_console_register_help_command());

    const esp_console_cmd_t cmd_free_entry = {
        .command = "free",
        .help = "Get free heap size",
        .func = cmd_free,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_free_entry));

    const esp_console_cmd_t cmd_version_entry = {
        .command = "version",
        .help = "Print IDF version",
        .func = cmd_version,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_version_entry));

    const esp_console_cmd_t cmd_restart_entry = {
        .command = "restart",
        .help = "Software reset of the chip",
        .func = cmd_restart,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_restart_entry));

    /* 所有基础命令就绪后启动 REPL 任务 */
    ESP_ERROR_CHECK(esp_console_start_repl(s_repl));

    ESP_LOGI(TAG, "console init done");
    return ESP_OK;
}

esp_err_t console_cmd_add(const esp_console_cmd_t *cmd)
{
    if (cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_console_cmd_register(cmd);
}
