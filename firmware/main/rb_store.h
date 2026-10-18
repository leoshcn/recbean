// main/rb_store.h —— NVS 中的配网与配对数据（命名空间 "recbean"）。
//
// 键：ssid / pass / srv（"host:port"）/ tok（设备令牌）。
// 只在 STA 用新凭据拿到 IP 之后才写 Wi-Fi（错误密码不会覆盖可用配置）；
// 写入新 Wi-Fi/服务器时同时清除令牌（换服务器后旧令牌无效，必须重新配对）。
// 所有函数都是阻塞的 Flash 操作（毫秒级），只在网络/会话任务中调用，不在按键回调中调用。
#pragma once

#include "rb_proto.h"
#include "rb_setup.h"

#include "esp_err.h"

typedef struct {
    char ssid[RB_SSID_CAP];                 // 空串 = 未配网
    char password[RB_PASS_CAP];
    char server[RB_SERVER_CAP];             // 空串时调用方使用 RB_DEFAULT_SERVER
    char token[RB_PROTO_TOKEN_MAX + 1];     // 空串 = 未配对
} rb_config_t;

// 初始化 NVS。分区损坏/版本不兼容时擦除后重建（本固件 NVS 只存上述配置，
// 丢失后重新配网即可恢复，优于设备无法启动）。
esp_err_t rb_store_init(void);

// 读取配置；缺失的键为空串。NVS 不可用时返回错误且 out 全为空串。
esp_err_t rb_store_load(rb_config_t *out);

// 保存 Wi-Fi 与服务器地址，并清除令牌。
esp_err_t rb_store_save_wifi(const char *ssid, const char *password, const char *server);

esp_err_t rb_store_save_token(const char *token);
esp_err_t rb_store_clear_token(void);

// 清除全部配网/配对数据（设置页“重新配网”）。
esp_err_t rb_store_clear_all(void);
