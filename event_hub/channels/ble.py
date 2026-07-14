#!/usr/bin/env python3
"""BLE 广播渠道 —— 飞牛 NAS 当 BLE 中心的常驻桥。

数据流:collector(Mac Mini)──MQTT──▶ 飞牛 mosquitto ──本模块订阅──▶ BLE 分帧写 ──▶ M5PaperS3

职责:
  - 订阅本机 broker 的 m5paper/events(事件)+ m5paper/cmd(如 ota)。
  - 维持到 "M5PaperNotify" 的 BLE 连接(扫描→连→断线自动重连)。
  - 事件 → {"t":"ev","live":true,...} 分帧写 NUS RX(> MTU 自动切片 + '\n' 终止,设备重组)。
  - 维护最近 N 条历史;设备(重)连时以 live:false 补发,重启后列表不空。
  - cmd(ota)→ {"t":"cmd","cmd":"ota"},设备据此临时开 WiFi 做 OTA。

依赖:bleak(BlueZ,Linux 无需编译)+ paho-mqtt。
运行:python3 -m event_hub.channels.ble           # 常驻 daemon(systemd: deploy/ble-bridge.service)
     python3 -m event_hub.channels.ble --test "手动测试通知"   # 一次性发一条(联调用)
"""
from __future__ import annotations

import asyncio
import collections
import json
import os
import sys
import threading

from bleak import BleakScanner, BleakClient

DEVICE_NAME = os.environ.get("M5_BLE_NAME", "M5PaperNotify")
NUS_RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"      # 中心→外设 写
NUS_TX = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"      # 外设→中心 notify(电量遥测)
TELEMETRY_LOG = os.environ.get("M5_TELEMETRY_LOG", "/tmp/m5_telemetry.log")
BROKER_HOST = os.environ.get("M5_BROKER_HOST", "127.0.0.1")
BROKER_PORT = int(os.environ.get("M5_BROKER_PORT", "1883"))
EVENTS_TOPIC = "m5paper/events"
CMD_TOPIC = "m5paper/cmd"
HISTORY_N = int(os.environ.get("M5_HISTORY_N", "8"))
SCAN_TIMEOUT = 12.0
RECONNECT_DELAY = 5.0


def log(*a):
    print("[ble-bridge]", *a, file=sys.stderr, flush=True)


def _on_telemetry(_char, data: bytearray):
    """设备 TX notify 回调:电量遥测 {"t":"bat","pct":..,"up":..} → 追加到日志文件(带墙钟时间)。"""
    try:
        txt = bytes(data).decode("utf-8", "replace").strip()
    except Exception:
        return
    import time
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {txt}"
    log("telemetry", txt)
    try:
        with open(TELEMETRY_LOG, "a") as f:
            f.write(line + "\n")
    except Exception as e:
        log(f"写遥测日志失败: {e}")


# ---------- BLE 分帧发送 ----------
async def send_json(client: BleakClient, obj: dict):
    data = (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")
    mtu = getattr(client, "mtu_size", 0) or 23
    chunk = max(20, mtu - 3)
    for i in range(0, len(data), chunk):
        await client.write_gatt_char(NUS_RX, data[i:i + chunk], response=True)


def ev_msg(ev: dict, live: bool) -> dict:
    return {
        "t": "ev", "live": live,
        "kind": ev.get("kind", "done"), "src": ev.get("src", ""),
        "project": ev.get("project", "?"), "msg": ev.get("msg", ""),
        "meta": ev.get("meta", ""), "ts": ev.get("ts", 0),
    }


# ---------- MQTT(paho 线程)→ asyncio 队列 ----------
class Bridge:
    def __init__(self, loop: asyncio.AbstractEventLoop):
        self.loop = loop
        self.q: asyncio.Queue = asyncio.Queue()       # 待发给设备的消息(仅连接时投递)
        self.history: collections.deque = collections.deque(maxlen=HISTORY_N)
        self.connected = threading.Event()

    def start_mqtt(self):
        import paho.mqtt.client as mqtt

        def on_connect(c, u, flags, rc, props=None):
            log(f"MQTT connected rc={rc}, 订阅 {EVENTS_TOPIC} / {CMD_TOPIC}")
            c.subscribe([(EVENTS_TOPIC, 1), (CMD_TOPIC, 1)])

        def on_message(c, u, m):
            try:
                payload = m.payload.decode("utf-8", "replace")
            except Exception:
                return
            if m.topic == CMD_TOPIC:
                cmd = payload.strip().strip('"')
                if "ota" in cmd:
                    self._enqueue({"t": "cmd", "cmd": "ota"})
                return
            # 事件
            try:
                ev = json.loads(payload)
            except Exception:
                return
            self.history.append(ev)                    # 始终进历史(供重连补发)
            self._enqueue(ev_msg(ev, live=True))       # 连接时会被消费,否则丢弃(重连由 history 补)

        cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        cli.on_connect = on_connect
        cli.on_message = on_message
        cli.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
        cli.loop_start()
        return cli

    def _enqueue(self, item: dict):
        # 仅在设备已连接时投递即时消息;未连接时丢弃(重连会用 history 补发),避免队列膨胀
        if item.get("t") == "cmd" or self.connected.is_set():
            self.loop.call_soon_threadsafe(self.q.put_nowait, item)

    async def run_ble(self):
        while True:
            try:
                log(f"扫描 {DEVICE_NAME} ...")
                dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
                if not dev:
                    log("未发现设备,重试")
                    await asyncio.sleep(RECONNECT_DELAY)
                    continue

                def _on_disc(_):
                    self.connected.clear()
                    log("设备断开")

                async with BleakClient(dev, disconnected_callback=_on_disc) as client:
                    log(f"已连接 {dev.address} (mtu={getattr(client,'mtu_size','?')})")
                    # 订阅设备上报的电量遥测(TX notify),写入日志文件供实测续航
                    try:
                        await client.start_notify(NUS_TX, _on_telemetry)
                    except Exception as e:
                        log(f"订阅遥测失败(不影响通知): {e}")
                    # 清掉断连期间堆积的即时队列,避免与 history 补发重复
                    while not self.q.empty():
                        self.q.get_nowait()
                    # 补发历史(旧→新,live=false 只进列表不蜂鸣)
                    for ev in list(self.history):
                        await send_json(client, ev_msg(ev, live=False))
                    self.connected.set()
                    # 消费即时消息;定时醒来检查是否还连着
                    while client.is_connected:
                        try:
                            item = await asyncio.wait_for(self.q.get(), timeout=5.0)
                        except asyncio.TimeoutError:
                            continue
                        await send_json(client, item)
            except Exception as e:
                log(f"BLE 循环异常: {e}")
            finally:
                self.connected.clear()
            await asyncio.sleep(RECONNECT_DELAY)


async def _amain():
    loop = asyncio.get_running_loop()
    br = Bridge(loop)
    br.start_mqtt()
    await br.run_ble()


async def _atest(msg: str):
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
    if not dev:
        log("未发现设备"); return
    async with BleakClient(dev) as client:
        await send_json(client, {"t": "ev", "live": True, "kind": "done",
                                 "src": "codex", "project": "manual-test",
                                 "msg": msg, "meta": "手动测试", "ts": 0})
        log("已发送:", msg)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--test":
        asyncio.run(_atest(sys.argv[2]))
    else:
        try:
            asyncio.run(_amain())
        except KeyboardInterrupt:
            pass
