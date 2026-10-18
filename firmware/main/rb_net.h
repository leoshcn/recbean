// main/rb_net.h —— 网络任务：Wi-Fi STA、SoftAP 配网、4 位码配对、服务器在线探测。
//
// 阶段（rb_net_phase_t，经 rb_bus 上报给 app）：
//   PROV_AP     无 Wi-Fi 配置 → 打开热点 RecBean-Setup + 配置页
//   PROV_JOINING 收到表单 → 用新凭据连 Wi-Fi（≤ 20 s）；成功才写 NVS
//   PAIRING     Wi-Fi 已连、无令牌 → POST /device/pair 取码，每 3 s 轮询 GET /device/pair/{id}
//   READY       有令牌 → 每 15 s TCP 探测服务器端口（录音中暂停）
// 所有阻塞操作（扫描、HTTP、探测、NVS）都在本任务中执行；对外接口只置标志并唤醒任务，
// 可从 app 任务或会话任务调用。
#pragma once

#include "rb_app.h"
#include "rb_portal.h"
#include "rb_proto.h"
#include "rb_setup.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    rb_net_phase_t phase;
    bool wifi_up;                         // STA 已拿到 IP
    char ip[16];                          // STA IP（点分十进制），未连接为空
    char ssid[RB_SSID_CAP];               // 已保存的 Wi-Fi 名称
    char server[RB_SERVER_CAP];           // 当前服务器 "host:port"
    char device_id[20];                   // "rb-" + MAC
    char pair_code[RB_PROTO_PAIR_CODE + 1]; // 配对码，空 = 尚未取得
    int8_t server_ok;                     // -1 未知 / 0 不可达 / 1 可达（配对与探测共用）
    rb_portal_state_t portal;             // 配网页状态（PROV_* 阶段有效）
    bool phone_joined;                    // 有手机连上了热点
} rb_net_status_t;

// 读取 NVS、确定初始阶段并启动网络任务。必须在 rb_store_init() 之后调用。
esp_err_t rb_net_start(void);

// 初始阶段（rb_net_start 之后有效），供 app 状态机初始化。
rb_net_phase_t rb_net_initial_phase(void);

// 复制一份当前状态（加锁复制，几十字节）。
void rb_net_get_status(rb_net_status_t *out);

// 会话任务取连接参数；未处于 READY 返回 false。
bool rb_net_get_session_config(char *server, size_t server_cap, char *token, size_t token_cap);

// 清除全部配网数据并重新打开配网热点（设置页“重新配网”）。
void rb_net_request_reprovision(void);

// 服务端拒绝令牌（error.code=auth）：清除令牌，回到配对阶段。
void rb_net_request_repair(void);

// 录音会话进行中时暂停服务器探测，避免与上传争用。
void rb_net_set_session_active(bool active);

// 打印当前空闲堆与最大连续空闲块（内存预算记录点）。
void rb_net_log_heap(const char *where);
