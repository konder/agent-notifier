#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "secrets.h"
#include "config.h"
#include "types.h"
#include "render.h"
#include "power.h"
#include "ota.h"
#include "buzzer.h"

RTC_DATA_ATTR uint32_t g_bootCount = 0;

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

static Snapshot g_snap;
static volatile bool g_stateReceived = false;
static uint32_t g_lastContentHash = 0;
static uint32_t g_renderCount = 0;

// 视图:状态板 / 通知
enum View { V_BOARD, V_NOTIFY };
static View g_view = V_BOARD;
static uint32_t g_notifyUntil = 0;

// 待处理事件
static volatile bool g_haveEvent = false;
static String g_evKind, g_evSrc, g_evProject, g_evMsg, g_evMeta;
static long g_evTs = 0;

static volatile bool g_doOta = false;
static bool g_usb = false;
static uint32_t g_lastInteract = 0;

// ---------- JSON 解析 ----------
static void parseQuota(JsonObjectConst o, Quota& q) {
    if (o.isNull()) { q.valid = false; return; }
    q.valid = true;
    q.real = o["real"] | false;
    q.h5 = o["h5"].isNull() ? -1 : (float)(o["h5"] | -1.0);
    q.week = o["week"].isNull() ? -1 : (float)(o["week"] | -1.0);
    q.h5_tokens = o["h5_tokens"].isNull() ? -1 : (long)(o["h5_tokens"] | -1);
    q.week_tokens = o["week_tokens"].isNull() ? -1 : (long)(o["week_tokens"] | -1);
    q.h5_reset = o["h5_reset"] | 0;
    q.week_reset = o["week_reset"] | 0;
    q.plan = (const char*)(o["plan"] | "");
}

static void parseSnapshot(const char* payload, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;
    g_snap.ts = doc["ts"] | 0;
    g_snap.hhmm = (const char*)(doc["hhmm"] | "");
    parseQuota(doc["quota"]["codex"].as<JsonObjectConst>(), g_snap.codex);
    parseQuota(doc["quota"]["claude"].as<JsonObjectConst>(), g_snap.claude);
    g_snap.nSessions = 0;
    for (JsonObjectConst s : doc["sessions"].as<JsonArrayConst>()) {
        if (g_snap.nSessions >= MAX_SESSIONS_UI) break;
        Session& d = g_snap.sessions[g_snap.nSessions++];
        d.src = (const char*)(s["src"] | "");
        d.project = (const char*)(s["project"] | "?");
        d.state = (const char*)(s["state"] | "idle");
        d.task = (const char*)(s["task"] | "");
        d.last_msg = (const char*)(s["last_msg"] | "");
        d.model = (const char*)(s["model"] | "");
        d.elapsed_s = s["elapsed_s"].isNull() ? -1 : (long)(s["elapsed_s"] | -1);
        d.tokens = s["tokens"].isNull() ? -1 : (long)(s["tokens"] | -1);
        d.last_activity = s["last_activity"] | 0;
        d.nImages = 0;
    }
    g_stateReceived = true;
}

static void parseEvent(const char* payload, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;
    g_evKind = (const char*)(doc["kind"] | "done");
    g_evSrc = (const char*)(doc["src"] | "");
    g_evProject = (const char*)(doc["project"] | "?");
    g_evMsg = (const char*)(doc["msg"] | "");
    g_evMeta = (const char*)(doc["meta"] | "");
    g_evTs = doc["ts"] | 0;
    g_haveEvent = true;
}

static uint32_t contentHash() {
    uint32_t h = 2166136261u;
    auto mix = [&](const String& s) { for (size_t j = 0; j < s.length(); j++) { h ^= (uint8_t)s[j]; h *= 16777619u; } };
    auto mixi = [&](long v) { for (int b = 0; b < 4; b++) { h ^= (uint8_t)(v >> (b * 8)); h *= 16777619u; } };
    mixi((long)(g_snap.codex.h5 * 10)); mixi((long)(g_snap.codex.week * 10));
    mixi((long)(g_snap.claude.h5 * 10)); mixi((long)(g_snap.claude.week * 10));
    mixi(g_snap.nSessions);
    for (int i = 0; i < g_snap.nSessions; i++) {
        const Session& s = g_snap.sessions[i];
        mix(s.src); mix(s.project); mix(s.state); mix(s.last_msg);
    }
    return h;
}

static uint32_t g_lastBoardMs = 0;
static void showBoard(bool full) {
    g_view = V_BOARD;
    g_lastBoardMs = millis();
    renderList(g_snap, batteryPercent(), g_usb, full);
}

// "HH:MM" + 分钟 → "HH:MM"(设备无 RTC,用最近快照时间近似推算下次唤醒)
static String addMinutes(const String& hhmm, int mins) {
    int c = hhmm.indexOf(':');
    if (c < 1) return String();
    int total = hhmm.substring(0, c).toInt() * 60 + hhmm.substring(c + 1).toInt() + mins;
    total %= (24 * 60); if (total < 0) total += 24 * 60;
    char b[6]; snprintf(b, sizeof(b), "%02d:%02d", total / 60, total % 60);
    return String(b);
}

