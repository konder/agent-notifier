#pragma once
#include <Arduino.h>

// NimBLE 外设(NUS):开机初始化并开始广播为 BLE_NAME。
void blelinkInit();

// 停止并释放 BLE(OTA 前调用,把 2.4G 射频让给 WiFi)。
void blelinkStop();

// 当前是否有中心(飞牛 NAS)连着。
bool bleConnected();

// 取出一条已重组完成的消息(中心分帧写入、以 '\n' 结尾)。有则填入 out 返回 true。
// 在主循环里轮询调用;收发跨任务通过 FreeRTOS 队列安全交接。
bool blePopMessage(String& out);
