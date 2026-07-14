# agent-notifier

把长时间运行的 **AI 编程 agent(Codex / Claude Code)** 的进度、结果和订阅额度,推到一块
**M5PaperS3 电子墨水屏**上——抬头一瞥就知道任务跑到哪了、什么时候该回去,任务完成/等你输入
时**叮一声 + 弹屏提醒**。低功耗、常显、不刺眼。

> 起因:agent 干活周期长,人容易切去刷手机/做别的事;需要一个"随手一瞥 + 完成叫我"的物理面板,
> 而不是不停切屏盯进度。

![列表页](docs/design/design_split.png)

## 架构(三层)

```
┌─────────────────────────────────────────────────────────────┐
│ collectors/     【1】agent 日志采集                            │
│   读 ~/.codex/sessions 与 ~/.claude/projects 的本地日志,       │
│   归一化成「会话状态 + 当前任务 + 耗时 + token + 额度」快照。    │
│   纯本地、纯读文件,不碰任何 OAuth/token(见「合规」)。         │
└───────────────┬─────────────────────────────────────────────┘
                │ build_snapshot()
┌───────────────▼─────────────────────────────────────────────┐
│ event_hub/      【2】事件中心:采集编排 + 事件服务 + 广播渠道   │
│   collector.py   主进程:定时快照 → 发 MQTT + 完成/额度事件     │
│   eventserver.py hook POST → 事件(agent 一结束就实时上报)     │
│   thumbserver.py 固件 OTA(/fw)HTTP 服务(设备走 WiFi 拉)     │
│   channels/                                                   │
│     mqtt.py   MQTT 渠道(collector→broker 发布)               │
│     ble.py    蓝牙渠道 daemon:订阅 broker → BLE 转发给设备     │
└───────────────┬─────────────────────────────────────────────┘
     broker(飞牛 mosquitto) │ BLE(NUS,分帧)
┌───────────────▼─────────────────────────────────────────────┐
│ firmware/       【3】设备 ROM(M5PaperS3 / ESP32-S3)          │
│   纯 BLE 外设:待命屏(最近事件列表)+ 事件→蜂鸣+全屏通知卡;    │
│   WiFi 仅在收到 BLE "ota" 指令时临时开做 OTA。                 │
└─────────────────────────────────────────────────────────────┘

运行位置:collector 在 Mac Mini(日志在此);broker + BLE 桥在飞牛 NAS(Linux/BlueZ,常开、无
macOS 蓝牙权限坑);OTA 固件服务(thumbserver)留 Mac Mini(设备走家里 WiFi 可达)。
```

## 目录

| 路径 | 作用 |
|---|---|
| `collectors/` | codex/claude 日志解析(`codex.py`/`claude.py`/`common.py`)+ 快照组装(`snapshot.py`) |
| `event_hub/` | 事件中心主进程 `collector.py`、事件接收 `eventserver.py`、缩略图+OTA `thumbserver.py` |
| `event_hub/channels/` | 广播渠道:`mqtt.py`(collector 发布)、`ble.py`(飞牛 BLE 桥 daemon);后续可加 webhook 等 |
| `firmware/` | M5PaperS3 固件(PlatformIO + M5Unified + NimBLE,当前 v23:纯 BLE 通知端) |
| `experiments/ble-poc/` | BLE 外设验证固件(留档) |
| `deploy/` | 部署件:launchd 模板、`ble-bridge.service`(飞牛 systemd)、`mosquitto.conf`、`push_fw.sh`、agent hooks |
| `tools/sim_render.py` | 墨水屏布局模拟器(在电脑上渲染 PNG 迭代设计,不必反复烧录) |
| `config/config.example.toml` | 配置示例(复制为 `config/config.toml` 填真实值,已被 git 忽略) |

## 快速开始

### Gateway(采集 + 事件中心,跑在一台常开机器上)

```bash
pip install -r requirements.txt          # 仅 MQTT/缩略图需要 paho-mqtt / Pillow
cp config/config.example.toml config/config.toml   # 按需改 broker 地址/端口

# 本地调试:打印一次快照(不连 MQTT / 不查额度)
python3 -m event_hub.collector --once --print --no-quota

# 常驻:定时快照 + retained 发布 + 事件/额度告警
python3 -m event_hub.collector
```

持久化用 `deploy/com.user.m5monitor.plist.template`(macOS launchd):把 `__INSTALL_DIR__`
替换为仓库绝对路径,放进 `~/Library/LaunchAgents/` 后 `launchctl`。

MQTT broker 用 mosquitto:`deploy/mosquitto.conf` 是最小配置(局域网监听 + 允许匿名,仅家用)。

### 飞牛 NAS(broker + BLE 桥)

