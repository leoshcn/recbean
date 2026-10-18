// main/rb_store.c —— NVS 读写。约定见 rb_store.h。
#include "rb_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "rb_store";
static const char *NS = "recbean";   // NVS 命名空间（≤ 15 字节）

esp_err_t rb_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 不可用(%s)，擦除后重建；需要重新配网", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
    return err;
}

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) dst[0] = '\0';
}

esp_err_t rb_store_load(rb_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;   // 从未写过：全新设备
    if (err != ESP_OK) return err;
    get_str(h, "ssid", out->ssid, sizeof(out->ssid));
    get_str(h, "pass", out->password, sizeof(out->password));
    get_str(h, "srv", out->server, sizeof(out->server));
    get_str(h, "tok", out->token, sizeof(out->token));
    nvs_close(h);
    return ESP_OK;
}

esp_err_t rb_store_save_wifi(const char *ssid, const char *password, const char *server)
{
    if (!ssid || !password || !server) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", password);
    if (err == ESP_OK) err = nvs_set_str(h, "srv", server);
    if (err == ESP_OK) {
        esp_err_t e = nvs_erase_key(h, "tok");
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) err = e;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t rb_store_save_token(const char *token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "tok", token);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t erase_keys(const char *const *keys, size_t n)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < n; i++) {
        esp_err_t e = nvs_erase_key(h, keys[i]);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) err = e;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t rb_store_clear_token(void)
{
    static const char *const keys[] = {"tok"};
    return erase_keys(keys, 1);
}

esp_err_t rb_store_clear_all(void)
{
    static const char *const keys[] = {"ssid", "pass", "srv", "tok"};
    return erase_keys(keys, 4);
}
