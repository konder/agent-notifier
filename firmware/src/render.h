#pragma once
#include "types.h"

// 初始化屏幕(旋转/配色),开机调用一次。clearScreen=false 时保留当前画面(深睡唤醒用)
void renderInit(bool clearScreen = true);

// 列表页。full=true 全刷去残影,否则局刷。内部记录卡片位置供触摸命中。
void renderList(const Snapshot& snap, int batteryPct, bool usb, bool full);

// 触摸命中:返回被点中的会话下标(对应 snap.sessions),无则 -1
int cardIndexAtTouch(int x, int y);

// 触摸是否落在底栏版本号区域(用于"点版本主动查更新")
bool versionAtTouch(int x, int y);

// 详情页:文字结果 + 可选一张已拉取的 JPEG(jpg 可为 null)。
void renderDetail(const Session& s, long nowTs, const unsigned char* jpg, unsigned int jpgLen,
                  int imgIdx, int imgCount);

// 事件通知全屏卡。kind: done | needs_input | quota
void renderNotify(const String& kind, const String& src, const String& project,
                  const String& msg, const String& meta, long ts);

// 待命屏:最近事件历史列表(最新在上)。items 按新→旧传入,n 条;full=true 全刷去残影。
void renderIdle(const EventItem* items, int n, int batteryPct, bool bleConnected,
                int fwVersion, bool full);

// 休眠页:大字电量 + 电池条 + 下次唤醒时间(深睡前渲染,墨水屏保留)
void renderSleep(int batteryPct, const String& wakeAt);

// 顶部状态提示(连接中/离线等)
void renderStatus(const char* msg);
