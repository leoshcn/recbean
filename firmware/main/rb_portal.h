// main/rb_portal.h —— SoftAP 配网门户：通配 DNS + HTTP 配置页（热点 RecBean-Setup）。
//
// 只在配网阶段短时运行（design：热点配网），由 rb_net 任务启停；成功连上 Wi-Fi
// 后停止 DNS/HTTP 并关闭 AP，释放 socket 与内存（参考
// docs/reference/phoenixzhc/softap-provisioning-and-resource-budget.md）。
// HTTP 处理函数运行在 httpd 任务中：只解析有界表单并把结果交给网络任务，
// 不在这里连 Wi-Fi 或写 NVS。
#pragma once

#include "rb_setup.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RB_PORTAL_SCAN_MAX 12     // 页面最多列出的网络数

typedef struct {
    char ssid[RB_SSID_CAP];
    int8_t rssi;
    bool open;                    // 开放网络（无需密码）
} rb_scan_ap_t;

typedef enum {
    RB_PORTAL_WAITING = 0,        // 等待提交
    RB_PORTAL_JOINING,            // 已提交，正在连接
    RB_PORTAL_FAILED,             // 上次连接失败
    RB_PORTAL_JOINED,             // 已连上，请看设备屏幕
} rb_portal_state_t;

// 启动 DNS 与 HTTP 服务（AP 网络接口必须已就绪）。server_prefill 为表单中预填的服务器地址。
// notify 为收到表单后要唤醒的任务（rb_net）。
esp_err_t rb_portal_start(const char *server_prefill, TaskHandle_t notify);

// 停止 DNS 与 HTTP 服务，等待 DNS 任务退出（≤ 2 s）。幂等。
void rb_portal_stop(void);

bool rb_portal_running(void);

// 更新扫描结果（复制，按 RSSI 降序去重，最多 RB_PORTAL_SCAN_MAX 条）。
void rb_portal_set_scan(const rb_scan_ap_t *aps, size_t count);

void rb_portal_set_state(rb_portal_state_t state);

// 取走已提交的表单（取走后清空内部副本）。没有新表单返回 false。
bool rb_portal_take_form(rb_setup_form_t *out);

// 页面是否请求了重新扫描（取走即清除）。
bool rb_portal_take_rescan(void);
