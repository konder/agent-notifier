#pragma once
// 全局常量:BLE、引脚、OTA、刷新策略
// v23 起:运行时纯 BLE(外设),WiFi 仅在收到 BLE "ota" 指令时临时开做 OTA。

// 固件版本(每次要 OTA 推新时 +1;gateway 的 /fw/version 返回值 > 此值即触发更新)
#define FW_VERSION 25

// ---- BLE(Nordic UART Service,飞牛 NAS 当中心写通知)----
#define BLE_NAME     "M5PaperNotify"
#define NUS_SVC      "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // 中心→外设 写(事件/指令)
#define NUS_TX       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // 外设→中心 notify(电量遥测)
#define BLE_MSG_MAX  4096              // 单条重组消息上限(分帧累加,防溢出)
#define HISTORY_MAX  8                 // 待命屏历史事件列表最多显示/保留条数

// ---- 省电:电池模式自动轻睡眠(保持 BLE 连接,CPU 在连接事件间隙睡)----
// BLE 连接参数(1.25ms 单位):拉长间隔 + 从机延迟 → 空闲时约 1s 才醒一次,省电;有事件立即醒。
#define BLE_CONN_MIN_ITVL  24          // 30ms
#define BLE_CONN_MAX_ITVL  160         // 200ms
#define BLE_CONN_LATENCY   4           // 空闲可跳过 4 个事件 → 有效 ~1s
#define BLE_CONN_TIMEOUT   600         // 6s 监督超时
#define PM_MIN_FREQ_MHZ    40          // 轻睡眠时降频到此
#define PM_MAX_FREQ_MHZ    240

// ---- 电量遥测 + 低电告警 ----
#define BAT_REPORT_MS  300000          // 每 5 分钟经 BLE 上报一次电量(供实测续航)
#define LOW_BATT_PCT   15              // 低于此且非 USB → 弹低电告警 + 蜂鸣(每次下探只报一次)

#define PIN_BUZZER   21                // G21 BUZ_PWM 无源蜂鸣器
#define NOTIFY_MS    30000             // 通知全屏卡停留时长(毫秒),之后回待命屏(历史列表)

// OTA 固件服务(留在 Mac Mini,设备走家里 WiFi 可达;与 gateway config.toml [thumb].port 一致)
#define THUMB_PORT   8899

// PaperS3 引脚(见官方 PinMap)
#define PIN_USB_DET  5     // G5:USB 供电检测,>0.2V 视为插电
#define PIN_TOUCH_INT 48   // G48:GT911 触摸中断,做深睡唤醒源

// 触摸/视图
#define IDLE_SLEEP_MS 6000   // 电池模式:醒来处理完(收事件+画板)多久回睡;越短越省电

// OTA:插电常开模式下,每隔这么久再查一次新固件(毫秒);电池模式每次唤醒都会查
#define OTA_CHECK_MS 1800000  // 30 分钟

// 电池模式:深睡唤醒间隔(秒)。越大越省电,完成提醒延迟越大。
#define SLEEP_INTERVAL_SEC   300
// 插电模式:保持长连接,MQTT keepalive(秒)
#define MQTT_KEEPALIVE_SEC   30

// MQTT 快照较大,需要放大 PubSubClient 缓冲(默认仅 256B)
#define MQTT_BUFFER_SIZE     8192

// 全刷去残影:每累计这么多次局刷后做一次全刷
#define FULL_REFRESH_EVERY   10

// WiFi/MQTT 连接超时(毫秒)
#define WIFI_TIMEOUT_MS      15000
#define MQTT_WAIT_STATE_MS   6000   // 唤醒后等 retained 快照到达的最长时间
