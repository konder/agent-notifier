#pragma once
#include <Arduino.h>

// 与 gateway snapshot.py 的 JSON schema 对应

struct Quota {
    bool  valid = false;
    bool  real  = false;   // true=官方真值(codex);false=日志估算(claude)
    float h5    = -1;      // 已用百分比,-1=无(未校准)
    float week  = -1;
    long  h5_tokens   = -1; // 估算模式下的窗口内 token 数
    long  week_tokens = -1;
    long  h5_reset   = 0;   // unix 秒
    long  week_reset = 0;
    String plan;
};

#define MAX_IMAGES_UI 4

struct ImgRef {
    String id;
    String name;
};

struct Session {
    String src;      // "codex" | "claude"
    String project;
    String state;    // running | idle | done | needs_input
    String task;
    String last_msg; // 最新输出(结果)
    String model;
    long   elapsed_s = -1;
    long   tokens    = -1;
    long   last_activity = 0;
    ImgRef images[MAX_IMAGES_UI];
    int    nImages = 0;
};

#define MAX_SESSIONS_UI 8

struct Snapshot {
    long    ts = 0;
    String  hhmm;          // 生成时刻 HH:MM(设备显示"更新 …")
    Quota   codex;
    Quota   claude;
    Session sessions[MAX_SESSIONS_UI];
    int     nSessions = 0;
};

// v23 纯通知端:待命屏的历史事件列表项(设备 RAM 环形缓冲)
struct EventItem {
    String kind;      // done | needs_input | quota
    String src;       // codex | claude
    String project;
    String summary;   // 一行摘要(列表用,取正文首行/截断)
    long   ts = 0;
};
