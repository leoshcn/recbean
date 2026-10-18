// main/rb_ui.h —— 录音豆界面（LVGL，240×320 竖屏，深色主题，全新设计，不使用上游 demo 外壳）。
//
// 页面：待机 / 连接中 / 录音（含“再按确定结束”确认层与保存中层）/ 结束 / 设置（含服务器地址、
// 设备信息详情）/ 配网 / 配对。顶栏常驻：左侧 Wi-Fi 与服务器状态（录音时换成红点 + 计时 +
// 断网缓存提示），右上角电量。底部提示条显示 rb_app 的 notice。
// 录音笔记模式（app->mode == RB_MODE_NOTE）：录音页不显示说话人标签，顶栏显示“连接中/正在听”；
// 保存成功的结束页显示“已记入今日笔记”和本段文字（定稿句拼接，超出 5 行以省略号结尾）。
// 所有函数都必须在 app 任务中、持有 bsp_lvgl_lock() 时调用。
#pragma once

#include "rb_app.h"
#include "rb_net.h"
#include "rb_transcript.h"

#include <stdint.h>

typedef struct {
    rb_net_status_t net;      // 网络任务状态快照
    int battery_soc;          // 0..100；-1 = 电量计不可用（顶栏不显示电量）
    int battery_mv;           // -1 = 不可用
    uint32_t backlog_ms;      // 未上传音频时长
    uint32_t free_heap;       // 设备信息页显示
    uint32_t largest_block;
    const char *fw_version;
} rb_ui_info_t;

// 创建全部页面对象（只创建一次，之后只切换显示与更新文本）。
void rb_ui_init(void);

// 按当前状态刷新界面；只修改发生变化的对象。
void rb_ui_render(const rb_app_t *app, const rb_transcript_t *ts, const rb_ui_info_t *info,
                  uint32_t now);
