# ESP-IDF 嵌入式工程架构规范

> 版本：v1.0
> 适用：ESP32 系列 + ESP-IDF v5.x
> 用途：本规范是新工程脚手架的"宪法"。将本文件投喂给 Agent 并补充产品描述后，Agent 应严格按照本规范生成可编译的工程骨架（目录、CMakeLists、Kconfig、头文件、生命周期空实现），业务逻辑留待后续开发。

---

## 0. 阅读说明

- **强制项**：标题或条目标注 **[强制]** 的，Agent 必须遵守，不得变通。
- **推荐项**：标注 **[推荐]** 的，默认采用，有充分理由时可调整。
- **本规范只管"骨架"**：函数体内部的业务逻辑由后续开发填充，脚手架阶段只写空实现 / 桩代码 / 必要的初始化框架。

---

## 1. 架构总则 [强制]

### 1.1 分层模型

所有工程必须采用 **四层架构**，依赖方向**严格单向、自顶向下**，禁止反向依赖、禁止跨层依赖：

```
┌─────────────────────────────────────────────┐
│  App Layer (应用层)                         │
│  - 产品业务编排、状态机、启动流程            │
│  - 仅依赖 Service 层，禁止直接调用 Driver    │
└──────────────────┬──────────────────────────┘
                   │ depends on
┌──────────────────▼──────────────────────────┐
│  Service Layer (服务层)                      │
│  - 面向业务的功能服务（wifi_mgr、mqtt_cli、  │
│    data_router、provisioner 等）             │
│  - 通过 esp_event 与其他 Service 解耦通信    │
│  - 仅依赖 Driver 层 + Board/HAL 层           │
└──────────────────┬──────────────────────────┘
                   │ depends on
┌──────────────────▼──────────────────────────┐
│  Driver Layer (驱动层)                       │
│  - 芯片外设驱动封装（uart、spi、i2c、gpio、  │
│    led、sensor IC 驱动等）                   │
│  - 纯硬件操作，不含业务逻辑                  │
│  - 仅依赖 Board/HAL 层 + ESP-IDF 组件        │
└──────────────────┬──────────────────────────┘
                   │ depends on
┌──────────────────▼──────────────────────────┐
│  Board/HAL Layer (板级/硬件抽象层)          │
│  - 引脚映射、板级资源定义、时钟/存储配置     │
│  - 仅依赖 ESP-IDF，不依赖任何项目组件        │
└─────────────────────────────────────────────┘
```

**依赖判定规则**：
- 下层头文件**不得** include 上层任何头文件。
- 同层组件之间**禁止直接 include 彼此的内部头文件**；若需通信，走 `esp_event` 事件总线（见第 5 章）。
- `main` 组件属于 App 层，**只能**依赖 Service 层组件，不得直接依赖 Driver 层。

### 1.2 组件化原则

- **每个独立功能单元 = 一个独立 ESP-IDF component**，拥有自己的目录、`CMakeLists.txt`、`idf_component.yml`（如需依赖管理）、可选 `Kconfig`。
- **禁止**把多个业务模块的 `.c` 文件罗列到 `main/CMakeLists.txt` 的 `SRCS` 中（反模式）。
- 组件的对外头文件放在 `include/` 子目录，内部实现放在 `src/`，**对外只暴露必要的公共 API**。
- 可复用、与产品无关的通用能力（日志黑匣子、诊断、环形缓冲、协议解析）应抽为独立 component，禁止塞进 `main`。

### 1.3 可裁剪性

- 所有可选功能模块必须支持**编译期裁剪**：通过 `Kconfig` 的 `bool` 开关控制该 component 是否参与编译，或在 component 内部用 `#if CONFIG_XXX_ENABLE` 包裹实现。
- 裁剪后工程必须仍能编译通过、启动正常。

---

## 2. 标准目录结构 [强制]

新工程必须遵循以下目录结构（方括号表示按需创建）：

