# 语音聊天机器人详细设计

> 版本：v1.0
> 日期：2026-09-10
> 平台：正点原子 DNESP32S3（ESP32-S3-WROOM-1 R8，16MB Flash / 8MB Octal PSRAM）
> 关联文档：`ESP_IDF_ARCHITECTURE_SPEC.md`（架构规范）、`正点原子 DNESP32S3 硬件参考手册 V1.0.md`（硬件）

---

## 1. 目标与范围

### 1.1 产品目标

桌面机器人通过语音与人对话：

```
唤醒词唤醒 → 用户说话 → 云端识别(ASR) → 大模型回复(LLM) → 云端合成语音(TTS) → 喇叭播放
```

- **唤醒方式**：第一版用 **KEY0 按键**触发对话；唤醒词为 P2 预留设计
- **云端服务**：阿里 DashScope（ASR + LLM + TTS 一站式，国内直连）
- **交互兜底**：板上按键（XL9555 扩展）触发对话、重说、打断播放

### 1.2 分阶段范围

| 阶段 | 能力 | 说明 |
|---|---|---|
| **P1（第一版，本文实施范围）** | 按键触发 + HTTP 串行流水线 | 先打通端到端，延迟 2~4s；**不引入 esp-sr** |
| P2（后续版本） | 唤醒词 + AFE（AEC/VAD/降噪） | "喊一声即答"，播放时可语音打断 |
| P3（后续版本） | 升级 Realtime WebSocket 全双工 | 延迟压到 1s 级，流式对话 |

> 第一版按 P1 落地：唤醒由 **KEY0 按键**触发，VAD 用**能量阈值断句**。事件、API、状态机中与唤醒词相关的部分按最终形态**预留定义但不实现**，保证 P2 升级时上层不动。

---

## 2. 硬件资源

### 2.1 音频链路（板载，无外加硬件）

```
录音：板载 MIC ──> ES8388 ADC ──> I2S0 读（SDIN）
播放：I2S0 写（SDOUT）──> ES8388 DAC ──> MD8002A 功放 ──> 板载 8Ω/1W 喇叭
                                        └────────> PHONE 3.5mm 耳机口
```

### 2.2 引脚分配（新增到 `board.h`）

| 信号 | GPIO | 说明 |
|---|---|---|
| I2S_MCLK | IO3 | 主时钟，I2S 外设输出给 ES8388 |
| I2S_SCK | IO46 | 位时钟 |
| I2S_LRCK | IO9 | 帧时钟 |
| I2S_SDIN | IO10 | 录音数据（ES8388 ADC → S3） |
| I2S_SDOUT | IO14 | 播放数据（S3 → ES8388 DAC） |
| IIC0_SDA / SCL | IO41 / IO42 | ES8388、XL9555、AP3216C、24C02 **共享总线** |
| XL9555 P04~P07 | — | KEY0~KEY3 按键（低电平有效） |
| XL9555 P03 | — | 有源蜂鸣器 BEEP（提示音备用） |

### 2.3 资源约束（必须遵守）

1. **RGB 屏（J1 接口）与音频互斥**：RGB 屏的 G2/G3/G4/G5/R7 与 I2S 五根信号线物理并联（IO3/9/46/10/14）。语音功能启用期间禁止点亮 RGB 屏。**显示选 SPI 屏**（IO11/12/13/21/40，与音频零冲突）。
2. **I2C0 为共享总线**：ES8388 配置、XL9555 读写必须经 Board 层统一的总线管理（互斥锁），不得各自独占初始化。
3. **USB_SLAVE 与喇叭功放共用**：使用 USB_SLAVE(JTAG) 时喇叭不可用。
4. **供电**：WiFi 发射 + 功放同时工作时电流较大，建议双 Type-C 供电。

---

## 3. 总体架构

### 3.1 新增组件

遵循四层单向依赖，新增组件如下：

```
App       main（启动编排）+ conversation（对话状态机编排）
            │
Service   svc_audio        svc_ai_chat
            │
Driver    drv_audio   drv_es8388   drv_xl9555
            │
Board     board（I2C0 总线管理 + 音频引脚 + XL9555 引脚）
```

