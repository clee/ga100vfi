#ifndef PWMVID_SEQUENCE_DMA_H
#define PWMVID_SEQUENCE_DMA_H
#include "sequence_engine.h"

/* One TIM1 update DMA transfer writes the CCR1 preload for the FOLLOWING
 * output period. Each slot is tagged by that output period, not by its DMA
 * transfer number. The timer/DMA adapter owns IRQ setup and gate shutdown. */
#define PWMVID_SEQ_DMA_HALF 64u
#define PWMVID_SEQ_DMA_WORDS (2u * PWMVID_SEQ_DMA_HALF)
typedef struct {
    pwmvid_seq_engine_t *engine;
    uint16_t words[PWMVID_SEQ_DMA_WORDS];
    uint64_t next_unwritten_period;
    uint64_t completed_transfers;
    bool expect_half;
    bool failed;
} pwmvid_seq_dma_t;

/* Prime before enabling TIM1 update DMA. Period zero already has the steady
 * CCR in TIM1's active register and preload. Slots initially cover 1..128. */
bool pwmvid_seq_dma_init(pwmvid_seq_dma_t *queue, pwmvid_seq_engine_t *engine);

/* Called once for each DMA HT (half=true) or TC (half=false) event. Both or
 * out-of-order flags, transfer errors, and render failures are fatal: the
 * adapter must drop permission immediately when this returns false. */
bool pwmvid_seq_dma_event(pwmvid_seq_dma_t *queue, bool half, bool full,
                          bool transfer_error);

/* Current earliest possible device-owned start/cancel boundary. Calls to the
 * engine and this queue must be serialized against the DMA ISR. */
uint64_t pwmvid_seq_dma_earliest(const pwmvid_seq_dma_t *queue);
void pwmvid_seq_dma_abort(pwmvid_seq_dma_t *queue);
#endif