```
<project_root>/
├── CMakeLists.txt                  # 顶层，仅包含 project() + 证书/二进制嵌入逻辑
├── sdkconfig.defaults              # 默认配置（版本控制）
├── partitions.csv                  # 分区表
├── README.md                       # 项目说明（产品概述、构建烧录命令）
│
├── main/                           # App 层：只做组装，不含业务逻辑
│   ├── CMakeLists.txt              # 仅注册 main.c，REQUIRES 所有 Service 层组件
│   ├── idf_component.yml           # main 组件依赖声明
│   ├── Kconfig.projbuild           # 项目级 menuconfig 菜单
│   └── main.c                      # app_main：仅调用 app_init() 编排启动
│
├── components/                     # 所有自研组件（Driver + Service 层）
│   │
│   ├── board/                      # [Board/HAL 层] 板级抽象
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── board.h             # 引脚映射、板级资源句柄
│   │   └── src/
│   │       └── board.c             # 板级初始化
│   │
│   ├── drv_<periph>/               # [Driver 层] 外设驱动，每个外设一个 component
│   │   ├── CMakeLists.txt          # 命名：drv_uart / drv_spi / drv_led / drv_sensor_xxx
│   │   ├── include/
│   │   │   └── drv_<periph>.h
│   │   └── src/
│   │       └── drv_<periph>.c
│   │
│   ├── svc_<service>/              # [Service 层] 功能服务，每个服务一个 component
│   │   ├── CMakeLists.txt          # 命名：svc_wifi / svc_mqtt / svc_ble_prov
│   │   ├── idf_component.yml       # 声明对 driver 组件和 ESP-IDF 组件的依赖
│   │   ├── Kconfig                 # 服务级配置（使能开关、参数）
│   │   ├── include/
│   │   │   └── svc_<service>.h     # 对外公共 API
│   │   └── src/
│   │       └── svc_<service>.c
│   │
│   ├── common/                     # [跨层公共] 公共类型、宏、错误码
│   │   ├── include/
│   │   │   ├── app_types.h         # 跨组件共享的数据类型
│   │   │   └── app_events.h        # 自定义事件 ID 集中定义
│   │   └── CMakeLists.txt
│   │
│   ├── evt_log/                    # [基础设施] 事件黑匣子（NVS 环形持久化）
│   ├── diag/                       # [基础设施] 运行时诊断（内存/任务/统计）
│   └── console_cmd/                # [基础设施] 控制台命令注册中心
│
├── docs/                           # 设计文档（不参与编译）
│   └── architecture.md
│
└── [test/]                         # 单元测试（Unity），按需创建
    └── test_<component>.c
```

**目录命名规则**：
- Driver 层组件前缀 `drv_`
- Service 层组件前缀 `svc_`
- Board 层固定名 `board`
- 公共类型组件固定名 `common`
- 基础设施组件使用功能名（`evt_log`、`diag`、`console_cmd`）

---

## 3. Component 标准模板 [强制]

每个 component 必须包含以下内容，缺一不可。

### 3.1 CMakeLists.txt 模板

```cmake
idf_component_register(
    SRCS        "src/svc_<name>.c"
    INCLUDE_DIRS "include"
    REQUIRES    esp_event esp_timer   # 仅列出真实依赖的组件
    PRIV_REQUIRES board drv_<periph>  # 私有依赖（不传递给上层）
)
```

**规则**：
- `INCLUDE_DIRS` 只列 `include`，禁止把 `src` 或项目根目录加入 include 路径。
- 区分 `REQUIRES`（传递给依赖方）和 `PRIV_REQUIRES`（仅本组件内部使用）。能用 `PRIV_REQUIRES` 就不用 `REQUIRES`。
- 禁止在 component 的 CMakeLists 中 `glob` 源文件，必须显式列出。

### 3.2 idf_component.yml 模板（仅当需要声明依赖时）

```yaml
dependencies:
  idf: ">=5.0"
  # 第三方托管组件
  # some_lib: "^1.0.0"
```

- ESP-IDF 自带组件（如 `nvs_flash`、`esp_wifi`）在 CMakeLists 的 REQUIRES 中声明即可，**不写**进 yml。
- 第三方/自研托管组件才写进 yml。