```bash
sudo apt install -y bluez mosquitto            # 蓝牙栈 + broker
pip3 install --break-system-packages bleak paho-mqtt
# 部署仓库到飞牛,装 systemd 服务(把 __INSTALL_DIR__ 换成仓库绝对路径):
sudo cp deploy/ble-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now ble-bridge
# 联调:手动发一条到设备
python3 -m event_hub.channels.ble --test "手动测试通知"
```

### 设备固件(纯 BLE 通知端)

```bash
cd firmware
cp include/secrets.h.example include/secrets.h   # WiFi(仅 OTA 用)+ OTA 服务器 host(Mac Mini 家里 IP)
pio run -t upload -t monitor                      # 首次 USB 烧录
```
之后改固件:`deploy/push_fw.sh`(`config.h` 的 `FW_VERSION` +1 后跑)发布 → 飞牛发
`m5paper/cmd=ota` → 桥经 BLE 通知设备 → 设备临时开 WiFi 从 Mac Mini 拉固件**无线更新**,不必插线。

## 事件与主题

collector 发布到飞牛 broker;BLE 桥订阅后转发给设备。

| Topic | QoS | 说明 |
|---|---|---|
| `m5paper/events` | 1 | 离散事件:`done` / `needs_input` / `quota` |
| `m5paper/cmd` | 1 | 指令,如 `ota`(远程触发设备 WiFi-OTA) |
| `m5paper/state` | 0 + retained | 完整快照(BLE 纯通知端不消费,保留给未来仪表盘) |

事件 JSON 示例(collector 发):
```json
{"kind":"done","src":"codex","project":"my-app",
 "msg":"已完成:导出 storyboard...","meta":"用时 2m · 1.0M tok · gpt-5.5","ts":1783344380}
```
BLE 桥→设备帧(分帧 + `\n` 终止,设备重组):`{"t":"ev","live":true,...}`(实时,蜂鸣+弹屏)、
`{"t":"ev","live":false,...}`(重连补发的历史,仅进待命列表)、`{"t":"cmd","cmd":"ota"}`。

## 额度与合规 ⚠️

本项目**只读你自己机器上的本地日志,不调用任何 OAuth/token 接口**,零封禁风险:

- **Codex**:5h/周额度是 codex 自己写在本地 rollout(`~/.codex/sessions/**`)的 `rate_limits`,
  直接读即可(真实值)。注意:Codex Desktop 的 computer-use 类会话会记 `0`,故取「全局最新的
  非零读数」,重度用这类会话时额度可能滞后。
- **Claude**:官方日志不含 rate_limit 字段,故用本地 JSONL 的 token 按 5h/7天滚动窗口累加
  **估算**(ccusage 式纯本地做法),可选一次性校准出百分比。

> 那些"很准"的第三方工具用的是订阅 OAuth token 调 Anthropic/OpenAI 的用量接口——准确但触碰
> 服务条款、有账号风险。本项目**刻意不走这条路**,用可读性换零风险。

## 通知触发

| | 完成 | 待输入 | 机制 |
|---|---|---|---|
| Claude | ✅ | ✅ | hook(`Stop`/`Notification` → `deploy/hooks/on_event.sh` → eventserver) |
| Codex | ✅ | —— | 日志检测(rollout `task_complete` 的 `completed_at`,≤刷新周期) |
| 额度告警 | ✅ 两家 | | 采集器算,`≥85%` 自动发一次 |

装 hook:`python3 deploy/hooks/install_hooks.py`(幂等追加,自动备份;`--uninstall` 卸载)。

## 为什么走 BLE

设备当"电池 + 纯通知端"时,WiFi 有个根本矛盾:即时推送要保持长连接(费电)、省电要深睡
(通知延迟)。**BLE 兼得低功耗 + 即时**,所以运行时纯 BLE、WiFi 仅按需 OTA:

- ✅ **飞牛 NAS(Debian/BlueZ,MediaTek MT7961)当 BLE 中心**——headless 常驻无 macOS TCC 权限坑;
- ✅ 设备维持 BLE 连接(ESP32 默认 modem sleep 低功耗),事件即时到,数天续航;深睡会断 BLE,故不深睡;
- ✅ 事件正文可能 >MTU → 桥按 MTU 分帧写、`\n` 终止,设备重组;连接后协商 MTU 512 减少分帧;
- ✅ 设备重启后待命列表不空:桥重连时以 `live:false` 补发最近 N 条历史。

> 距离:PoC 实测飞牛↔真机 RSSI -66(BLE 到 -85 都能连),设备保持在该位置附近即可。

## 硬件

M5PaperS3:ESP32-S3R8 + 4.7" 960×540 16 级灰度墨水屏(ED047TC1 直驱)+ GT911 触摸 + 无源蜂鸣器
+ 1800mAh 电池。⚠️ **不要用 QC/PD 快充口供电**(时序问题可能过压烧板),用普通 5V。

## License

MIT
