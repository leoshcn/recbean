// tests/test_rb_proto.c —— 协议 v1 消息解析/构造、配对回复、配网表单与地址规范化主机测试。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rb_proto.h"
#include "rb_setup.h"

static rb_msg_t m;

static bool parse(const char *json)
{
    return rb_proto_parse(json, strlen(json), &m);
}

static void test_events(void)
{
    assert(parse("{\"type\":\"started\",\"recording_id\":\"20261018-101500-ab12\"}"));
    assert(m.type == RB_MSG_STARTED && strcmp(m.recording_id, "20261018-101500-ab12") == 0);
    assert(!m.resumed && !m.has_next_seq);

    // 续传回复（服务端扩展字段）。
    assert(parse("{\"type\":\"started\",\"recording_id\":\"r1\",\"resumed\":true,\"next_seq\":42}"));
    assert(m.resumed && m.has_next_seq && m.next_seq == 42);

    assert(parse(" { \"text\" : \"\\u4f60\\u597d\" , \"type\" : \"partial\" } "));
    assert(m.type == RB_MSG_PARTIAL && strcmp(m.text, "你好") == 0 && m.text_len == 6);

    assert(parse("{\"type\":\"final\",\"idx\":3,\"speaker\":2,\"start_ms\":1200,\"end_ms\":3400,"
                 "\"text\":\"今天\\\"开会\\\"\\n讨论\",\"extra\":{\"a\":[1,2,{\"b\":null}]}}"));
    assert(m.type == RB_MSG_FINAL && m.idx == 3 && m.speaker == 2);
    assert(m.start_ms == 1200 && m.end_ms == 3400);
    assert(strcmp(m.text, "今天\"开会\" 讨论") == 0);   // 换行被清洗为空格

    // 代理对 → 4 字节 UTF-8；孤立代理 → '?'。
    assert(parse("{\"type\":\"final\",\"idx\":0,\"text\":\"\\ud83d\\ude00x\\ud800\"}"));
    assert(strcmp(m.text, "\xF0\x9F\x98\x80x?") == 0);
    assert(m.speaker == 0);   // 缺省：未知

    assert(parse("{\"type\":\"error\",\"code\":\"asr_error\",\"msg\":\"识别服务错误\"}"));
    assert(m.type == RB_MSG_ERROR && m.err == RB_ERR_ASR && strcmp(m.text, "识别服务错误") == 0);
    assert(parse("{\"type\":\"error\",\"code\":\"auth\",\"msg\":\"device not paired\"}"));
    assert(m.err == RB_ERR_AUTH);
    assert(parse("{\"type\":\"error\",\"code\":\"busy\"}") && m.err == RB_ERR_BUSY);
    assert(parse("{\"type\":\"error\",\"code\":\"resume_failed\"}") && m.err == RB_ERR_OTHER);
    assert(strcmp(m.err_code, "resume_failed") == 0);

    assert(parse("{\"type\":\"saved\",\"recording_id\":\"r1\",\"duration_ms\":125000,\"sentences\":18}"));
    assert(m.type == RB_MSG_SAVED && m.duration_ms == 125000 && m.sentences == 18);
    assert(strcmp(m.recording_id, "r1") == 0);
    // 整数 recording_id 兼容。
    assert(parse("{\"type\":\"saved\",\"recording_id\":77,\"duration_ms\":null,\"sentences\":0}"));
    assert(strcmp(m.recording_id, "77") == 0 && m.duration_ms == 0);

    assert(parse("{\"type\":\"pong\"}") && m.type == RB_MSG_UNKNOWN);

    // 非法输入。
    assert(!parse(""));
    assert(!parse("[]"));
    assert(!parse("{\"type\":\"final\""));
    assert(!parse("{\"text\":\"x\"}"));                     // 缺 type
    assert(!parse("{\"type\":\"final\",\"text\":\"a\\q\"}"));  // 非法转义
    assert(!parse("{\"type\":\"partial\",\"text\":\"a"));    // 未闭合
    {
        char deep[200];
        size_t n = 0;
        n += (size_t)snprintf(deep + n, sizeof(deep) - n, "{\"type\":\"x\",\"a\":");
        for (int i = 0; i < 40; i++) deep[n++] = '[';
        for (int i = 0; i < 40; i++) deep[n++] = ']';
        deep[n++] = '}';
        deep[n] = '\0';
        assert(!parse(deep));                                // 过深嵌套被拒绝
    }

    // 长文本：final 保留开头、partial 保留末尾，均 ≤ 256 B 且字符完整。
    static char big[4096];
    size_t n = 0;
    n += (size_t)snprintf(big, sizeof(big), "{\"type\":\"partial\",\"text\":\"");
    for (int i = 0; i < 300; i++) {
        memcpy(big + n, i < 299 ? "\\u4e00" : "\\u4e8c", 6);   // 末字为“二”
        n += 6;
    }
    n += (size_t)snprintf(big + n, sizeof(big) - n, "\"}");
    assert(rb_proto_parse(big, n, &m));
    assert(m.text_len == 255 && m.text_truncated);
    assert(memcmp(m.text + 252, "二", 3) == 0);
    memcpy(big + 9, "final\"  ", 8);   // 把 partial" 改成 final" 加两个空格，长度不变
    assert(rb_proto_parse(big, n, &m) && m.type == RB_MSG_FINAL);
    assert(m.text_len == 255 && memcmp(m.text, "一", 3) == 0);
}