### 3.3 Kconfig 模板（仅当组件有可调参数时）

```kconfig
menu "<Service Name> Configuration"

    config <SVC_NAME>_ENABLE
        bool "Enable <service name>"
        default y
        help
            Compile <service name> service into firmware.

    config <SVC_NAME>_TASK_STACK_SIZE
        int "Task stack size (bytes)"
        default 4096
        depends on <SVC_NAME>_ENABLE
        help
            Stack size of <service name> task.

    config <SVC_NAME>_TASK_PRIORITY
        int "Task priority"
        default 5
        range 1 25
        depends on <SVC_NAME>_ENABLE

endmenu
```

### 3.4 头文件模板（对外公共 API）

```c
/**
 * @file    svc_<name>.h
 * @brief   <一句话描述该服务职责>
 * @note    <使用约束、依赖说明>
 */
#ifndef SVC_<NAME>_H
#define SVC_<NAME>_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"      /* 仅当对外 API 涉及事件时才 include */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 公共类型定义 ---- */
typedef struct {
    /* 对外暴露的配置/状态字段 */
} svc_<name>_config_t;

/* ---- 生命周期 API（所有组件必须实现以下五个函数） ---- */

/**
 * @brief  初始化服务（创建资源、注册事件监听，不启动任务）
 * @return ESP_OK / 错误码
 */
esp_err_t svc_<name>_init(const svc_<name>_config_t *config);

/**
 * @brief  启动服务任务（必须在 init 成功后调用）
 */
esp_err_t svc_<name>_start(void);

/**
 * @brief  停止服务任务（保留资源，可重新 start）
 */
esp_err_t svc_<name>_stop(void);

/**
 * @brief  反初始化（释放所有资源，回到未初始化状态）
 */
esp_err_t svc_<name>_deinit(void);

/**
 * @brief  注册该服务的控制台命令（由 console_cmd 中心统一调用）
 */
void svc_<name>_register_console_cmds(void);

#ifdef __cplusplus
}
#endif

#endif /* SVC_<NAME>_H */
```

### 3.5 源文件骨架模板

```c
#include "svc_<name>.h"

#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "svc_<name>";

/* ---- 私有状态（封装，不对外暴露） ---- */
typedef struct {
    bool            initialized;
    TaskHandle_t    task_handle;
    /* 其他内部状态 */
} svc_<name>_ctx_t;

static svc_<name>_ctx_t s_ctx;   /* 唯一的内部状态实例 */

/* ---- 事件处理（订阅其他组件的事件） ---- */
static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    /* 脚手架阶段留空，仅打印日志 */
    ESP_LOGD(TAG, "event base=%s id=%ld", base, (long)id);
}

/* ---- 服务任务主体 ---- */
static void svc_<name>_task(void *arg)
{
    /* 脚手架阶段仅占位循环 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t svc_<name>_init(const svc_<name>_config_t *config)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* TODO: 初始化内部资源、注册事件监听 */
    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t svc_<name>_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t ret = xTaskCreate(svc_<name>_task, "svc_<name>",
                                 CONFIG_SVC_<NAME>_TASK_STACK_SIZE, NULL,
                                 CONFIG_SVC_<NAME>_TASK_PRIORITY,
                                 &s_ctx.task_handle);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t svc_<name>_stop(void)
{
    /* TODO: 通知任务退出，清理 */
    return ESP_OK;
}

esp_err_t svc_<name>_deinit(void)
{
    /* TODO: 注销事件监听、释放资源 */
    s_ctx.initialized = false;
    return ESP_OK;
}

void svc_<name>_register_console_cmds(void)
{
    /* TODO: 注册该服务专属的 console 命令 */
}
```

---

## 4. 命名约定 [强制]

