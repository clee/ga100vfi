#include "sequence_dma.h"
#include <string.h>

static bool fail(pwmvid_seq_dma_t *q) {
    pwmvid_seq_dma_abort(q);
    return false;
}

bool pwmvid_seq_dma_init(pwmvid_seq_dma_t *q, pwmvid_seq_engine_t *e) {
    if (!q || !e || e->active) return false;
    memset(q, 0, sizeof(*q));
    q->engine = e;
    if (!pwmvid_seq_render_absolute(e, 1u, q->words,
                                    PWMVID_SEQ_DMA_WORDS)) {
        return fail(q);
    }
    q->next_unwritten_period = PWMVID_SEQ_DMA_WORDS + 1u;
    q->expect_half = true;
    return true;
}

bool pwmvid_seq_dma_event(pwmvid_seq_dma_t *q, bool half, bool full,
                          bool transfer_error) {
    if (!q || !q->engine || q->failed) return false;
    if (transfer_error || half == full || half != q->expect_half ||
        q->next_unwritten_period > UINT64_MAX - PWMVID_SEQ_DMA_HALF) {
        return fail(q);
    }
    /* At HT the 64th transfer has happened; at TC the 128th. Its CCR
     * preload has not become active until the following update, so observe
     * only the output period immediately before that transfer. */
    q->completed_transfers += PWMVID_SEQ_DMA_HALF;
    pwmvid_seq_observe(q->engine, q->completed_transfers - 1u);
    uint16_t *freed = q->words + (half ? 0u : PWMVID_SEQ_DMA_HALF);
    if (!pwmvid_seq_render_absolute(q->engine, q->next_unwritten_period,
                                    freed, PWMVID_SEQ_DMA_HALF)) {
        return fail(q);
    }
    q->next_unwritten_period += PWMVID_SEQ_DMA_HALF;
    q->expect_half = !half;
    return true;
}

uint64_t pwmvid_seq_dma_earliest(const pwmvid_seq_dma_t *q) {
    return q && !q->failed ? q->next_unwritten_period : 0u;
}

void pwmvid_seq_dma_abort(pwmvid_seq_dma_t *q) {
    if (!q) return;
    q->failed = true;
    if (q->engine) pwmvid_seq_abort(q->engine);
}