| 层 | 组件 | 职责 | Kconfig 开关 |
|---|---|---|---|
| Board | `board`(扩展) | I2C0 总线初始化/互斥、音频与按键引脚宏 | — |
| Driver | `drv_xl9555` | IO 扩展芯片驱动：按键扫描、蜂鸣器 | `CONFIG_DRV_XL9555_ENABLE` |
| Driver | `drv_es8388` | 音频 Codec 寄存器配置：通路/音量/增益 | `CONFIG_DRV_ES8388_ENABLE` |
| Driver | `drv_audio` | I2S 全双工数据通路：PCM 帧读/写 | `CONFIG_DRV_AUDIO_ENABLE` |
| Service | `svc_audio` | 音频前端：VAD 断句（P1 能量阈值）、录音/播放调度、提示音（P2 加唤醒词/AEC） | `CONFIG_SVC_AUDIO_ENABLE` |
| Service | `svc_ai_chat` | DashScope 客户端：ASR/LLM/TTS 请求与会话管理 | `CONFIG_SVC_AI_CHAT_ENABLE` |
| App | `main/conversation` | **对话状态机**：监听事件、调度 service、超时/异常兜底 | — |

依赖关系说明：

- `main/conversation` 是唯一"知道全局流程"的地方，**只调用 Service 层公开 API**，不碰 Driver。
- `svc_audio` 与 `svc_ai_chat` 同层，**互不 include**，通过 `esp_event` 事件总线解耦。
- `svc_audio` 依赖 `drv_audio` + `drv_es8388`；esp-sr（AFE/WakeNet）作为 `svc_audio` 的私有依赖引入。

### 3.2 端到端数据流（P1 形态）

```
                 ┌────────────────────────── main / conversation（状态机）──────────────────────────┐
                 │                        │                              │                          │
            (按键/唤醒)              (ASR文本/LLM回复)               (播放完成/打断)                  │
                 ▼                        ▼                              ▼                          │
┌────────────────────────┐   ┌─────────────────────────┐   ┌──────────────────────────┐            │
│ svc_audio              │   │ svc_ai_chat             │   │ svc_audio                │            │
│  唤醒/VAD/录音调度      │   │  ① ASR: 音频→文本       │   │  TTS音频流→I2S播放       │            │
│  录音PCM(PSRAM环形缓冲) │──▶│  ② LLM: 文本→回复       │──▶│  wav解码→PCM→drv_audio   │            │
│                        │   │  ③ TTS: 回复→音频URL/流  │   │                          │            │
└────────────────────────┘   └─────────────────────────┘   └──────────────────────────┘            │
        │            ▲                       │                                    ▲                  │
        └── esp_event(AUDIO_EVENT) ──┘        └──── esp_event(CHAT_EVENT) ────────┘                  │
```

P3（Realtime WebSocket）时仅替换 `svc_ai_chat` 内部协议实现，PCM 流直接在 `svc_audio` ↔ `svc_ai_chat` 间通过环形缓冲对接，状态机不变。

---

## 4. 对话状态机（main/conversation）

### 4.1 状态定义

```c
typedef enum {
    CHAT_STATE_BOOT,         /* 上电初始化中 */
    CHAT_STATE_NET_WAIT,     /* 等待 WiFi 连接（LED 慢闪） */
    CHAT_STATE_STANDBY,      /* 待机：唤醒词监听中（LED 心跳） */
    CHAT_STATE_LISTENING,    /* 录音中：等待 VAD 断句 */
    CHAT_STATE_RECOGNIZING,  /* ASR 上行识别中 */
    CHAT_STATE_THINKING,     /* LLM 生成回复中 */
    CHAT_STATE_SPEAKING,     /* TTS 音频播放中 */
    CHAT_STATE_ERROR,        /* 错误态：可按键/唤醒恢复 */
} chat_state_t;
```

### 4.2 状态转移图

```
                        WiFi GOT_IP
   BOOT ──▶ NET_WAIT ──────────────▶ STANDBY ◀─────────────────────┐
              │                     │    │                          │
              │ WiFi 断开            │    │ 唤醒词命中/KEY0 按下      │
              └─────────────────────┘    ▼                          │
                                       LISTENING ──(录音出错)──▶ ERROR
                                      VAD断句│  ▲                   │
                                             ▼  │KEY0(重说)          │
                                      RECOGNIZING                  │
                                             │ ASR结果             │
                                             ▼                     │
                                         THINKING ──(请求失败)──▶ ERROR
                                             │ LLM回复完成          │
                                             ▼                     │
                                         SPEAKING ──(播放失败)──▶ ERROR
                                             │                     │
                       播放完成 / KEY1(打断)  │                     │
                             ┌───────────────┘                     │
                             ▼                                     │
                         STANDBY ◀────────────── 恢复(按键/唤醒) ───┘
```