| 对象 | 规则 | 示例 |
|---|---|---|
| 源文件 | 小写 + 下划线，与组件名一致 | `svc_wifi.c`、`drv_uart.c` |
| 头文件 | 同上，对外头放 `include/` | `svc_wifi.h` |
| Include guard | `SVC_<NAME>_H` 全大写 + 下划线，**无前导下划线** | `SVC_WIFI_H` |
| 组件名 | 小写，层级前缀 | `svc_mqtt`、`drv_led` |
| 公共函数 | `<层前缀>_<模块>_<动作>` 小写下划线 | `svc_wifi_start()`、`drv_uart_send()` |
| 私有函数 | `static` + 无前缀或 `<模块>_` 前缀 | `wifi_event_handler()` |
| 公共类型 | `<层前缀>_<模块>_<语义>_t` | `svc_wifi_config_t` |
| 宏/常量 | 全大写 + 下划线，带模块前缀 | `SVC_WIFI_MAX_RETRY` |
| Kconfig 符号 | 全大写 + 下划线，带模块前缀 | `CONFIG_SVC_WIFI_ENABLE` |
| 日志 TAG | 与组件名一致，带层前缀 | `"svc_wifi"`、`"drv_uart"` |
| 事件 base | 全大写，`<模块>_EVENT` | `WIFI_EVENT`、`MQTT_EVENT` |
| 事件 ID | 全大写，`<模块>_<动作>` | `WIFI_CONNECTED` |

**禁止项**：
- 禁止使用前导下划线的 include guard（如 `_WIFI_DEAL_H_`、`__DIAG_CORE_H__`），这是 C 标准保留给实现的命名空间。
- 禁止在头文件中使用 `typedef struct { ... } *pXxx_t` 形式的指针别名 typedef，指针应显式书写 `xxx_t *`。
- 禁止使用拼音命名。

---

## 5. 事件总线强制规范 [强制]

**模块间通信必须使用 `esp_event`，禁止跨组件直接调用对方的业务函数（生命周期函数除外）。**

### 5.1 事件 ID 集中定义

所有自定义事件 base 和 event ID 统一在 `components/common/include/app_events.h` 中声明：

```c
#ifndef APP_EVENTS_H
#define APP_EVENTS_H

#include "esp_event.h"

/* 事件 base 声明 */
ESP_EVENT_DECLARE_BASE(WIFI_EVENT);
ESP_EVENT_DECLARE_BASE(MQTT_EVENT);
ESP_EVENT_DECLARE_BASE(BLE_EVENT);
ESP_EVENT_DECLARE_BASE(DATA_EVENT);

/* 各 base 下的 event ID */
typedef enum {
    WIFI_CONNECTED,          /* arg: NULL */
    WIFI_DISCONNECTED,       /* arg: wifi_disconnected_reason_t* */
    WIFI_GOT_IP,             /* arg: ip_event_got_ip_t* */
} wifi_event_id_t;

typedef enum {
    MQTT_CONNECTED,
    MQTT_DISCONNECTED,
    MQTT_MESSAGE_RECEIVED,   /* arg: mqtt_message_t*  */
} mqtt_event_id_t;

#endif /* APP_EVENTS_H */
```

对应的 base 定义放在某个公共源文件（如 `common/src/app_events.c`）：

```c
ESP_EVENT_DEFINE_BASE(WIFI_EVENT);
ESP_EVENT_DEFINE_BASE(MQTT_EVENT);
```

### 5.2 事件循环

- 使用**默认事件循环** `esp_event_loop_create_default()`，由 `board` 或 `app_init` 在启动早期创建一次。
- 组件通过 `esp_event_handler_register(BASE, ID, handler, arg)` 订阅，通过 `esp_event_post(BASE, ID, data, data_size, ticks_to_wait)` 发布。

### 5.3 数据所有权契约（关键）

事件载荷 `data` 的内存所有权必须明确，分两种模式：

**模式 A：值传递（推荐用于小数据）**
- 发布方将数据**拷贝**到事件循环内部缓冲（`esp_event_post` 会 copy `data_size` 字节）。
- 发布方在 `esp_event_post` 返回后即可释放/复用自己的缓冲区。
- 订阅方收到的 `data` 指针在回调执行期间有效，回调返回后失效。若需跨回调保留，必须自行拷贝。

