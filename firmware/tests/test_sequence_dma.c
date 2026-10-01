#include "sequence_dma.h"
#include <assert.h>
#include <stdio.h>

static void upload(pwmvid_seq_engine_t *e) {
    /* One, three, and six PWM periods: about 1.48, 4.44, and 8.89 us. */
    const uint32_t ends[] = {1u, 4u, 10u};
    const uint16_t ccrs[] = {2u, 60u, 48u};
    assert(pwmvid_seq_begin(e, 7u, 3u, 72u, 10u));
    for (unsigned i = 0; i < 3u; ++i)
        assert(pwmvid_seq_add(e, 7u, (uint8_t)i, ends[i], ccrs[i]));
    uint32_t crc;
    e->staged.count = 3u;
    assert(pwmvid_seq_crc32(&e->staged, 7u, &crc));
    e->staged.count = 0u;
    assert(pwmvid_seq_commit(e, 7u, crc, 2u, 224u));
}

static uint16_t expected(uint64_t period) {
    if (period < 129u) return 60u;
    if (period < 130u) return 2u;
    if (period < 133u) return 60u;
    if (period < 139u) return 48u;
    return 72u;
}

int main(void) {
    pwmvid_seq_engine_t e;
    pwmvid_seq_dma_t q;
    pwmvid_seq_engine_init(&e, 60u);
    assert(pwmvid_seq_dma_init(&q, &e));
    assert(pwmvid_seq_dma_earliest(&q) == 129u);
    upload(&e);
    assert(!pwmvid_seq_start(&e, 7u, 128u, pwmvid_seq_dma_earliest(&q)));
    assert(pwmvid_seq_start(&e, 7u, 129u, pwmvid_seq_dma_earliest(&q)));
    uint16_t active = 60u, preload = 60u;
    for (unsigned transfer = 0; transfer < 384u; ++transfer) {
        /* At update n, first transfer old preload to active CCR, then DMA
         * writes the next preload. The DMA word from update n takes effect
         * on output period n+1. */
        active = preload;
        preload = q.words[transfer % PWMVID_SEQ_DMA_WORDS];
        assert(active == expected(transfer));
        if ((transfer + 1u) % PWMVID_SEQ_DMA_HALF == 0u) {
            bool half = (transfer + 1u) % PWMVID_SEQ_DMA_WORDS != 0u;
            assert(pwmvid_seq_dma_event(&q, half, !half, false));
        }
    }
    assert(!e.active && e.background_ccr == 72u);
    assert(q.completed_transfers == 384u);

    pwmvid_seq_engine_init(&e, 60u);
    assert(pwmvid_seq_dma_init(&q, &e));
    upload(&e);
    assert(pwmvid_seq_start(&e, 7u, 129u, pwmvid_seq_dma_earliest(&q)));
    assert(!pwmvid_seq_dma_event(&q, true, true, false));
    assert(q.failed && !e.active && !e.committed);
    assert(!pwmvid_seq_dma_event(&q, true, false, false));
    assert(pwmvid_seq_dma_init(&q, &e));
    assert(!pwmvid_seq_dma_event(&q, false, true, false));
    assert(pwmvid_seq_dma_init(&q, &e));
    assert(!pwmvid_seq_dma_event(&q, true, false, true));
    assert(pwmvid_seq_dma_init(&q, &e));
    pwmvid_seq_dma_abort(&q);
    assert(q.failed && !pwmvid_seq_dma_earliest(&q));
    puts("DMA CCR preload boundary and fault model passed");
    return 0;
}