### 4.3 关键转移逻辑

| 当前态 | 触发 | 动作 | 次态 |
|---|---|---|---|
| NET_WAIT | `WIFI_GOT_IP` | `svc_audio_start()` 启动音频服务 | STANDBY |
| STANDBY | 2 ；`s按下（P2 增加：唤醒词命中） vc_audio_start_recording()` | LISTENING |
| LISTENING | `AUDIO_VAD_SPEECH_END` | `svc_ai_chat_recognize(录音缓冲)` | RECOGNIZING |
| LISTENING | 15s 无有效语音（超时） | 播放提示音 | STANDBY |
| RECOGNIZING | `CHAT_ASR_RESULT` | `svc_ai_chat_ask(text)` | THINKING |
| RECOGNIZING | `CHAT_ERROR`(ASR失败) | 提示音"没听清" | STANDBY |
| THINKING | `CHAT_TTS_READY`(回复+TTS音频就绪/流开始) | `svc_audio_play(url/buffer)` | SPEAKING |
| THINKING | 30s 超时 / `CHAT_ERROR` | 提示音 | STANDBY |
| SPEAKING | `AUDIO_PLAYBACK_DONE` | — | STANDBY |
| SPEAKING | KEY1 按下（P2 AEC 就绪后增加：唤醒词打断） | `svc_audio_stop_play()`；丢弃剩余回复 | STANDBY |
| ERROR | 按键 / 唤醒词 | 清理会话，重启监听 | STANDBY |
| 任意态 | `WIFI_DISCONNECTED` | 停止录音/播放，提示 | NET_WAIT |

### 4.4 会话上下文（多轮对话）

由 `conversation` 持有，组织成 OpenAI 兼容 messages 数组传给 LLM：

- 保留最近 **4 轮**（8 条消息）+ 1 条 system prompt，防止上下文超长
- 机器人进入 STANDBY 超过 **5 分钟**自动清空上下文（重新开始会话）
- system prompt 放 Kconfig 可配置字符串（默认定义机器人人设）

---

## 5. 事件定义（追加到 `components/common/include/app_events.h`）

```c
/* ============== 事件 base 声明（新增） ============== */
ESP_EVENT_DECLARE_BASE(AUDIO_EVENT);   /* svc_audio 发布 */
ESP_EVENT_DECLARE_BASE(CHAT_EVENT);    /* svc_ai_chat 发布 */

/* ============== AUDIO_EVENT（svc_audio → 状态机） ============== */
typedef enum {
    /* arg: NULL —— 唤醒词命中 */
    AUDIO_WAKE_WORD_DETECTED,
    /* arg: NULL —— VAD 检测到说话开始（录音中） */
    AUDIO_VAD_SPEECH_START,
    /* arg: NULL —— VAD 断句（一段话说完），录音数据可取 */
    AUDIO_VAD_SPEECH_END,
    /* arg: NULL —— 一段音频播放完成 */
    AUDIO_PLAYBACK_DONE,
    /* arg: int32_t*（错误码） —— 音频子系统错误 */
    AUDIO_ERROR,
} audio_event_id_t;

/* ============== CHAT_EVENT（svc_ai_chat → 状态机） ============== */
typedef enum {
    /* arg: chat_text_t*（识别文本，值传递） */
    CHAT_ASR_RESULT,
    /* arg: chat_text_t*（LLM 完整回复文本） */
    CHAT_LLM_REPLY,
    /* arg: NULL —— TTS 音频已就绪/开始可播 */
    CHAT_TTS_READY,
    /* arg: chat_err_info_t* —— 云端链路错误 */
    CHAT_ERROR,
} chat_event_id_t;
```

配套类型追加到 `app_types.h`：

```c
/* ---- 文本消息载荷（esp_event 值传递） ---- */
typedef struct {
    char text[256];          /* UTF-8 文本 */
} chat_text_t;

/* ---- 云端错误载荷 ---- */
typedef struct {
    int32_t code;            /* 业务错误码 */
    char    stage;           /* 'a'=ASR 'l'=LLM 't'=TTS */
} chat_err_info_t;
```

> 事件载荷遵循现有规范：`esp_event_post` 内部拷贝，小结构体值传递；录音音频**不走事件总线**，由 `svc_audio` 内部缓冲，`svc_ai_chat` 通过 API 拉取。

