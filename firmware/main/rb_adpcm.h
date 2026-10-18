// main/rb_adpcm.h —— IMA-ADPCM 帧编解码（纯 C，无 ESP-IDF/LVGL 依赖，可主机测试）。
//
// 帧格式与设备↔服务协议 v1（design.md §3）一致，每帧 200 ms：
//
//   偏移  长度  字段
//   0     4     seq          uint32 小端，帧序号（从 0 递增，断线续传/去重用）
//   4     2     predictor    int16  小端，编码本帧第一个样本【之前】的预测值
//   6     1     step_index   uint8  编码本帧第一个样本【之前】的步长索引（0..88）
//   7     1     reserved     固定 0
//   8     1600  data         3200 个 4 bit 码字；每字节先低 4 位（较早样本）后高 4 位
//
// 头部携带帧起始状态，因此每帧可独立解码：解码器以头部 predictor/step_index
// 为初值依次解出 3200 个样本（头部的 predictor 本身【不】作为输出样本）。
// 量化表/步长表为标准 IMA（与 WAV IMA-ADPCM、ffmpeg adpcm_ima_wav 一致）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RB_ADPCM_SAMPLE_RATE   16000                     // 采样率 Hz（协议固定）
#define RB_ADPCM_FRAME_MS      200                       // 每帧时长 ms
#define RB_ADPCM_FRAME_SAMPLES 3200                      // 每帧样本数 = 16000 × 0.2
#define RB_ADPCM_HEADER_BYTES  8                         // seq4 + predictor2 + index1 + 保留1
#define RB_ADPCM_DATA_BYTES    (RB_ADPCM_FRAME_SAMPLES / 2)   // 1600：每样本 4 bit
#define RB_ADPCM_FRAME_BYTES   (RB_ADPCM_HEADER_BYTES + RB_ADPCM_DATA_BYTES) // 1608

// 编解码器跨样本的状态。编码器在帧之间延续该状态（音质更好），
// 并把每帧起始时的状态写进帧头，保证帧独立可解码。
typedef struct {
    int16_t predictor;   // 当前预测值（上一个重建样本），-32768..32767
    uint8_t step_index;  // 当前步长索引，0..88
} rb_adpcm_state_t;

// 把状态复位为 predictor=0, step_index=0。每次新录音开始时调用一次。
void rb_adpcm_reset(rb_adpcm_state_t *state);

// 编码一个 4 bit 码字，并就地更新状态。返回 0..15 的码字。
uint8_t rb_adpcm_encode_sample(rb_adpcm_state_t *state, int16_t sample);

// 解码一个 4 bit 码字（只取低 4 位），并就地更新状态。返回重建样本。
int16_t rb_adpcm_decode_sample(rb_adpcm_state_t *state, uint8_t code);

// 编码完整一帧（RB_ADPCM_FRAME_SAMPLES 个样本）到 out（RB_ADPCM_FRAME_BYTES 字节）。
// 参数：
//   seq   —— 写入帧头的序号
//   pcm   —— 3200 个 16 bit 单声道样本，不得为 NULL
//   state —— 进入时为帧起始状态（写入帧头）；返回时为帧末状态，供下一帧继续
//   out   —— 输出缓冲，至少 RB_ADPCM_FRAME_BYTES 字节，由调用方持有
// 纯计算、不阻塞、可在任意任务调用；参数为 NULL 时不做任何事。
void rb_adpcm_encode_frame(uint32_t seq, const int16_t *pcm,
                           rb_adpcm_state_t *state, uint8_t *out);

// 分块编码（采集任务按 20 ms 小块读 PCM，省去 6.4 KB 的整帧 PCM 缓冲）：
//   1. rb_adpcm_write_header(frame, seq, state)  —— 用帧起始状态写帧头；
//   2. 多次 rb_adpcm_encode_into(state, pcm, n, frame, offset) —— n 为偶数，
//      offset 为本块第一个样本在帧内的序号（偶数），offset + n ≤ RB_ADPCM_FRAME_SAMPLES。
// 结果与一次性 rb_adpcm_encode_frame() 逐字节相同（主机测试覆盖）。
void rb_adpcm_write_header(uint8_t *frame, uint32_t seq, const rb_adpcm_state_t *state);
void rb_adpcm_encode_into(rb_adpcm_state_t *state, const int16_t *pcm, size_t n_samples,
                          uint8_t *frame, size_t sample_offset);

// 解码一帧。frame_len 必须恰为 RB_ADPCM_FRAME_BYTES，否则返回 false。
// 成功时 pcm 写入 3200 个样本，*seq（可为 NULL）写入帧序号。
// 帧头 step_index > 88 或保留字节非 0 视为损坏帧，返回 false。
bool rb_adpcm_decode_frame(const uint8_t *frame, size_t frame_len,
                           int16_t *pcm, uint32_t *seq);

// 只读帧头中的 seq（frame 至少 4 字节）。
uint32_t rb_adpcm_frame_seq(const uint8_t *frame);

// 改写帧头中的 seq（frame 至少 4 字节）。环形缓冲按入队顺序分配序号时使用。
void rb_adpcm_set_frame_seq(uint8_t *frame, uint32_t seq);
