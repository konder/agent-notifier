// BLE PoC —— 照搬能用版配置 + M5.begin(电源保持) + 只加 NimBLE(不画屏),隔离崩因。
#include <M5Unified.h>
#include <NimBLEDevice.h>

#define NUS_SVC "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define PIN_BUZZER 21
#define BUZ_CH 6

static volatile bool g_connected = false;
static volatile bool g_dirty = false;
static String g_msg;

static void beep(int f, int ms) { ledcWriteTone(BUZ_CH, f); delay(ms); ledcWriteTone(BUZ_CH, 0); }

class RxCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) { g_msg = String(c->getValue().c_str()); g_dirty = true; }
};
class SrvCB : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*) { g_connected = true; }
    void onDisconnect(NimBLEServer* s) { g_connected = false; s->startAdvertising(); }
};

void setup() {
    auto cfg = M5.config();
    cfg.clear_display = false;
    M5.begin(cfg);                 // 关键:初始化电源(PMS150G 保持供电)
    Serial.begin(115200);
    delay(600);
    Serial.println("\n[boot] BLE PoC (M5.begin + NimBLE) starting");
    ledcSetup(BUZ_CH, 2000, 10); ledcAttachPin(PIN_BUZZER, BUZ_CH);
    beep(1200, 90);

    NimBLEDevice::init("M5PaperNotify");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new SrvCB());
    NimBLEService* svc = server->createService(NUS_SVC);
    NimBLECharacteristic* rx = svc->createCharacteristic(NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new RxCB());
    svc->start();
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SVC);
    adv->setScanResponse(true);
    NimBLEDevice::startAdvertising();
    Serial.println("[ble] advertising as M5PaperNotify");
}

void loop() {
    if (g_dirty) { g_dirty = false; beep(1568, 130); delay(50); beep(2093, 180); Serial.printf("[ble] rx: %s\n", g_msg.c_str()); }
    static uint32_t hb = 0;
    if (millis() - hb > 3000) { hb = millis(); Serial.printf("[hb] adv connected=%d\n", g_connected); }
    delay(20);
}
