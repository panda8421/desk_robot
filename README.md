# Desktop Robot (桌面机器人)

基于 ESP32-S3 的桌面机器人固件骨架。当前实现 2 自由度 Pan-Tilt 云台（双 PWM 舵机），预留显示屏与摄像头扩展位。

## 硬件规格

| 项目 | 规格 |
|---|---|
| 主控 | ESP32-S3-WROOM-1 R8 |
| Flash | 16 MB |
| PSRAM | 8 MB (Octal) |
| 舵机 | 2 × PWM 舵机（Pan / Tilt） |
| 联网 | WiFi (STA) |
| 交互 | UART 控制台 |

## 架构

遵循四层单向依赖架构（见 `doc/ESP_IDF_ARCHITECTURE_SPEC.md`）：

```
App       main (启动编排)
  │
Service   svc_pan_tilt   svc_wifi
  │
Driver    drv_servo_pwm  [drv_display]  [drv_camera]
  │
Board     board (引脚映射 / NVS / 事件循环)
```

基础设施：`common`（类型/事件）、`evt_log`（黑匣子）、`diag`（诊断）、`console_cmd`（控制台）。

## 组件清单

| 层 | 组件 | 职责 | 开关 |
|---|---|---|---|
| Board | `board` | 引脚映射、NVS、事件循环 | — |
| Driver | `drv_servo_pwm` | MCPWM PWM 舵机驱动 | `CONFIG_DRV_SERVO_PWM_ENABLE` |
| Driver | `drv_display` | 显示屏驱动（占位） | `CONFIG_DRV_DISPLAY_ENABLE` (默认关) |
| Driver | `drv_camera` | DVP 摄像头驱动（占位） | `CONFIG_DRV_CAMERA_ENABLE` (默认关) |
| Service | `svc_pan_tilt` | 云台平滑插值、限位 | `CONFIG_SVC_PAN_TILT_ENABLE` |
| Service | `svc_wifi` | WiFi STA 连接管理 | `CONFIG_SVC_WIFI_ENABLE` |
| Infra | `evt_log` | 事件黑匣子（NVS 环形） | `CONFIG_EVT_LOG_ENABLE` |
| Infra | `diag` | 内存/任务诊断 | `CONFIG_DIAG_ENABLE` |
| Infra | `console_cmd` | UART 控制台命令中心 | — |
| Common | `common` | 共享类型、事件定义 | — |

## 构建与烧录

```bash
# 设置目标芯片
idf.py set-target esp32s3

# 配置（可选：修改 WiFi SSID/密码、舵机角度范围等）
idf.py menuconfig
#   → WiFi Service Configuration  → 修改 SSID / 密码
#   → Pan-Tilt Service Configuration → 调整限位角度

# 编译
idf.py build

# 烧录 + 监控
idf.py -p /dev/ttyUSB0 flash monitor
```

## 控制台命令

启动后串口进入 `robot> ` 提示符，内置命令：

| 命令 | 说明 |
|---|---|
| `help` | 列出所有命令 |
| `free` | 查看剩余堆内存 |
| `version` | 打印 IDF 版本 |
| `restart` | 软复位 |

> 舵机控制命令待实现（见各组件 `*_register_console_cmds()` 的 TODO）。

## 引脚映射

| 功能 | GPIO |
|---|---|
| Pan 舵机 PWM | GPIO4 |
| Tilt 舵机 PWM | GPIO5 |

> 引脚定义在 `components/board/include/board.h`，可按需修改。

## 开发约定

- 所有组件遵循五段式生命周期：`init` / `start` / `stop` / `deinit` / `register_console_cmds`
- 跨组件通信走 `esp_event`，事件 ID 集中在 `components/common/include/app_events.h`
- 可选功能通过 Kconfig `bool` 开关编译期裁剪
- 详细规范见 `doc/ESP_IDF_ARCHITECTURE_SPEC.md`
