#include "blelink.h"
#include "config.h"
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// 收发跨任务:onWrite 在 NimBLE 主机任务里跑,主循环在 loop() 里跑。
// 完整消息(strdup 的 char*)通过 FreeRTOS 队列交给主循环,避免跨任务共享 String。
static volatile bool s_connected = false;
static QueueHandle_t s_queue = nullptr;
static String s_asm;                       // 分帧累加缓冲(仅 NimBLE 任务访问)
static NimBLECharacteristic* s_tx = nullptr;   // 外设→中心 notify(电量遥测)

class RxCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) {
        // 逐字节累加,遇 '\n' 取出一条完整消息入队(见 blelink 分帧协议)
        String chunk = String(c->getValue().c_str());
        for (size_t i = 0; i < chunk.length(); i++) {
            char ch = chunk[i];
            if (ch == '\n') {
                if (s_asm.length()) {
                    char* line = strdup(s_asm.c_str());
                    if (line) { if (!s_queue || xQueueSend(s_queue, &line, 0) != pdTRUE) free(line); }
                    s_asm = "";
                }
            } else if (s_asm.length() < BLE_MSG_MAX) {
                s_asm += ch;
            } else {
                s_asm = "";   // 溢出保护:异常超长帧直接丢弃
            }
        }
    }
};

class SrvCB : public NimBLEServerCallbacks {
    // 带 desc 的重载:拿到 conn_handle 后请求长连接参数(省电,空闲 ~1s 才醒)
    void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) {
        s_connected = true;
        Serial.println("[ble] central connected");
        s->updateConnParams(desc->conn_handle, BLE_CONN_MIN_ITVL, BLE_CONN_MAX_ITVL,
                            BLE_CONN_LATENCY, BLE_CONN_TIMEOUT);
    }
    void onDisconnect(NimBLEServer* s) {
        s_connected = false; s_asm = "";
        Serial.println("[ble] central disconnected, re-advertising");
        s->startAdvertising();
    }
};

void blelinkInit() {
    if (!s_queue) s_queue = xQueueCreate(8, sizeof(char*));  // OTA 中止后重入不重复创建
    s_asm = "";
    NimBLEDevice::init(BLE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);   // 最大发射功率,尽量拉距离
    NimBLEDevice::setMTU(517);                // 允许大 MTU,减少分帧次数
    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new SrvCB());
    NimBLEService* svc = server->createService(NUS_SVC);
    NimBLECharacteristic* rx = svc->createCharacteristic(
        NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new RxCB());
    s_tx = svc->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY);   // 电量遥测回传
    svc->start();
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SVC);
    adv->setScanResponse(true);
    NimBLEDevice::startAdvertising();
    Serial.println("[ble] advertising as " BLE_NAME);
}

void blelinkStop() {
    NimBLEDevice::deinit(true);
    s_connected = false;
    s_tx = nullptr;
}

bool bleConnected() { return s_connected; }

void bleNotify(const String& s) {
    if (s_connected && s_tx) {
        s_tx->setValue((uint8_t*)s.c_str(), s.length());
        s_tx->notify();
    }
}

// 阻塞至多 waitMs 取一条消息:轻睡眠期间 CPU 在此休眠,来消息立即唤醒(近即时)
bool blePopMessageWait(String& out, uint32_t waitMs) {
    if (!s_queue) return false;
    char* line = nullptr;
    if (xQueueReceive(s_queue, &line, pdMS_TO_TICKS(waitMs)) == pdTRUE) {
        out = String(line);
        free(line);
        return true;
    }
    return false;
}

bool blePopMessage(String& out) {
    if (!s_queue) return false;
    char* line = nullptr;
    if (xQueueReceive(s_queue, &line, 0) == pdTRUE) {
        out = String(line);
        free(line);
        return true;
    }
    return false;
}