---

## 6. 组件 API 设计（五段式生命周期）

### 6.1 board（扩展，无独立新组件）

```c
/* board.h 追加 */
#define BOARD_I2C0_SDA_GPIO     GPIO_NUM_41
#define BOARD_I2C0_SCL_GPIO     GPIO_NUM_42
#define BOARD_I2C0_FREQ_HZ      100000          /* 共享总线保守速率 */

#define BOARD_I2S_MCLK_GPIO     GPIO_NUM_3
#define BOARD_I2S_SCK_GPIO      GPIO_NUM_46
#define BOARD_I2S_LRCK_GPIO     GPIO_NUM_9
#define BOARD_I2S_SDIN_GPIO     GPIO_NUM_10     /* 录音 */
#define BOARD_I2S_SDOUT_GPIO    GPIO_NUM_14     /* 播放 */

#define BOARD_ES8388_I2C_ADDR   0x10            /* ES8388 从机地址 */

/* 共享 I2C0 总线管理（drv_es8388 / drv_xl9555 / 未来 ap3216c 共用） */
esp_err_t board_i2c_init(void);                  /* board_init 内自动调用 */
i2c_master_dev_handle_t board_i2c_get_dev(uint8_t addr);  /* 挂载子设备 */
void board_i2c_lock(void);                       /* 事务级互斥 */
void board_i2c_unlock(void);
```

### 6.2 drv_xl9555 —— IO 扩展芯片

```c
esp_err_t drv_xl9555_init(void);
esp_err_t drv_xl9555_start(void);
esp_err_t drv_xl9555_stop(void);
esp_err_t drv_xl9555_deinit(void);
void      drv_xl9555_register_console_cmds(void);

/* 业务 API */
esp_err_t drv_xl9555_read_keys(uint8_t *key_bits);   /* KEY0~3 位图，低有效 */
esp_err_t drv_xl9555_set_pin(uint8_t pin, bool level); /* 蜂鸣器等输出 */
```

- 内部以 20ms 周期任务扫描按键，消抖后发布到默认事件循环（自定义 `KEY_EVENT`）。

### 6.3 drv_es8388 —— 音频 Codec 配置

```c
esp_err_t drv_es8388_init(void);
esp_err_t drv_es8388_start(void);      /* 上电、默认通路：MIC→ADC，DAC→喇叭 */
esp_err_t drv_es8388_stop(void);
esp_err_t drv_es8388_deinit(void);
void      drv_es8388_register_console_cmds(void);

/* 业务 API */
esp_err_t drv_es8388_set_volume(uint8_t vol_pct);     /* 0~100 */
esp_err_t drv_es8388_set_mic_gain(uint8_t gain_db);   /* ADC 增益 */
esp_err_t drv_es8388_route_to_speaker(bool speaker);  /* 喇叭 / 耳机切换 */
esp_err_t drv_es8388_mute(bool mute);
```

- 仅做寄存器配置（走 `board_i2c`），**不做任何 I2S 数据操作**，与 `drv_audio` 职责正交。

### 6.4 drv_audio —— I2S 全双工数据通路

```c
esp_err_t drv_audio_init(const drv_audio_cfg_t *cfg);
esp_err_t drv_audio_start(void);
esp_err_t drv_audio_stop(void);
esp_err_t drv_audio_deinit(void);
void      drv_audio_register_console_cmds(void);

/* 业务 API（阻塞式帧读写，调度由 svc_audio 负责） */
esp_err_t drv_audio_read(int16_t *buf, size_t samples, uint32_t timeout_ms);
esp_err_t drv_audio_write(const int16_t *buf, size_t samples, uint32_t timeout_ms);
void      drv_audio_set_input_gain(float gain);   /* 数字增益（软件） */

/* 配置结构 */
typedef struct {
    uint32_t sample_rate;    /* 统一 16000 */
    uint8_t  channels;       /* 1 = mono */
    uint8_t  dma_frames;     /* DMA 缓冲块数 */
} drv_audio_cfg_t;
```

- 音频规格全局统一：**16kHz / 16bit / 单声道 PCM**（ASR 输入、TTS 输出、esp-sr AFE 全部同规格，避免重采样）。
- DMA 缓冲申请在内部 RAM（DMA 限制），业务大缓冲一律 PSRAM。

### 6.5 svc_audio —— 音频前端与录放调度