static void enterSleep() {
    String wake = addMinutes(g_snap.hhmm, SLEEP_INTERVAL_SEC / 60);
    renderSleep(batteryPercent(), wake);   // 睡前渲染电量页(墨水屏保留)
    if (mqtt.connected()) {
        mqtt.publish("m5paper/debug", (String("sleep bat=") + batteryPercent() + " usb=" + g_usb).c_str());
        mqtt.loop(); delay(250);
    }
    deepSleepFor(SLEEP_INTERVAL_SEC);
}

static void showNotify() {
    g_view = V_NOTIFY;
    g_notifyUntil = millis() + NOTIFY_MS;
    g_lastInteract = millis();
    buzzPattern(g_evKind.c_str());
    renderNotify(g_evKind, g_evSrc, g_evProject, g_evMsg, g_evMeta, g_evTs);
}

// ---------- 网络 ----------
static void onMessage(char* topic, byte* payload, unsigned int len) {
    if (!strcmp(topic, TOPIC_STATE)) parseSnapshot((const char*)payload, len);
    else if (!strcmp(topic, TOPIC_EVENT)) parseEvent((const char*)payload, len);
    else if (!strcmp(topic, TOPIC_CMD)) {
        String c((const char*)payload, len);
        if (c.indexOf("ota") >= 0) g_doOta = true;
    }
}
static bool connectMqtt() {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(MQTT_BUFFER_SIZE);
    mqtt.setKeepAlive(MQTT_KEEPALIVE_SEC);
    mqtt.setCallback(onMessage);
    String cid = "m5papers3-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    // 持久会话(cleanSession=false):睡眠离线期间 broker 为我们排队 QoS1 事件,醒来补发,不丢通知
    const char* user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
    const char* pass = strlen(MQTT_PASS) ? MQTT_PASS : nullptr;
    bool ok = mqtt.connect(cid.c_str(), user, pass, nullptr, 0, false, nullptr, false);
    if (ok) {
        mqtt.subscribe(TOPIC_STATE, 0);   // 状态 QoS0(retained 给最新)
        mqtt.subscribe(TOPIC_EVENT, 1);   // 事件 QoS1(离线排队)
        mqtt.subscribe(TOPIC_CMD, 1);
    }
    return ok;
}
static bool waitForState(uint32_t ms) {
    uint32_t t0 = millis();
    while (!g_stateReceived && millis() - t0 < ms) { mqtt.loop(); delay(30); }
    return g_stateReceived;
}

// ---------- 主流程 ----------
void setup() {
    auto cfg = M5.config();
    cfg.clear_display = false;   // 别在 M5.begin 时清屏(否则唤醒抹掉休眠页 → 白屏)
    M5.begin(cfg);
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    g_bootCount++;
    analogReadResolution(12);
    buzzerInit();
    renderInit(!wokeFromDeepSleep());   // 深睡唤醒不清屏,保留休眠页直到状态板画出

    g_usb = isUsbPowered();
    g_lastInteract = millis();
    buzzPattern("boot");   // 开机测试音:确认蜂鸣器可用

    // 深睡唤醒时不覆盖休眠页(快连很快,连上直接画板);仅冷启动显示"连接中"
    if (!wokeFromDeepSleep()) renderStatus("连接 WiFi...");
    if (!wifiConnectNVS()) { renderStatus("WiFi 失败"); if (!g_usb) enterSleep(); return; }
    Serial.print("[wifi] IP="); Serial.println(WiFi.localIP());
    checkOTA();
    if (!connectMqtt()) { renderStatus("MQTT 失败"); if (!g_usb) enterSleep(); return; }

    waitForState(MQTT_WAIT_STATE_MS);
    mqtt.publish("m5paper/debug", (String("boot v") + FW_VERSION + " usb=" + g_usb
                 + " st=" + g_stateReceived + " boot#" + g_bootCount).c_str());
    if (g_stateReceived) { g_lastContentHash = contentHash(); showBoard((g_bootCount % FULL_REFRESH_EVERY) == 1); }
    else renderStatus("等待数据超时");
}

void loop() {
    if (!mqtt.connected()) { if (!connectMqtt()) { delay(2000); return; } }
    mqtt.loop();

    // 远程 OTA 指令
    if (g_doOta) { g_doOta = false; checkOTA(); }

    // 事件 → 蜂鸣 + 通知屏
    if (g_haveEvent) {
        g_haveEvent = false;
        showNotify();
    }

    // 通知停留结束 → 回状态板
    if (g_view == V_NOTIFY && millis() > g_notifyUntil) {
        g_lastContentHash = contentHash();
        showBoard(false);
    }

    // 状态板:内容变化才刷
    if (g_view == V_BOARD && g_stateReceived) {
        g_stateReceived = false;
        uint32_t h = contentHash();
        if (h != g_lastContentHash) { g_lastContentHash = h; showBoard((g_renderCount++ % FULL_REFRESH_EVERY) == 0); }
    }

    // 插电定期重刷板(更新右上角时间/电量,即使内容没变)
    if (g_usb && g_view == V_BOARD && millis() - g_lastBoardMs > 120000) {
        showBoard(false);
    }

    // 插电定期查 OTA
    static uint32_t lastOta = 0;
    if (g_usb && millis() - lastOta > OTA_CHECK_MS) { lastOta = millis(); checkOTA(); }

    // 电池:空闲深睡(通知端建议插电常连以即时提醒)
    if (!g_usb && g_view == V_BOARD && millis() - g_lastInteract > IDLE_SLEEP_MS) {
        enterSleep();
    }
    delay(20);
}
