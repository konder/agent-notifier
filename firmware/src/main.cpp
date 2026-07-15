// M5PaperS3 —— WiFi + MQTT 事件通知端(v26)
// 连家里 WiFi + MQTT broker,PM 自动轻睡眠(WiFi modem sleep)→ 低功耗、常连、通知即时;
// WiFi 走路由器全屋覆盖,不受 BLE ~10m 限制,设备可放书桌。
// 平时显示「待命屏 = 最近事件历史列表」;收到事件→蜂鸣+全屏通知卡,NOTIFY_MS 后回列表。
// cmd=ota → 直接 checkOTA(WiFi 常开)。电量经 MQTT 上报(m5paper/device)供实测续航。
#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_pm.h>
#include <esp_wifi.h>

#include "secrets.h"
#include "config.h"
#include "types.h"
#include "render.h"
#include "power.h"
#include "ota.h"
#include "buzzer.h"

RTC_DATA_ATTR uint32_t g_bootCount = 0;
static bool g_usb = true;

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

enum View { V_IDLE, V_NOTIFY };
static View g_view = V_IDLE;
static uint32_t g_notifyUntil = 0;

static EventItem g_hist[HISTORY_MAX];
static int g_histN = 0;
static bool g_idleDirty = true;
static uint32_t g_idleRenders = 0;

// 待处理事件(onMessage 里置位,loop 里处理)
static volatile bool g_haveEvent = false;
static String g_evKind, g_evSrc, g_evProject, g_evMsg, g_evMeta;
static long g_evTs = 0;
static volatile bool g_doOta = false;

static String summarize(const String& msg) {
    String s = msg; int nl = s.indexOf('\n');
    if (nl >= 0) s = s.substring(0, nl);
    s.trim();
    if (s.length() > 160) s = s.substring(0, 160);
    return s;
}

static void addHistory(const String& kind, const String& src, const String& project,
                       const String& msg, long ts) {
    for (int i = HISTORY_MAX - 1; i > 0; i--) g_hist[i] = g_hist[i - 1];
    g_hist[0].kind = kind; g_hist[0].src = src; g_hist[0].project = project;
    g_hist[0].summary = summarize(msg); g_hist[0].ts = ts;
    if (g_histN < HISTORY_MAX) g_histN++;
    g_idleDirty = true;
}

static void showIdle() {
    g_view = V_IDLE;
    renderIdle(g_hist, g_histN, batteryPercent(), mqtt.connected(),
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

// 电池模式开自动轻睡眠 + WiFi modem sleep(保持连接、CPU 空闲即睡);插电全速。
static void configurePowerSave() {
    WiFi.setSleep(g_usb ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_pm_config_t pm = {};
#else
    esp_pm_config_esp32s3_t pm = {};
#endif
    pm.max_freq_mhz = PM_MAX_FREQ_MHZ;
    pm.min_freq_mhz = g_usb ? PM_MAX_FREQ_MHZ : PM_MIN_FREQ_MHZ;
    pm.light_sleep_enable = g_usb ? false : true;
    esp_err_t e = esp_pm_configure(&pm);
    Serial.printf("[pm] light_sleep=%d min=%d -> %s\n", pm.light_sleep_enable, pm.min_freq_mhz, esp_err_to_name(e));
}

static void sendBattery() {
    if (!mqtt.connected()) return;
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"pct\":%d,\"up\":%lu,\"usb\":%d,\"v\":%d}",
             batteryPercent(), (unsigned long)(millis() / 1000), g_usb ? 1 : 0, FW_VERSION);
    mqtt.publish(TOPIC_DEVICE, buf);
    Serial.printf("[bat] %s\n", buf);
}

static void checkLowBatt() {
    static bool alerted = false;
    int p = batteryPercent();
    if (!g_usb && p >= 0 && p < LOW_BATT_PCT) {
        if (!alerted) {
            alerted = true;
            showNotify("quota", "", "电量不足", String("电量 ") + p + "% ,请尽快充电。", "", millis() / 1000);
        }
    } else if (g_usb || p >= LOW_BATT_PCT + 5) {
        alerted = false;
    }
}

// ---------- MQTT ----------
static void onMessage(char* topic, byte* payload, unsigned int len) {
    if (!strcmp(topic, TOPIC_CMD)) {
        String c((const char*)payload, len);
        if (c.indexOf("ota") >= 0) g_doOta = true;
        return;
    }
    if (strcmp(topic, TOPIC_EVENT)) return;
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

static bool connectMqtt() {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(MQTT_BUFFER_SIZE);
    mqtt.setKeepAlive(MQTT_KEEPALIVE_SEC);
    mqtt.setCallback(onMessage);
    String cid = "m5papers3-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    const char* user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
    const char* pass = strlen(MQTT_PASS) ? MQTT_PASS : nullptr;
    // 持久会话(cleanSession=false):短暂掉线期间 broker 排队 QoS1 事件,重连补发
    bool ok = mqtt.connect(cid.c_str(), user, pass, nullptr, 0, false, nullptr, false);
    if (ok) {
        mqtt.subscribe(TOPIC_EVENT, 1);
        mqtt.subscribe(TOPIC_CMD, 1);
        Serial.println("[mqtt] connected + subscribed");
    }
    return ok;
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
    buzzPattern("boot");
    renderStatus("连接 WiFi...");
    if (!wifiConnectNVS()) { renderStatus("WiFi 失败,重试中"); }
    else Serial.print("[wifi] IP="), Serial.println(WiFi.localIP());
    configurePowerSave();
    checkOTA();                 // 开机查一次固件更新
    connectMqtt();
    Serial.printf("[boot] v%d WiFi+PM usb=%d boot#%u\n", FW_VERSION, g_usb, g_bootCount);
    showIdle();
}

void loop() {
    // WiFi 断线重连
    if (WiFi.status() != WL_CONNECTED) {
        renderStatus("重连 WiFi...");
        wifiConnectNVS();
    }
    // MQTT 断线重连(节流)
    if (!mqtt.connected()) {
        static uint32_t lastTry = 0;
        if (millis() - lastTry > 3000) { lastTry = millis(); connectMqtt(); }
    }
    mqtt.loop();

    if (g_doOta) { g_doOta = false; checkOTA(); }

    if (g_haveEvent) {
        g_haveEvent = false;
        addHistory(g_evKind, g_evSrc, g_evProject, g_evMsg, g_evTs);
        showNotify(g_evKind, g_evSrc, g_evProject, g_evMsg, g_evMeta, g_evTs);
    }

    uint32_t now = millis();
    if (g_view == V_NOTIFY && (int32_t)(now - g_notifyUntil) >= 0) showIdle();
    else if (g_view == V_IDLE && g_idleDirty) showIdle();

    // 插电定期重刷(更新时间/电量)
    static uint32_t lastBoard = 0;
    if (g_usb && g_view == V_IDLE && now - lastBoard > 120000) { lastBoard = now; showIdle(); }

    // 周期:电量遥测 + 低电告警
    static uint32_t lastBat = 0;
    if (now - lastBat > BAT_REPORT_MS || lastBat == 0) { lastBat = now; sendBattery(); checkLowBatt(); }

    // 空闲步进:电池模式睡长一点(轻睡眠),插电全速跟手
    delay(g_usb ? 20 : LOOP_IDLE_MS);
}