**模式 B：指针传递（用于大数据，必须遵守以下契约）**
- 发布方 `malloc` 一块内存，填入数据，将**指针的地址**或指针本身作为 `data` 发布。
- 必须明确**唯一所有权转移**：要么约定"订阅方负责 free"，要么约定"发布方在确认所有订阅者处理完后 free"。
- **强制要求**：使用指针传递时，必须在 `app_events.h` 中该 event ID 的注释里写明所有权规则。
- **禁止**让多个订阅者同时 free 同一块内存。
- **禁止**发布指向栈内存的指针。

> 反模式警告：不要把 `esp_event` 的 pub/sub 与 FreeRTOS Queue 语义混用。若某模块需要阻塞等待特定事件，应在该模块内部维护一个**专用单消费者队列**，在事件回调中把数据**深拷贝**入队，由模块任务从队列取出处理。

### 5.4 事件使用示例

发布方（Service A）：
```c
mqtt_message_t *msg = malloc(sizeof(mqtt_message_t));
/* 填充 msg ... */
esp_event_post(MQTT_EVENT, MQTT_MESSAGE_RECEIVED,
               &msg, sizeof(msg), pdMS_TO_TICKS(100));
/* 发布后 msg 的所有权已转移，订阅方负责 free */
```

订阅方（Service B）：
```c
static void mqtt_msg_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    mqtt_message_t *msg = *(mqtt_message_t **)data;
    /* 处理 msg ... */
    free(msg);   /* 按契约，订阅方负责释放 */
}

esp_event_handler_register(MQTT_EVENT, MQTT_MESSAGE_RECEIVED,
                           mqtt_msg_handler, NULL);
```

---

## 6. 模块生命周期规范 [强制]

### 6.1 五段式生命周期

所有 Service 层和 Driver 层组件必须实现且仅实现以下五个公共函数（见 3.4 节模板）：

| 函数 | 职责 | 可被谁调用 |
|---|---|---|
| `init()` | 分配资源、注册事件监听、初始化硬件；**不创建任务、不启动通信** | App 层 |
| `start()` | 创建任务、启动通信/采集 | App 层（init 成功后） |
| `stop()` | 停止任务、暂停通信；保留资源，可重新 start | App 层 |
| `deinit()` | 注销事件、释放全部资源；回到未 init 状态 | App 层 |
| `register_console_cmds()` | 注册该组件的调试命令 | console_cmd 中心 |

**规则**：
- `init` 与 `start` 分离，保证内存敏感场景可分阶段初始化（类比 TLS 握手前不分配大缓冲的模式）。
- 每个组件内部维护 `initialized` / `running` 状态标志，重复调用必须返回 `ESP_ERR_INVALID_STATE` 而非崩溃。
- `deinit` 必须能完全回滚 `init` 的所有副作用，使组件可重新 `init`。

### 6.2 启动编排（App 层职责）

`main/app_init()`（由 `app_main` 调用）负责按依赖顺序编排所有组件的生命周期，示例：

```c
void app_init(void)
{
    /* 1. Board/HAL 层 */
    ESP_ERROR_CHECK(board_init());

    /* 2. 基础设施 */
    ESP_ERROR_CHECK(nvs_flash_init_wrapper());
    ESP_ERROR_CHECK(evt_log_init());
    ESP_ERROR_CHECK(diag_init(CONFIG_DIAG_INTERVAL_SEC));
    ESP_ERROR_CHECK(console_cmd_init());

    /* 3. Driver 层 init（不 start） */
    ESP_ERROR_CHECK(drv_uart_init(...));
    ESP_ERROR_CHECK(drv_led_init(...));

    /* 4. Service 层 init + start（按依赖顺序） */
    ESP_ERROR_CHECK(svc_wifi_init(NULL));
    ESP_ERROR_CHECK(svc_wifi_start());
    /* ... 其他服务 */
}
```