```c
esp_err_t svc_audio_init(void);
esp_err_t svc_audio_start(void);       /* 启动唤醒词监听（内部跑 AFE） */
esp_err_t svc_audio_stop(void);
esp_err_t svc_audio_deinit(void);
void      svc_audio_register_console_cmds(void);

/* 业务 API（由状态机调用） */
esp_err_t svc_audio_start_recording(void);      /* 进入录音，VAD 断句后停 */
esp_err_t svc_audio_stop_recording(void);
const int16_t *svc_audio_get_record_data(size_t *samples); /* 取录音数据(PSRAM) */
esp_err_t svc_audio_play_pcm(const int16_t *pcm, size_t samples);  /* 流式播放 */
esp_err_t svc_audio_play_file(const char *path_or_url);            /* P1: wav 文件/内存 */
esp_err_t svc_audio_stop_play(void);            /* 打断播放 */
void      svc_audio_play_tone(tone_id_t id);    /* 提示音：唤醒/开始说/结束/错误 */
```

内部结构：

```
┌──────────────────────── svc_audio ────────────────────────┐
│  录音调度(P1): I2S read → 能量VAD断句 → 录音环形缓冲       │
│                (PSRAM 512KB ≈ 16s)                        │
│  播放调度: 播放缓冲(PSRAM 256KB) → I2S write → ES8388      │
│  提示音模块: 内置正弦/和弦 PCM 常量，无需文件系统          │
│  ────────────────────────────────────────────────         │
│  [P2 预留] 唤醒检测: I2S read → AFE → WakeNet；            │
│            AEC 回声消除参考信号接入                       │
└───────────────────────────────────────────────────────────┘
```

- **P1 形态（第一版实现）**：不引入 esp-sr。按键触发录音，能量 VAD（阈值 + 静音超时断句）判断说完。
- P2 引入 `esp-sr`（AFE + WakeNet）：`idf_component.yml` 声明 `espressif/esp-sr`，唤醒词模型经 srmodels 烧录进 `model` 分区，事件 `AUDIO_WAKE_WORD_DETECTED` 届时启用。
- 播放打断：`stop_play()` 立即清空 DMA 与播放缓冲并静音功放，防止残留音。

### 6.6 svc_ai_chat —— DashScope 客户端

```c
esp_err_t svc_ai_chat_init(void);
esp_err_t svc_ai_chat_start(void);
esp_err_t svc_ai_chat_stop(void);
esp_err_t svc_ai_chat_deinit(void);
void      svc_ai_chat_register_console_cmds(void);

/* 业务 API（由状态机调用；阻塞式，完成后发 CHAT_EVENT） */
esp_err_t svc_ai_chat_recognize(const int16_t *pcm, size_t samples);  /* ASR → CHAT_ASR_RESULT */
esp_err_t svc_ai_chat_ask(const char *user_text);                     /* LLM+TTS → CHAT_TTS_READY */
esp_err_t svc_ai_chat_set_api_key(const char *key);                   /* 存 NVS */
esp_err_t svc_ai_chat_reset_context(void);                            /* 清空多轮上下文 */
```

内部三级流水线（P1，HTTP 串行）：

| 步骤 | 接口 | 说明 |
|---|---|---|
| ① ASR | DashScope 语音识别（Paraformer/Gummy 系列） | WAV(16k/mono/16bit) 上行 → 返回文本。**接口以官方文档为准** |
| ② LLM | `POST https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions` | OpenAI 兼容模式，模型名 Kconfig 配置（默认 qwen-turbo 级别，可换）；携带多轮 messages |
| ③ TTS | DashScope CosyVoice 合成 | 回复文本 → 16kHz WAV/PCM 流，音色 Kconfig 配置。**接口以官方文档为准** |

设计要点：

- **API Key 存 NVS**，不硬编码；console 命令 `chat setkey <key>` 写入，`chat status` 查看。
- **TLS 证书**：使用 `esp_crt_bundle`（内置主流 CA 根证书），通过 Kconfig 开关 `CONFIG_SVC_AI_CHAT_CRT_BUNDLE` 控制；同时支持 `menuconfig` 指定自定义 PEM，便于环境切换。
- **单任务串行请求**（`chat_worker` 任务）：ASR→LLM→TTS 顺序执行，同一时刻最多一条 TLS 连接，节省内存，也天然符合状态机串行流程。
- LLM 回复文本通过 `CHAT_LLM_REPLY` 事件上抛（状态机可接 SPI 屏/日志显示）；TTS 音频直接送 `svc_audio_play_*`。
- P3 演进：`svc_ai_chat` 内部替换为 Qwen-Omni Realtime WebSocket 双向流，对外 API 与事件不变。

