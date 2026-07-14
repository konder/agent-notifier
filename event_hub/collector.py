#!/usr/bin/env python3
"""事件中心 collector 主进程:采集 → 快照 → 广播渠道 + 事件/额度告警。

用法(从仓库根运行):
  python -m event_hub.collector --once --print   # 打印一次快照 JSON,不连渠道(本地调试)
  python -m event_hub.collector --once           # 采集一次并 publish,退出
  python -m event_hub.collector                  # 常驻:定时刷新 + retained 发布 + 事件推送

渠道(channels/):当前用 mqtt(设备走 WiFi+MQTT);后续可加 ble / webhook,
collector 只依赖渠道的 publish_state / publish_event 接口。
依赖:paho-mqtt(仅 MQTT 模式需要);解析/额度均 stdlib。
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _REPO_ROOT)
from collectors.snapshot import build_snapshot  # noqa: E402
from event_hub.channels.mqtt import MqttPublisher  # noqa: E402

# 真实运行配置放 config/config.toml(git 忽略);仓库里只带 config.example.toml
CONFIG_PATH = os.path.join(_REPO_ROOT, "config", "config.toml")

DEFAULTS = {
    "mqtt": {
        "host": "127.0.0.1", "port": 1883, "username": "", "password": "",
        "state_topic": "m5paper/state", "event_topic": "m5paper/events",
    },
    "collector": {"refresh_sec": 20, "with_quota": True},
    "thumb": {"port": 8080},
    "event": {"port": 8898, "quota_alert_pct": 85},
}

# 值得蜂鸣提醒的目标状态
ALERT_STATES = {"done", "needs_input"}


def _tiny_toml(path: str) -> dict:
    """极简 toml 解析(仅支持 [section] + key = value),供无 tomllib 的 py<3.11 回退。"""
    out: dict = {}
    section = None
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip()
                out[section] = {}
                continue
            if "=" not in line or section is None:
                continue
            k, v = (x.strip() for x in line.split("=", 1))
            if v and v[0] in "\"'" and v[-1] == v[0]:
                val = v[1:-1]
            elif v.lower() in ("true", "false"):
                val = v.lower() == "true"
            else:
                try:
                    val = int(v)
                except ValueError:
                    try:
                        val = float(v)
                    except ValueError:
                        val = v
            out[section][k] = val
    return out


def load_config() -> dict:
    cfg = {k: dict(v) for k, v in DEFAULTS.items()}
    try:
        try:
            import tomllib
            with open(CONFIG_PATH, "rb") as fh:
                user = tomllib.load(fh)
        except ModuleNotFoundError:
            user = _tiny_toml(CONFIG_PATH)
        for section, vals in user.items():
            cfg.setdefault(section, {}).update(vals)
    except FileNotFoundError:
        pass
    except Exception as e:
        print(f"[warn] 读取 config.toml 失败,用默认值: {e}", file=sys.stderr)
    return cfg


def _session_key(s: dict) -> str:
    return f"{s.get('src')}:{s.get('project')}:{s.get('task')}"


def diff_events(prev: dict, cur: dict) -> list[dict]:
    """比较前后两帧,找出新进入 done/needs_input 的会话 → 事件列表。"""
    prev_states = {_session_key(s): s.get("state") for s in (prev or {}).get("sessions", [])}
    events = []
    for s in cur.get("sessions", []):
        k = _session_key(s)
        new_state = s.get("state")
        if new_state in ALERT_STATES and prev_states.get(k) != new_state:
            events.append({
                "kind": new_state,
                "src": s.get("src"),
                "project": s.get("project"),
                "task": s.get("task"),
                "ts": cur.get("ts"),
            })
    return events


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true", help="采集一次后退出")
    ap.add_argument("--print", dest="do_print", action="store_true",
                    help="打印快照到 stdout(不连 MQTT)")
    ap.add_argument("--no-quota", action="store_true", help="跳过额度查询(加速调试)")
    args = ap.parse_args()

    cfg = load_config()
    with_quota = cfg["collector"].get("with_quota", True) and not args.no_quota

    # 纯打印模式:不碰 MQTT
    if args.do_print:
        snap = build_snapshot(with_quota=with_quota)
        print(json.dumps(snap, ensure_ascii=False, indent=2))
        return

    # 启动灰度缩略图 HTTP 服务(设备详情页拉图)
    try:
        from event_hub import thumbserver
        thumbserver.start(int(cfg["thumb"].get("port", 8080)))
        print(f"[thumb] serving on :{cfg['thumb'].get('port', 8080)}", file=sys.stderr)
    except Exception as e:
        print(f"[warn] thumb server 启动失败: {e}", file=sys.stderr)

    pub = MqttPublisher(cfg)

    # 事件接收服务(hook POST 进来 → 补全 → 发 MQTT)
    try:
        from event_hub import eventserver
        eventserver.start(int(cfg["event"].get("port", 8898)), pub.publish_event)
        print(f"[event] 接收端口 :{cfg['event'].get('port', 8898)}", file=sys.stderr)
    except Exception as e:
        print(f"[warn] event server 启动失败: {e}", file=sys.stderr)

    quota_alert_pct = int(cfg["event"].get("quota_alert_pct", 85))
    alerted = {}  # (provider,window) -> reset_ts,避免重复告警直到重置
    codex_done = {}   # key -> 最近已通知的完成时刻(codex 无 hook,靠日志检测)
    first_pass = True

    def _emit_done_from_session(s):
        e = s.get("elapsed_s"); t = s.get("tokens")
        meta = []
        if e and e >= 0: meta.append(f"用时 {e//60}m{e%60}s" if e >= 60 else f"用时 {e}s")
        if t and t >= 0: meta.append(f"{t/1e6:.1f}M tok" if t >= 1e6 else f"{t//1000}k tok")
        if s.get("model"): meta.append(s["model"])
        pub.publish_event({
            "kind": "done", "src": "codex", "project": s.get("project", "?"),
            "msg": (s.get("last_msg") or "任务完成")[:600], "meta": " · ".join(meta),
            "ts": s.get("done_ts") or int(time.time()),
        })

    prev = None
    try:
        while True:
            snap = build_snapshot(with_quota=with_quota)
            pub.publish_state(snap)
            for ev in diff_events(prev, snap):
                pub.publish_event(ev)
                print(f"[event] {ev['kind']} {ev['src']}/{ev['project']}: {ev['task']}", file=sys.stderr)
            # 额度阈值告警(达阈值发一次,直到该窗口重置)
            for prov in ("codex", "claude"):
                q = (snap.get("quota") or {}).get(prov)
                if not q:
                    continue
                for win, label in (("h5", "5小时"), ("week", "本周")):
                    pct = q.get(win)
                    reset = q.get(win + "_reset")
                    if pct is not None and pct >= quota_alert_pct:
                        key = (prov, win)
                        if alerted.get(key) != reset:   # 同一重置周期只报一次
                            alerted[key] = reset
                            pub.publish_event({
                                "kind": "quota", "src": prov, "project": prov.upper(),
                                "msg": f"{label}额度已用 {pct:.0f}%,注意节流或等待重置。",
                                "meta": f"{'真实' if q.get('real') else '估算'} · {label}窗口",
                                "ts": snap.get("ts"),
                            })
                            print(f"[event] quota {prov}/{win} {pct}%", file=sys.stderr)

            # codex 无 hook:靠 rollout 的 completed_at 检测每轮完成
            for s in snap.get("sessions", []):
                if s.get("src") != "codex":
                    continue
                dt = s.get("done_ts") or 0
                if dt <= 0:
                    continue
                key = "codex:" + (s.get("project") or "?")
                if not first_pass and dt > codex_done.get(key, 0):
                    _emit_done_from_session(s)
                    print(f"[event] codex done {s.get('project')}", file=sys.stderr)
                codex_done[key] = max(dt, codex_done.get(key, 0))
            first_pass = False
            prev = snap
            n = len(snap["sessions"])
            q = snap["quota"]
            print(f"[state] {time.strftime('%H:%M:%S')} sessions={n} "
                  f"codex={_fmt_q(q.get('codex'))} claude={_fmt_q(q.get('claude'))}", file=sys.stderr)
            if args.once:
                break
            time.sleep(int(cfg["collector"].get("refresh_sec", 20)))
    except KeyboardInterrupt:
        pass
    finally:
        pub.close()


def _fmt_q(q):
    if not q or q.get("h5") is None:
        return "-"
    return f"5h {q.get('h5')}%/wk {q.get('week')}%"


if __name__ == "__main__":
    main()