**规则**：
- App 层**只做编排**，不实现任何业务逻辑。
- 启动顺序必须显式、可读，禁止用隐式依赖。
- 关键步骤失败用 `ESP_ERROR_CHECK` 终止；非关键步骤失败应记录日志并继续（优雅降级）。

---

## 7. 配置规范 [强制]

### 7.1 Kconfig 与头文件常量的分工

| 内容 | 放置位置 |
|---|---|
| 功能使能开关（是否编译某模块） | **Kconfig** `bool` |
| 任务栈大小、优先级、队列长度、超时、重试次数 | **Kconfig** `int` |
| 网络地址、端口、Topic、证书路径 | **Kconfig** `string/int` |
| 协议固定常量（帧头、校验码、字段长度、状态枚举值） | **头文件** `#define` / `enum` |
| 编译期不可调的内部限制 | **头文件** `#define` |

**规则**：
- 凡是产品量产时可能需要调整的参数，**必须**走 Kconfig，禁止写死在头文件。
- Kconfig 项必须有 `help` 说明。
- 头文件中的 `#define` 常量必须带模块前缀。

### 7.2 禁止使用"任务开关宏"

反模式：在头文件中用 `#define WIFI_TASK_ENABLE 1` 控制模块是否编译。
正确方式：在该组件的 `Kconfig` 中定义 `CONFIG_SVC_WIFI_ENABLE`，在 `CMakeLists.txt` 中根据该配置决定是否注册源文件：

```cmake
if(CONFIG_SVC_WIFI_ENABLE)
    idf_component_register(SRCS "src/svc_wifi.c" ...)
else()
    idf_component_register()  # 空组件，或提供 stub
endif()
```

---

## 8. main 组件职责边界 [强制]

`main` 组件是 App 层的唯一入口，**必须遵守以下限制**：

1. `main/CMakeLists.txt` 的 `SRCS` **只能包含 `main.c`**，禁止把业务模块的 `.c` 塞进来。
2. `main.c` 中**只能**：
   - 打印启动日志（芯片型号、IDF 版本、可用内存）。
   - 调用 `app_init()` 进行启动编排。
   - 不包含任何业务逻辑、不直接操作硬件、不直接调用 Driver 层函数。
3. `main` 组件的 `REQUIRES` 只能列出 **Service 层组件 + 基础设施组件**，禁止列出 Driver 层组件。
4. `app_init()` 函数可放在 `main.c` 或单独的 `app_init.c` 中，但必须属于 `main` 组件。

---

## 9. 基础设施要求 [推荐]

每个工程应内置以下基础设施组件（脚手架阶段至少提供空实现 + 接口）：

### 9.1 日志
- 统一使用 `ESP_LOGx`，每个组件定义自己的 `TAG`（与组件名一致）。
- 禁止使用 `printf` 直接输出。

### 9.2 事件黑匣子（evt_log）
- 环形缓冲区，NVS 持久化，跨重启保留。
- 只记录白名单内的关键事件（启动、连接、断开、错误、丢包等），每条定长，防 flash 磨损。
- 提供 console 命令读取/清空。

### 9.3 诊断（diag）
- 周期性采集内存（free / largest block / min free ever）、任务栈高水位、各模块统计。
- 支持 console 命令手动触发快照。

### 9.4 控制台命令（console_cmd）
- 提供统一的命令注册中心，各组件通过 `register_console_cmds()` 注册自己的命令。
- 默认包含 `help`、`restart`、`version`、`free` 等基础命令。

### 9.5 内存快照工具
- 提供统一的 `mem_snapshot(stage)` 函数，打印 free/largest/min_free_ever，TAG 固定为 `MEM_SNAP`，便于 grep。

---

## 10. 错误处理规范 [强制]

