// M5PaperS3 —— 纯 BLE 事件通知端(v23+)
// 运行时纯 BLE(外设,NUS);飞牛 NAS 当中心把事件分帧写进来。
// 平时显示「待命屏 = 最近事件历史列表」;收到 live 事件→蜂鸣+全屏通知卡,NOTIFY_MS 后回列表。
// 收到 BLE "ota" 指令→停 BLE、临时开 WiFi 从 Mac Mini 拉固件(httpUpdate 成功自动重启)、否则关 WiFi 恢复 BLE。
// 不深睡(深睡会断 BLE);BLE 连接态靠 ESP 默认 modem sleep 省电。
#include <M5Unified.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "config.h"
#include "types.h"
#include "render.h"
#include "power.h"
#include "ota.h"
#include "buzzer.h"
#include "blelink.h"

RTC_DATA_ATTR uint32_t g_bootCount = 0;

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

    buzzPattern("boot");          // 开机测试音:确认蜂鸣器
    blelinkInit();                // 纯 BLE:开机即广播,不连 WiFi
    Serial.printf("[boot] v%d BLE-only, boot#%u\n", FW_VERSION, g_bootCount);
    showIdle();                   // 待命屏(初始为空,等桥补发历史)
}

void loop() {
    M5.update();

    // 排空 BLE 收到的完整消息
    bool got = false;
    String js;
    while (blePopMessage(js)) { handleMessage(js); got = true; }

    // OTA 指令(可能来自消息处理)
    if (g_doOta) { g_doOta = false; doOta(); return; }

    // 通知停留结束 → 回待命屏
    if (g_view == V_NOTIFY && (int32_t)(millis() - g_notifyUntil) >= 0) {
        showIdle();
    }
    // 待命屏内容有更新(如收到 replay 历史)→ 重画
    else if (g_view == V_IDLE && g_idleDirty) {
        showIdle();
    }

    (void)got;
    delay(20);
}
