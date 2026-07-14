#pragma once
#include <Arduino.h>

// NimBLE 外设(NUS):开机初始化并开始广播为 BLE_NAME。
void blelinkInit();

// 停止并释放 BLE(OTA 前调用,把 2.4G 射频让给 WiFi)。
void blelinkStop();

// 当前是否有中心(飞牛 NAS)连着。
bool bleConnected();

// 经 TX 特征 notify 一段文本给中心(如电量遥测 JSON)。未连接时静默丢弃。
void bleNotify(const String& s);

// 阻塞至多 waitMs 取一条完整消息;轻睡眠下 CPU 在此休眠,来消息立即唤醒(近即时)。
bool blePopMessageWait(String& out, uint32_t waitMs);

// 非阻塞取一条已重组完成的消息(中心分帧写入、以 '\n' 结尾)。有则填入 out 返回 true。
bool blePopMessage(String& out);