1. 所有对外 API 返回 `esp_err_t`，**禁止**返回 `void` 却可能失败的函数。
2. 内部私有函数可返回 `bool` 或具体错误码，但对外必须统一为 `esp_err_t`。
3. 致命错误（启动关键路径）用 `ESP_ERROR_CHECK` 终止并打印原因。
4. 非致命错误必须用 `ESP_LOGW` / `ESP_LOGE` 记录，**禁止静默失败**。
5. 错误码优先使用 ESP-IDF 预定义错误（`ESP_FAIL`、`ESP_ERR_INVALID_ARG`、`ESP_ERR_INVALID_STATE`、`ESP_ERR_NO_MEM`、`ESP_ERR_TIMEOUT` 等），不要自定义。

---

## 11. 头文件规范 [强制]

1. **对外头文件**（`include/` 下）只包含：
   - 公共类型定义
   - 公共函数声明
   - 公共宏/常量
2. **对外头文件禁止 include 以下内容**：
   - 其他组件的内部头文件
   - `driver/gpio.h`、`driver/uart.h` 等具体硬件驱动头（除非该 API 签名直接用到相关类型）
   - `freertos/FreeRTOS.h`（除非 API 签名用到 `TickType_t` 等 FreeRTOS 类型）
3. **最小包含原则**：头文件只 include 其声明所必需的头文件，用前向声明替代不必要的 include。
4. 每个对外头文件必须有文件级 Doxygen 注释（`@file`、`@brief`）。
5. 每个对外函数必须有 Doxygen 注释（`@brief`、`@param`、`@return`）。

---

## 12. 可裁剪性规范 [强制]

1. 每个 Service 组件必须有 `CONFIG_<SVC>_ENABLE` 开关。
2. 关闭开关后，该组件不参与编译，且工程仍可正常启动。
3. App 层的启动编排中，对可选服务用 `#if CONFIG_<SVC>_ENABLE` 包裹调用。
4. 可选服务之间的事件依赖必须处理"发布方未编译"的情况：订阅方在未收到事件时应安全降级。

---

## 13. 第三方组件管理 [推荐]

1. ESP-IDF 自带组件直接在 CMakeLists 的 `REQUIRES` 中引用。
2. 第三方托管组件通过 `idf_component.yml` 声明，版本号固定。
3. **禁止**将第三方库源码拷贝进 `components/` 后直接修改。如需修改，应 fork 并通过 `idf_component.yml` 的 `git` 源引用，或在自有 component 中封装适配。
4. 自研可复用组件（协议解析、算法等）应发布为独立 component，通过 `idf_component.yml` 引用，而非拷贝源码。

---

## 14. Agent 脚手架执行指令

> 以下内容是给 Agent 的操作指令。当你（Agent）收到本规范 + 一份产品描述时，严格按以下步骤生成工程骨架。

### 步骤 1：解析产品描述

从产品描述中提取以下信息，若缺失则向用户询问：
- 目标芯片（ESP32 / ESP32-C3 / ESP32-S3 等）
- ESP-IDF 版本（默认 v5.x）
- 核心功能模块列表（如：WiFi 联网、MQTT 上报、BLE 配网、UART 传感器、LoRa 通信等）
- 是否需要 OTA、是否需要证书/TLS、是否需要显示屏
- 分区要求（是否需要 OTA 分区、NVS 大小等）

### 步骤 2：规划组件清单

根据产品描述，按四层模型列出所有组件，例如：

| 层 | 组件名 | 职责 |
|---|---|---|
| Board | `board` | 引脚映射、板级初始化 |
| Driver | `drv_uart` | UART 驱动封装 |
| Driver | `drv_led` | LED 控制 |
| Service | `svc_wifi` | WiFi 连接管理 |
| Service | `svc_mqtt` | MQTT 客户端 |
| Service | `svc_ble_prov` | BLE 配网 |
| Service | `svc_data_router` | 数据路由/转发 |
| 基础设施 | `evt_log` | 事件黑匣子 |
| 基础设施 | `diag` | 运行诊断 |
| 基础设施 | `console_cmd` | 控制台命令中心 |
| 公共 | `common` | 共享类型、事件定义 |

### 步骤 3：生成目录与文件

按第 2 章目录结构创建所有目录。对每个组件，按第 3 章模板生成：
- `CMakeLists.txt`
- `idf_component.yml`（如需）
- `Kconfig`（如有可调参数）
- `include/<name>.h`（按 3.4 模板）
- `src/<name>.c`（按 3.5 骨架模板）

