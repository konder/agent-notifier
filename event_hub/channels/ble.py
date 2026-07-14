#!/usr/bin/env python3
"""BLE 广播渠道(飞牛 NAS 当 BLE 中心)—— 目前是已验证通过的 PoC。

PoC:扫到 M5PaperNotify → 连上 → 往 NUS RX 特征写一条通知。
用法: python3 -m event_hub.channels.ble [要发送的文本]
依赖: pip install bleak(Linux 走 BlueZ/dbus,无需编译;macOS 有 TCC 权限坑)

验证结论(见 README「BLE 现状」):飞牛 NAS(Debian/BlueZ,MT7961)能稳定当 BLE 中心,
设备收到通知蜂鸣,RSSI -66 够用;macOS 当中心受 TCC 权限限制不适合常驻。

TODO(BLE 完整改造阶段,老板已定「随后」):
  - 提供与 channels/mqtt.py 对齐的接口:publish_event(ev) / publish_state(snap)
  - 常驻:订阅本机 MQTT(飞牛跑 broker)m5paper/events → 通过 BLE 写给设备
  - 断连自动重连、开机自启(systemd)
  - 设备端:BLE 外设 + 低功耗连接态 + 加回墨水屏显示 + WiFi 按需 OTA
"""
import asyncio
import sys

from bleak import BleakScanner, BleakClient

NUS_RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   # 中心→外设 写


async def main():
    msg = sys.argv[1] if len(sys.argv) > 1 else "测试通知 · payment-platform 任务完成"
    print("扫描 M5PaperNotify ...")
    dev = await BleakScanner.find_device_by_name("M5PaperNotify", timeout=12.0)
    if not dev:
        print("❌ 没找到设备(确认设备已刷 PoC 固件、在广播、且在蓝牙范围内)")
        return
    print(f"✓ 找到 {dev.address},连接中...")
    async with BleakClient(dev) as client:
        await client.write_gatt_char(NUS_RX, msg.encode("utf-8"), response=True)
        print(f"✓ 已发送: {msg}")
    print("完成。设备应已蜂鸣并显示该文本。")


if __name__ == "__main__":
    asyncio.run(main())
