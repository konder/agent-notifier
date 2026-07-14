// M5PaperS3 —— 纯 BLE 事件通知端(v23+)
// 运行时纯 BLE(外设,NUS);飞牛 NAS 当中心把事件分帧写进来。
// 平时显示「待命屏 = 最近事件历史列表」;收到 live 事件→蜂鸣+全屏通知卡,NOTIFY_MS 后回列表。
// 收到 BLE "ota" 指令→停 BLE、临时开 WiFi 从 Mac Mini 拉固件(httpUpdate 成功自动重启)、否则关 WiFi 恢复 BLE。
// 省电:电池模式开自动轻睡眠(保持 BLE 连接,CPU 在连接事件间隙 light sleep);插 USB 全速。
// 深睡会断 BLE,所以不用深睡。主循环阻塞在 BLE 消息队列上,来消息即时唤醒。
#include <M5Unified.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_pm.h>

#include "config.h"
#include "types.h"
#include "render.h"
#include "power.h"
#include "ota.h"
#include "buzzer.h"
#include "blelink.h"

RTC_DATA_ATTR uint32_t g_bootCount = 0;
static bool g_usb = true;              // 供电模式(USB=全速灵敏;电池=轻睡眠省电)

// 视图
enum View { V_IDLE, V_NOTIFY };
static View g_view = V_IDLE;
static uint32_t g_notifyUntil = 0;

// 历史事件环形缓冲(最新在 [0])
static EventItem g_hist[HISTORY_MAX];
static int g_histN = 0;
static bool g_idleDirty = true;
static uint32_t g_idleRenders = 0;

static volatile bool g_doOta = false;

// 取正文首行、截断,做列表摘要
static String summarize(const String& msg) {
    String s = msg;
    int nl = s.indexOf('\n');
    if (nl >= 0) s = s.substring(0, nl);
    s.trim();
    if (s.length() > 160) s = s.substring(0, 160);
    return s;
}

static void addHistory(const String& kind, const String& src, const String& project,
                       const String& msg, long ts) {
    for (int i = HISTORY_MAX - 1; i > 0; i--) g_hist[i] = g_hist[i - 1];
    g_hist[0].kind = kind;
    g_hist[0].src = src;
    g_hist[0].project = project;
    g_hist[0].summary = summarize(msg);
    g_hist[0].ts = ts;
    if (g_histN < HISTORY_MAX) g_histN++;
    g_idleDirty = true;
}

static void showIdle() {
    g_view = V_IDLE;
    renderIdle(g_hist, g_histN, batteryPercent(), bleConnected(),
               FW_VERSION, (g_idleRenders++ % FULL_REFRESH_EVERY) == 0);
    g_idleDirty = false;
}

static void showNotify(const String& kind, const String& src, const String& project,
                       const String& msg, const String& meta, long ts) {
    g_view = V_NOTIFY;
    g_notifyUntil = millis() + NOTIFY_MS;
    buzzPattern(kind.c_str());
    renderNotify(kind, src, project, msg, meta, ts);
}

// BLE "ota" 指令:停 BLE → 开 WiFi → 拉固件(成功自动重启)→ 否则关 WiFi 恢复 BLE
static void doOta() {
    renderStatus("发现更新指令,连接 WiFi...");
    blelinkStop();
    if (wifiConnectNVS()) {
        renderStatus("检查/下载固件中...");
        checkOTA();               // 有新版则 httpUpdate 并自动重启;无则返回
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    blelinkInit();                // 恢复 BLE 广播
    g_idleDirty = true;
    showIdle();
}

// 电池模式开自动轻睡眠:CPU 空闲(阻塞在队列/延时)时进 light sleep,BLE 控制器按连接事件唤醒。
// 依赖 SDK 的 CONFIG_PM_ENABLE + tickless idle(arduino-esp32 默认开);不支持则返回错误,记录后照常跑。
static void configurePowerSave() {
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_pm_config_t pm = {};
#else
    esp_pm_config_esp32s3_t pm = {};
#endif
    pm.max_freq_mhz = PM_MAX_FREQ_MHZ;
    pm.min_freq_mhz = g_usb ? PM_MAX_FREQ_MHZ : PM_MIN_FREQ_MHZ;
    pm.light_sleep_enable = g_usb ? false : true;   // 仅电池模式轻睡眠
    esp_err_t e = esp_pm_configure(&pm);
    Serial.printf("[pm] light_sleep=%d min=%d max=%d -> %s\n",
                  pm.light_sleep_enable, pm.min_freq_mhz, pm.max_freq_mhz, esp_err_to_name(e));
}

// 电量遥测:经 BLE 上报给飞牛桥(桥记日志,用于实测续航 + 低电判断)
static void sendBattery() {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"t\":\"bat\",\"pct\":%d,\"up\":%lu,\"usb\":%d,\"v\":%d}",
             batteryPercent(), (unsigned long)(millis() / 1000), g_usb ? 1 : 0, FW_VERSION);
    bleNotify(String(buf));
    Serial.printf("[bat] %s\n", buf);
}