### 步骤 4：生成公共事件定义

在 `components/common/include/app_events.h` 中，根据组件清单声明所有事件 base 和 event ID，并在 `components/common/src/app_events.c` 中定义 base。每个 event ID 必须注释数据类型和所有权规则（见 5.3 节）。

### 步骤 5：生成 main 组件

- `main/CMakeLists.txt`：仅 `SRCS "main.c"`，`REQUIRES` 列出所有 Service + 基础设施组件。
- `main/main.c`：`app_main` 调用 `app_init()`，`app_init` 按第 6.2 节模板编排启动顺序。
- `main/Kconfig.projbuild`：项目级配置菜单。

### 步骤 6：生成构建配置

- 顶层 `CMakeLists.txt`（`project()` 调用）
- `partitions.csv`（根据分区要求）
- `sdkconfig.defaults`（控制台、日志级别、FreeRTOS 等基础配置）

### 步骤 7：校验

- 确认所有组件的依赖方向符合第 1.1 节分层规则。
- 确认没有跨组件直接业务调用，模块间通信全部走 `esp_event`。
- 确认 `main/CMakeLists.txt` 的 `SRCS` 只有 `main.c`。
- 确认所有对外头文件 include guard 无前导下划线。
- 确认所有可裁剪组件有 `CONFIG_<SVC>_ENABLE` 开关。

### 输出要求

- 生成的代码必须**能通过编译**（`idf.py build` 不报错），即使函数体是空实现。
- 不要编写任何业务逻辑，只搭骨架。
- 在每个 TODO 处用 `/* TODO: <具体待实现内容> */` 标记，便于后续开发定位。
- 在工程根目录生成 `README.md`，说明产品概述、组件清单、构建烧录命令。

---

## 附录 A：反模式清单（禁止出现）

| 反模式 | 正确做法 |
|---|---|
| 把所有 `.c` 列在 `main/CMakeLists.txt` | 每个模块拆成独立 component |
| `#include "system_deal.h"` 作为公共依赖 | 公共类型放 `common`，各组件按需 include |
| 模块 A 直接调用模块 B 的业务函数 | 通过 `esp_event` 发布/订阅 |
| 头文件中 `#define MODULE_ENABLE 1` 控编译 | 用 Kconfig `CONFIG_MODULE_ENABLE` |
| include guard 用 `_XXX_H_` 或 `__XXX_H__` | 用 `XXX_H` |
| `typedef struct {...} *pXxx_t` | 显式写 `xxx_t *` |
| `printf` 输出日志 | `ESP_LOGx(TAG, ...)` |
| 返回 `void` 但可能失败的函数 | 返回 `esp_err_t` |
| 全局变量暴露模块状态 | 封装在组件内部 ctx，通过 API 暴露 |
| 事件载荷指向栈内存 | 用值拷贝或 malloc + 所有权契约 |
| main.c 中直接操作硬件 | main 只做编排，硬件操作在 driver 层 |

---

## 附录 B：组件依赖声明速查

```
main (App)
  └─ REQUIRES: svc_wifi svc_mqtt svc_ble_prov svc_data_router evt_log diag console_cmd common

svc_wifi (Service)
  └─ REQUIRES: esp_wifi esp_event nvs_flash common
     PRIV_REQUIRES: board

svc_mqtt (Service)
  └─ REQUIRES: esp_event mqtt common
     PRIV_REQUIRES: board

drv_uart (Driver)
  └─ REQUIRES: driver
     PRIV_REQUIRES: board

board (Board/HAL)
  └─ REQUIRES: driver esp_hw_support

common
  └─ REQUIRES: esp_event
```

---

*本规范为脚手架生成的唯一依据。若产品描述与本规范冲突，以本规范为准；若本规范未覆盖某场景，Agent 应遵循"分层单向依赖、事件解耦、组件化、显式配置"的四项基本原则处理。*
