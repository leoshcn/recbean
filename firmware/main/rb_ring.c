// main/rb_ring.c —— ADPCM 帧环形缓冲实现。语义见 rb_ring.h。
#include "rb_ring.h"

#include <string.h>

void rb_ring_reset(rb_ring_t *ring)
{
    if (!ring) return;
    ring->head_seq = 0;
    ring->tail_seq = 0;
    ring->send_seq = 0;
    ring->overflow = false;
    // 不清零 frames：80 KB memset 没有必要，旧内容永远不会在 [tail, head) 之外被读到。
}

uint32_t rb_ring_next_seq(const rb_ring_t *ring)
{
    return ring ? ring->head_seq : 0;
}

uint32_t rb_ring_push(rb_ring_t *ring, const uint8_t *frame)
{
    if (!ring || !frame) return 0;
    if (ring->head_seq - ring->tail_seq >= RB_RING_FRAMES) {
        // 满：丢最旧帧。如果它还没发出去，说明断网已超过缓存时长。
        if (ring->send_seq == ring->tail_seq) {
            ring->overflow = true;
            ring->send_seq++;
        }
        ring->tail_seq++;
    }
    uint32_t seq = ring->head_seq;
    uint8_t *slot = ring->frames[seq % RB_RING_FRAMES];
    memcpy(slot, frame, RB_ADPCM_FRAME_BYTES);
    rb_adpcm_set_frame_seq(slot, seq);
    ring->head_seq++;
    return seq;
}

bool rb_ring_peek_unsent(const rb_ring_t *ring, uint8_t *out, uint32_t *seq)
{
    if (!ring || !out) return false;
    if (ring->head_seq == ring->send_seq) return false;
    memcpy(out, ring->frames[ring->send_seq % RB_RING_FRAMES], RB_ADPCM_FRAME_BYTES);
    if (seq) *seq = ring->send_seq;
    return true;
}

bool rb_ring_mark_sent(rb_ring_t *ring, uint32_t seq)
{
    if (!ring) return false;
    // 发送期间若 audio 任务覆盖了该帧导致 send_seq 已被推进，则不重复推进。
    if (seq != ring->send_seq || ring->send_seq == ring->head_seq) return false;
    ring->send_seq++;
    return true;
}

uint32_t rb_ring_rewind(rb_ring_t *ring, uint32_t from_seq)
{
    if (!ring) return 0;
    uint32_t span = ring->head_seq - ring->tail_seq;
    uint32_t offset = from_seq - ring->tail_seq;      // 无符号差：from 在 tail 之前时会很大
    if (offset <= span) {
        ring->send_seq = from_seq;
    } else if ((int32_t)(from_seq - ring->tail_seq) < 0) {
        ring->send_seq = ring->tail_seq;               // 早于最旧保留帧：从最旧帧开始
    } else {
        ring->send_seq = ring->head_seq;               // 晚于已写入帧：没有可重发的
    }
    return ring->send_seq;
}

uint32_t rb_ring_backlog(const rb_ring_t *ring)
{
    return ring ? ring->head_seq - ring->send_seq : 0;
}

uint32_t rb_ring_retained(const rb_ring_t *ring)
{
    return ring ? ring->head_seq - ring->tail_seq : 0;
}

uint32_t rb_ring_tail(const rb_ring_t *ring)
{
    return ring ? ring->tail_seq : 0;
}

bool rb_ring_overflowed(const rb_ring_t *ring)
{
    return ring ? ring->overflow : false;
}
