// tests/test_rb_ring.c —— 断网缓存环形缓冲主机测试：序号分配、发送推进、
// 断线回退重发（resume）、溢出判定。
#include <assert.h>
#include <string.h>

#include "rb_ring.h"

static rb_ring_t s_ring;   // 约 80 KB，放静态区，与设备端一致

static void make_frame(uint8_t *frame, uint8_t fill)
{
    memset(frame, fill, RB_ADPCM_FRAME_BYTES);
    frame[7] = 0;
}

int main(void)
{
    static uint8_t frame[RB_ADPCM_FRAME_BYTES];
    static uint8_t out[RB_ADPCM_FRAME_BYTES];
    uint32_t seq = 99;
    assert(sizeof(s_ring.frames) == (size_t)RB_RING_FRAMES * 1608u);

    rb_ring_reset(&s_ring);
    assert(rb_ring_next_seq(&s_ring) == 0);
    assert(!rb_ring_peek_unsent(&s_ring, out, &seq));
    assert(rb_ring_backlog(&s_ring) == 0);

    // 压入 3 帧：序号被改写为 0,1,2（无论帧头原来是什么）。
    for (uint8_t i = 0; i < 3; i++) {
        make_frame(frame, (uint8_t)(0x10 + i));
        assert(rb_ring_push(&s_ring, frame) == i);
    }
    assert(rb_ring_backlog(&s_ring) == 3 && rb_ring_retained(&s_ring) == 3);

    // 正常发送：peek 不推进，mark_sent 才推进；错的 seq 不推进。
    assert(rb_ring_peek_unsent(&s_ring, out, &seq) && seq == 0);
    assert(rb_adpcm_frame_seq(out) == 0 && out[8] == 0x10);
    assert(!rb_ring_mark_sent(&s_ring, 1));
    assert(rb_ring_mark_sent(&s_ring, 0));
    assert(rb_ring_peek_unsent(&s_ring, out, &seq) && seq == 1 && out[8] == 0x11);
    assert(rb_ring_mark_sent(&s_ring, 1));
    assert(rb_ring_mark_sent(&s_ring, 2));
    assert(!rb_ring_mark_sent(&s_ring, 3));
    assert(rb_ring_backlog(&s_ring) == 0);
    assert(!rb_ring_peek_unsent(&s_ring, out, &seq));

    // 断线：回退到服务端给的 next_seq=1，从 1 开始重发。
    assert(rb_ring_rewind(&s_ring, 1) == 1);
    assert(rb_ring_backlog(&s_ring) == 2);
    assert(rb_ring_peek_unsent(&s_ring, out, &seq) && seq == 1);
    // 服务端声称收到了未来的帧：夹到 head。
    assert(rb_ring_rewind(&s_ring, 50) == 3);
    assert(rb_ring_backlog(&s_ring) == 0);

    // 断网期间持续写满：全部已发送过的帧被覆盖不算溢出。
    rb_ring_reset(&s_ring);
    for (uint32_t i = 0; i < RB_RING_FRAMES; i++) {
        make_frame(frame, (uint8_t)i);
        rb_ring_push(&s_ring, frame);
        assert(rb_ring_mark_sent(&s_ring, i));
    }
    // 链路断开（send_seq == head）。再写 RB_RING_FRAMES 帧：被挤掉的都是已发送帧。
    for (uint32_t i = 0; i < RB_RING_FRAMES; i++) {
        make_frame(frame, (uint8_t)(i + 1));
        rb_ring_push(&s_ring, frame);
        assert(!rb_ring_overflowed(&s_ring));
    }
    assert(rb_ring_backlog(&s_ring) == RB_RING_FRAMES);
    assert(rb_ring_tail(&s_ring) == RB_RING_FRAMES);
    // 重连后若服务端要求的帧已被丢弃（早于 tail），夹到 tail。
    assert(rb_ring_rewind(&s_ring, 3) == RB_RING_FRAMES);
    assert(rb_ring_peek_unsent(&s_ring, out, &seq) && seq == RB_RING_FRAMES);
    assert(rb_adpcm_frame_seq(out) == RB_RING_FRAMES);
    // 再多写一帧：未发送的最旧帧被覆盖 → 溢出（断网超过缓存时长）。
    make_frame(frame, 0xEE);
    assert(rb_ring_push(&s_ring, frame) == 2 * RB_RING_FRAMES);
    assert(rb_ring_overflowed(&s_ring));
    assert(rb_ring_backlog(&s_ring) == RB_RING_FRAMES);
    assert(rb_ring_peek_unsent(&s_ring, out, &seq) && seq == RB_RING_FRAMES + 1);
    // 发送进行中该帧被覆盖：mark_sent 旧 seq 不得重复推进。
    assert(!rb_ring_mark_sent(&s_ring, RB_RING_FRAMES));

    // reset 清除溢出并从 0 开始。
    rb_ring_reset(&s_ring);
    assert(!rb_ring_overflowed(&s_ring) && rb_ring_next_seq(&s_ring) == 0);

    // 序号回绕：从接近 UINT32_MAX 开始依然正确。
    s_ring.head_seq = s_ring.tail_seq = s_ring.send_seq = 0xFFFFFFFEu;
    for (int i = 0; i < 4; i++) {
        make_frame(frame, (uint8_t)i);
        rb_ring_push(&s_ring, frame);
    }
    assert(rb_ring_backlog(&s_ring) == 4 && rb_ring_retained(&s_ring) == 4);
    assert(rb_ring_rewind(&s_ring, 0xFFFFFFFFu) == 0xFFFFFFFFu);
    assert(rb_ring_backlog(&s_ring) == 3);
    assert(rb_ring_rewind(&s_ring, 0xFFFFFF00u) == 0xFFFFFFFEu);
    return 0;
}
