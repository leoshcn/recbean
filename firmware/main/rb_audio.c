// main/rb_audio.c —— 录音采集任务与断网缓存。设计见 rb_audio.h。
#include "rb_audio.h"

#include "rb_adpcm.h"
#include "rb_ring.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "rb_audio";

// ---- 静态缓冲（无 PSRAM：全部放 .bss，启动即确定，避免运行时大块分配失败）----
static rb_ring_t s_ring;                                   // 断网缓存，≈ 80.4 KB
// 按 20 ms（320 样本，640 B）一块读取并立即编码，省掉整帧 6.4 KB 的 PCM 缓冲。
#define CHUNK_SAMPLES 320
static int16_t s_pcm[CHUNK_SAMPLES];
static uint8_t s_frame[RB_ADPCM_FRAME_BYTES];              // 编码暂存帧，1.6 KB

static SemaphoreHandle_t s_lock;        // 保护 s_ring
static SemaphoreHandle_t s_started;     // 采集任务 → 调用方：codec 打开结果已写入 s_start_err
static SemaphoreHandle_t s_stopped;     // 采集任务 → 调用方：采集循环已退出
static TaskHandle_t s_task;             // 采集任务
static TaskHandle_t s_consumer;         // 会话任务（接收新帧通知）
static volatile bool s_run;             // 采集循环是否继续；会话任务写、采集任务读
static volatile bool s_active;          // 采集循环正在运行
static esp_err_t s_start_err;
static bool s_codec_sleeping;           // codec 已 bsp_audio_sleep()，下次开始前需 wake
static rb_adpcm_state_t s_enc;          // 编码器状态，只在采集任务中使用

#define NOTIFY_START (1u << 0)

static void capture_loop(void)
{
    uint32_t frames = 0;
    while (s_run) {
        // 帧头写入本帧起始状态；seq 先占位为 0，压入时由环形缓冲改写为连续序号。
        rb_adpcm_write_header(s_frame, 0, &s_enc);
        size_t filled = 0;
        while (filled < RB_ADPCM_FRAME_SAMPLES) {
            // 阻塞约 20 ms：I2S DMA 攒够 320 个样本才返回。
            esp_err_t e = bsp_audio_read(s_pcm, sizeof(s_pcm));
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "bsp_audio_read 失败: %s", esp_err_to_name(e));
                vTaskDelay(pdMS_TO_TICKS(20));
                if (!s_run) break;
                continue;
            }
            rb_adpcm_encode_into(&s_enc, s_pcm, CHUNK_SAMPLES, s_frame, filled);
            filled += CHUNK_SAMPLES;
        }
        if (filled < RB_ADPCM_FRAME_SAMPLES) break;   // 停止时丢弃不完整的最后一帧
        xSemaphoreTake(s_lock, portMAX_DELAY);
        rb_ring_push(&s_ring, s_frame);
        xSemaphoreGive(s_lock);
        if (s_consumer) xTaskNotify(s_consumer, RB_AUDIO_NOTIFY_BIT, eSetBits);
        frames++;
    }
    ESP_LOGI(TAG, "采集停止，共 %lu 帧", (unsigned long)frames);
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        if (!(bits & NOTIFY_START)) continue;

        esp_err_t e = ESP_OK;
        if (s_codec_sleeping) {
            e = bsp_audio_wake();
            if (e == ESP_OK) s_codec_sleeping = false;
        }
        if (e == ESP_OK) e = bsp_audio_set_format(RB_ADPCM_SAMPLE_RATE, 16, 1);
        s_start_err = e;
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "codec 打开失败: %s", esp_err_to_name(e));
            s_run = false;
            xSemaphoreGive(s_started);
            continue;
        }
        rb_adpcm_reset(&s_enc);
        s_active = true;
        xSemaphoreGive(s_started);
        capture_loop();
        // 录音结束：codec 进入低功耗，省电（下次开始时 wake）。
        if (bsp_audio_sleep() == ESP_OK) s_codec_sleeping = true;
        s_active = false;
        xSemaphoreGive(s_stopped);
    }
}

esp_err_t rb_audio_init(TaskHandle_t consumer)
{
    if (s_task) return ESP_OK;
    s_consumer = consumer;
    s_lock = xSemaphoreCreateMutex();
    s_started = xSemaphoreCreateBinary();
    s_stopped = xSemaphoreCreateBinary();
    if (!s_lock || !s_started || !s_stopped) return ESP_ERR_NO_MEM;
    rb_ring_reset(&s_ring);
    // 待机时 codec 不需要工作：开机先让 ES8311 进入低功耗，开始录音时再唤醒。
    if (bsp_audio_sleep() == ESP_OK) s_codec_sleeping = true;
    ESP_LOGI(TAG, "断网缓存 %d 帧 × %d B = %u B（≈ %d s）", RB_RING_FRAMES, RB_ADPCM_FRAME_BYTES,
             (unsigned)sizeof(s_ring.frames), RB_RING_FRAMES * RB_ADPCM_FRAME_MS / 1000);
    // 3 KB 栈：采集循环只调用 BSP 读和编码，缓冲都在 .bss。
    if (xTaskCreate(audio_task, "rb_audio", 3072, NULL, 6, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t rb_audio_start(void)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    if (s_active) return ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    rb_ring_reset(&s_ring);
    xSemaphoreGive(s_lock);
    (void)xSemaphoreTake(s_started, 0);   // 清掉可能残留的信号
    (void)xSemaphoreTake(s_stopped, 0);
    s_run = true;
    xTaskNotify(s_task, NOTIFY_START, eSetBits);
    if (xSemaphoreTake(s_started, pdMS_TO_TICKS(1000)) != pdTRUE) {
        s_run = false;
        return ESP_ERR_TIMEOUT;
    }
    return s_start_err;
}

void rb_audio_stop(void)
{
    if (!s_active) {
        s_run = false;
        return;
    }
    s_run = false;
    // 最后一次读取最多 200 ms；留余量。超时也不卡死会话任务。
    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(600)) != pdTRUE) {
        ESP_LOGW(TAG, "等待采集停止超时");
    }
}

bool rb_audio_peek(uint8_t *out_frame, uint32_t *seq)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = rb_ring_peek_unsent(&s_ring, out_frame, seq);
    xSemaphoreGive(s_lock);
    return ok;
}

void rb_audio_mark_sent(uint32_t seq)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    (void)rb_ring_mark_sent(&s_ring, seq);
    xSemaphoreGive(s_lock);
}

uint32_t rb_audio_rewind(uint32_t from_seq)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t v = rb_ring_rewind(&s_ring, from_seq);
    xSemaphoreGive(s_lock);
    return v;
}

uint32_t rb_audio_tail(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t v = rb_ring_tail(&s_ring);
    xSemaphoreGive(s_lock);
    return v;
}

uint32_t rb_audio_backlog(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t v = rb_ring_backlog(&s_ring);
    xSemaphoreGive(s_lock);
    return v;
}

bool rb_audio_overflowed(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = rb_ring_overflowed(&s_ring);
    xSemaphoreGive(s_lock);
    return v;
}