static void test_builders(void)
{
    char buf[160];
    assert(rb_proto_build_start(buf, sizeof(buf), "0.1.0") > 0);
    assert(strcmp(buf, "{\"type\":\"start\",\"sample_rate\":16000,\"ch\":1,"
                       "\"codec\":\"ima_adpcm\",\"fw\":\"0.1.0\"}") == 0);
    assert(rb_proto_build_stop(buf, sizeof(buf)) == 15 && strcmp(buf, "{\"type\":\"stop\"}") == 0);
    assert(rb_proto_build_resume(buf, sizeof(buf), "r\"1", 4294967295u) > 0);
    assert(strcmp(buf, "{\"type\":\"resume\",\"recording_id\":\"r\\\"1\",\"next_seq\":4294967295}") == 0);
    assert(rb_proto_build_resume(buf, sizeof(buf), "", 1) < 0);
    assert(rb_proto_build_stop(buf, 5) < 0);
    assert(rb_proto_build_pair_request(buf, sizeof(buf), "rb-001122334455") > 0);
    assert(strcmp(buf, "{\"device_id\":\"rb-001122334455\"}") == 0);
    // 构造结果能被自己的解析器读回（服务端消息解析与客户端构造共用转义规则）。
    assert(rb_proto_build_resume(buf, sizeof(buf), "abc", 7) > 0);
    rb_msg_t back;
    assert(rb_proto_parse(buf, strlen(buf), &back) && back.type == RB_MSG_UNKNOWN);
    assert(strcmp(back.recording_id, "abc") == 0 && back.next_seq == 7);
}

static void test_pair(void)
{
    rb_pair_reply_t r;
    const char *a = "{\"code\":\"0427\",\"expires_in\":600}";
    assert(rb_proto_parse_pair(a, strlen(a), &r) && r.has_code && strcmp(r.code, "0427") == 0);
    assert(!r.has_token);
    const char *b = "{\"status\":\"pending\",\"code\":\"0427\"}";
    assert(rb_proto_parse_pair(b, strlen(b), &r) && strcmp(r.status, "pending") == 0);
    const char *c = "{\"status\":\"paired\",\"token\":\"AbC-_123xyz\"}";
    assert(rb_proto_parse_pair(c, strlen(c), &r) && r.has_token && strcmp(r.token, "AbC-_123xyz") == 0);
    const char *d = "{\"code\":42}";
    assert(rb_proto_parse_pair(d, strlen(d), &r) && r.has_code && strcmp(r.code, "0042") == 0);
    const char *e = "{\"code\":\"12a4\",\"token\":\"bad token\"}";
    assert(rb_proto_parse_pair(e, strlen(e), &r) && !r.has_code && !r.has_token);
    const char *f = "{\"token\":\"\"}";
    assert(rb_proto_parse_pair(f, strlen(f), &r) && !r.has_token);
    assert(!rb_proto_parse_pair("nope", 4, &r));
}