// 低电告警:电池模式下低于阈值,弹屏+蜂鸣,每次下探只报一次(回充或回升后重置)
static void checkLowBatt() {
    static bool alerted = false;
    int p = batteryPercent();
    if (!g_usb && p >= 0 && p < LOW_BATT_PCT) {
        if (!alerted) {
            alerted = true;
            buzzPattern("quota");   // 低沉告警音
            renderNotify("quota", "", "电量不足", String("电量 ") + p + "% ,请尽快充电。", "", millis()/1000);
            g_notifyUntil = millis() + NOTIFY_MS;
            g_view = V_NOTIFY;
        }
    } else if (g_usb || p >= LOW_BATT_PCT + 5) {
        alerted = false;
    }
}

// 处理一条来自 BLE 的完整 JSON 消息
static void handleMessage(const String& js) {
    JsonDocument doc;
    if (deserializeJson(doc, js)) return;
    String t = (const char*)(doc["t"] | "");
    if (t == "cmd") {
        String c = (const char*)(doc["cmd"] | "");
        if (c == "ota") g_doOta = true;
        return;
    }
    if (t != "ev") return;
    bool live = doc["live"] | true;   // 缺省视为实时(replay 补发会显式 live:false)
    String kind = (const char*)(doc["kind"] | "done");
    String src = (const char*)(doc["src"] | "");
    String project = (const char*)(doc["project"] | "?");
    String msg = (const char*)(doc["msg"] | "");
    String meta = (const char*)(doc["meta"] | "");
    long ts = doc["ts"] | 0;

    Serial.printf("[ev] rx kind=%s src=%s project=%s live=%d msglen=%d\n",
                  kind.c_str(), src.c_str(), project.c_str(), live, (int)msg.length());
    addHistory(kind, src, project, msg, ts);
    if (live) {
        showNotify(kind, src, project, msg, meta, ts);   // 蜂鸣 + 全屏通知卡
    }
    // replay(live=false)只进历史;等这批处理完统一重画待命屏
}

void setup() {
    auto cfg = M5.config();
    cfg.clear_display = false;
    M5.begin(cfg);
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    g_bootCount++;
    analogReadResolution(12);
    buzzerInit();
    renderInit(true);

    g_usb = isUsbPowered();
    buzzPattern("boot");          // 开机测试音:确认蜂鸣器
    blelinkInit();                // 纯 BLE:开机即广播,不连 WiFi
    configurePowerSave();         // 电池模式开轻睡眠(保持 BLE)
    Serial.printf("[boot] v%d BLE-only usb=%d boot#%u\n", FW_VERSION, g_usb, g_bootCount);
    showIdle();                   // 待命屏(初始为空,等桥补发历史)
}

void loop() {
    // 阻塞至多 1s 等 BLE 消息:轻睡眠下 CPU 在此休眠,来消息立即醒(≤1s 延迟)
    String js;
    if (blePopMessageWait(js, 1000)) {
        handleMessage(js);
        while (blePopMessage(js)) handleMessage(js);   // 排空这批(如历史补发)
    }

    // OTA 指令(可能来自消息处理)
    if (g_doOta) { g_doOta = false; doOta(); return; }

    uint32_t now = millis();
    // 通知停留结束 → 回待命屏
    if (g_view == V_NOTIFY && (int32_t)(now - g_notifyUntil) >= 0) showIdle();
    // 待命屏内容有更新(如收到 replay 历史)→ 重画
    else if (g_view == V_IDLE && g_idleDirty) showIdle();

    // 周期:电量遥测 + 低电告警
    static uint32_t lastBat = 0;
    if (now - lastBat > BAT_REPORT_MS || lastBat == 0) {
        lastBat = now;
        sendBattery();
        checkLowBatt();
    }
}