### 6.7 main/conversation —— 对话状态机（App 层）

```c
/* main/conversation.h（main 组件内部模块） */
void conversation_init(void);    /* 订阅 AUDIO_EVENT / CHAT_EVENT / WIFI 事件 / KEY 事件 */
void conversation_reset(void);   /* 清理会话回到 STANDBY */
chat_state_t conversation_get_state(void);
```

- 纯事件驱动：esp_event handler 内做状态转移，**只调 Service 层 API**，不阻塞、不碰驱动。
- 每次状态变化发布 `CHAT_STATE_CHANGED`（供 LED/屏显示状态）。

---

## 7. 任务与内存规划

### 7.1 任务列表（新增）

| 任务 | 归属 | 栈大小 | 优先级 | 职责 |
|---|---|---|---|---|
| `audio_feed` | svc_audio | 4 KB | 5（高） | I2S 采集 → 实时性最 / VAD高） |
| `sr_detect` | esp-sr 自建 | 6 KB | 4 | WakeNet 推理（****P才2 **才引入**） |
| `audio_play` | svc_audio | 4 KB | 5 | 播放缓冲 → I2S 写 |
| `chat_worker` | svc_ai_chat | 8 KB | 3 | TLS 请求串行流水线（mbedTLS 栈消耗大） |
| `key_scan` | drv_xl9555 | 2 KB | 2 | 按键扫描消抖 |

现有任务（console、wifi、pan_tilt、evt_log、diag、svc_status）不变。状态机为 esp_event handler，不占独立任务。

### 7.2 内存预算（关键约束：内部 RAM 仅 512KB）

| 用途 | 位置 | 大小（预估） |
|---|---|---|
| AFE 工作区（AEC/VAD/降噪AM | ~200 KB（P2 引入后（P2）引入后）  |
| WakeNet 模型运行区M | ~40 KB |
| 录音环形缓冲（16s 上限） | PSRAM | 512 KB |
| 播放环形缓冲 | PSRAM | 256 KB |
| I2S DMA 缓冲 | 内部 RAM | ~12 KB |
| mbedTLS 连接（1 条） | 内部 RAM | ~45 KB |
| LLM 响应解析缓冲 | PSRAM | 32 KB |
| 各任务栈 | 内部 RAM | ~24 KB |

策略与红线：

- **所有 >4KB 的缓冲一律 PSRAM**（`heap_caps_malloc(MALLOC_CAP_SPIRAM)`）；`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 已保证小对象优先内部 RAM。
- 目标红线：对话全程空闲内部堆 **≥ 80 KB**；`diag` 组件每 10s 打印水位，低于 60KB 告警。
- P3 Realtime WebSocket 双流并行时内存压力最大，需重测预算。

### 7.3 Flash 分区（`partitions.csv`）

**P1（第一版）：不改分区表**。当前 factory 2MB 足够容纳 App（TLS + 音频组件约 1.2~1.6MB）。

P2 引入 esp-sr 时再调整（届时注意：**改分区表会擦除 NVS，需重新写入 API Key**）：

```
# Name,     Type, SubType, Offset,   Size
nvs,        data, nvs,     0x9000,   0x6000,
phy_init,   data, phy,     0xf000,   0x1000,
factory,    app,  factory, 0x10000,  0x400000,   # 2MB → 4MB
model,      data, spiffs,  ,         0x500000,   # 新增：esp-sr 模型 (~1.5MB 实际占用)
storage,    data, nvs,     ,         0xAFF000,   # 保留
```

> P2 时 `srmodels.bin` 通过 `esp-sr` 构建后用 `parttool.py` 烧录到 model 分区。

---

## 8. Kconfig 汇总（新增）

```text
DRV_XL9555_ENABLE      bool  "IO 扩展芯片驱动"            default y
DRV_ES8388_ENABLE      bool  "ES8388 音频 Codec 驱动"      default y
DRV_AUDIO_ENABLE       bool  "I2S 全双工音频驱动"          default y
SVC_AUDIO_ENABLE       bool  "语音前端服务(唤醒/VAD/录放)" default y
SVC_AI_CHAT_ENABLE     bool  "云端对话服务(DashScope)"     default y