static void test_setup(void)
{
    char out[RB_SERVER_CAP];
    assert(rb_server_normalize("192.168.2.144:8080", out, sizeof(out)) && strcmp(out, "192.168.2.144:8080") == 0);
    assert(rb_server_normalize("192.168.2.144", out, sizeof(out)) && strcmp(out, "192.168.2.144:8090") == 0);
    assert(rb_server_normalize(" http://rec.lan:9000/ ", out, sizeof(out)) && strcmp(out, "rec.lan:9000") == 0);
    assert(rb_server_normalize("ws://host", out, sizeof(out)) && strcmp(out, "host:8090") == 0);
    assert(!rb_server_normalize("", out, sizeof(out)));
    assert(!rb_server_normalize("host:0", out, sizeof(out)));
    assert(!rb_server_normalize("host:65536", out, sizeof(out)));
    assert(!rb_server_normalize("host:80a", out, sizeof(out)));
    assert(!rb_server_normalize("ho st", out, sizeof(out)));
    assert(!rb_server_normalize("host/path", out, sizeof(out)));
    assert(!rb_server_normalize("https://host", out, sizeof(out)));
    assert(!rb_server_normalize("-bad.host", out, sizeof(out)));

    rb_setup_form_t form;
    const char *body = "ssid=Home+WiFi&password=p%40ss%26word&server=192.168.2.144%3A8080";
    assert(rb_setup_parse_form(body, strlen(body), &form));
    assert(strcmp(form.ssid, "Home WiFi") == 0 && strcmp(form.password, "p@ss&word") == 0);
    assert(strcmp(form.server, "192.168.2.144:8080") == 0);
    // 手动输入覆盖点选；server 留空用默认；开放网络无密码。
    const char *b2 = "ssid=Other&ssid_manual=%E5%AE%B6&password=&server=";
    assert(rb_setup_parse_form(b2, strlen(b2), &form));
    assert(strcmp(form.ssid, "家") == 0 && form.password[0] == '\0');
    assert(strcmp(form.server, RB_DEFAULT_SERVER) == 0);
    // 失败：无 SSID、密码过短、服务器非法、%00、%不完整、正文过长。
    const char *bad[] = {
        "password=12345678&server=",
        "ssid=a&password=1234567",
        "ssid=a&server=bad%20host",
        "ssid=a%00b",
        "ssid=a%4",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(!rb_setup_parse_form(bad[i], strlen(bad[i]), &form));
        assert(form.ssid[0] == '\0' && form.password[0] == '\0');
    }
    static char huge[RB_SETUP_BODY_MAX + 2];
    memset(huge, 'a', sizeof(huge) - 1);
    assert(!rb_setup_parse_form(huge, sizeof(huge) - 1, &form));
    // SSID 超过 32 字节被拒绝。
    const char *b3 = "ssid=123456789012345678901234567890123";
    assert(!rb_setup_parse_form(b3, strlen(b3), &form));

    char url[96];
    assert(rb_url_ws("192.168.2.144:8080", url, sizeof(url)) && strcmp(url, "ws://192.168.2.144:8080/device/ws") == 0);
    assert(rb_url_http("h:1", "/device/pair", url, sizeof(url)) && strcmp(url, "http://h:1/device/pair") == 0);
    assert(!rb_url_http("h:1", "device", url, sizeof(url)));
    assert(!rb_url_ws("h:1", url, 8));

    const uint8_t mac[6] = {0x00, 0x11, 0x22, 0xAA, 0xBB, 0xCC};
    char id[20];
    rb_device_id_from_mac(mac, id, sizeof(id));
    assert(strcmp(id, "rb-001122aabbcc") == 0);

    char esc[32];
    assert(rb_html_escape("a<b>&\"'", esc, sizeof(esc)) && strcmp(esc, "a&lt;b&gt;&amp;&quot;&#39;") == 0);
    assert(!rb_html_escape("<<<<<<<<", esc, 8) && esc[0] == '\0');

    assert(rb_wifi_reason_is_final(15) && rb_wifi_reason_is_final(202) && !rb_wifi_reason_is_final(8));
}

int main(void)
{
    test_events();
    test_builders();
    test_pair();
    test_setup();
    return 0;
}