SVC_AI_CHAT_LLM_MODEL      string "LLM 模型名"       default "qwen-turbo"
SVC_AI_CHAT_TTS_VOICE      string "TTS 音色"         default "longxiaochun"
SVC_AI_CHAT_SYSTEM_PROMPT  string "机器人人设 prompt"
SVC_AI_CHAT_CRT_BUNDLE     bool   "使用内置证书包"    default y
```

裁剪验证：所有新开关全关时，工程行为与当前骨架完全一致（编译期裁剪，启动正常）。

---

## 9. 控制台命令（新增）

| 命令 | 说明 |
|---|---|
| `audio rec 3` | 录 3 秒并回放（硬件自测） |
| `audio vol 60` | 设置音量 |
| `audio tone` | 播放提示音 |
| `chat setkey <key>` | 写入 DashScope API Key（NVS） |
| `chat status` | 状态机状态、上下文轮数、内存水位 |
| `chat ask <text>` | 跳过语音直接文本对话（调试 LLM/TTS 链路） |
| `chat reset` | 清空会话上下文 |

---

## 10. 实施计划

| 步骤 | 内容 | 验收标准 |
|---|---|---|
| S1 | `board_i2c` + `drv_xl9555` | 串口能读到 KEY0~3 状态、控制蜂鸣器 |
| S2 | `drv_es8388` + `drv_audio` | `audio rec 3` 录音回放清晰 |
| S3（P2） | `svc_audio` P1 形态（能量 VAD + 录放 + 提示音） | 按键触发录一段话进缓冲 |
| S4（P3） | `svc_ai_chat` HTTP 流水线 + `conversation` 状态机 | **端到端**：按键→说话→机器人开口回答（≤4s） |
| S5 | ，P3 后引入 esp-sr：WakeNet 唤醒词 + AFE（AEC/VAD） | 喊唤醒词即对话；播放时可语音打断 |
| S6 | `svc_ai_chat` 切 Realtime WebSocket | 端到端延迟 ≤1s，流式响应 |
**第一版实施范围 = S1 ~ S4**。| S7（可选） | SPI 屏表情/状态显示（`svc_chat_ui`） | 屏幕显示状态与字幕 |

每步保持工程可编译、可裁剪，遵循架构规范五段式生命周期。

---

## 11. 风险与注意事项

| 风险 | 影响 | 对策 |
|---|---|---|
| 内部 RAM 紧张（TLS + AFE + DMA 同存） | 分配失败、崩溃 | 缓冲全 PSRAM；单条 TLS 连接；diag 水位监控；必要时降 AFE 档位 |
| AEC 回声消除效果不佳，播放时误唤醒 | 自说自话循环 | 播放期间锁唤醒（仅 KEY 可打断）；P2 用 AFE AEC 参考信号调优 |
| DashScope 接口形态/端点随版本变化 | 联调受阻 | 协议层隔离在 `svc_ai_chat/src/` 内，接口以官方文档为准，API 版本进 Kconfig |
| I2C 共享总线竞争 | codec 配置失败 | board_i2c 事务级互斥；codec 配置仅在初始化/调音量时发生 |
| RGB 屏误启用烧屏/冲突 | 显示异常 | `drv_display`(RGB) 与 `CONFIG_SVC_AUDIO_ENABLE` 互斥编译告警 |
| 供电不足（WiFi+功放峰值） | 蓝屏重启 | 双 Type-C 供电；功放播放时降低 WiFi 省电模式 |
| 控制台口配置 | 下载/调试不通 | 当前 `sdkconfig` 用 USB-Serial-JTAG；本板调试主口是 CH340(UART0)，联调时按实际接线选择控制台 |

---

## 12. 里程碑验收（第一版 P1）

1. 上电 → WiFi 连接 → 喇叭"叮"一声进入待机（LED 心跳）
2. 按 KEY0 → LED 变色 → 说一句话 → 停顿（VAD 断句）后 ≤4s 开始回答
3. 播放中按 KEY1 可打断
4. 多轮对话上下文连贯；5 分钟无交互自动重置
5. 断网自动提示并进入 NET_WAIT，恢复后自动回 STANDBY
6. `free`/`diag` 全程内部堆 ≥ 80KB，连续对话 30 分钟无重启

> P2/P3 验收标准见第 10 章实施计划中对应步骤。
