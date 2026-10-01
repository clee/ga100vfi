#ifndef PWMVID_SEQUENCE_ENGINE_H
#define PWMVID_SEQUENCE_ENGINE_H
#include "sequence.h"

/* Portable, allocation-free staging and absolute-period output model.
 * The CB timer/DMA adapter supplies period numbers and buffer alignment;
 * the STM32G431CB application links this model. */
typedef struct {
    pwmvid_seq_plan_t staged, running;
    uint32_t staged_id, running_id, expected_total;
    uint64_t start_period, cancel_period;
    uint16_t background_ccr;
    uint8_t expected_count, added_count;
    bool staging, committed, active, cancel_pending;
} pwmvid_seq_engine_t;

void pwmvid_seq_engine_init(pwmvid_seq_engine_t *engine, uint16_t background_ccr);
bool pwmvid_seq_set_background(pwmvid_seq_engine_t *engine, uint16_t ccr,
                               uint16_t min_ccr, uint16_t max_ccr);
bool pwmvid_seq_begin(pwmvid_seq_engine_t *engine, uint32_t id, uint8_t count,
                      uint16_t post_ccr, uint32_t total_periods);
bool pwmvid_seq_add(pwmvid_seq_engine_t *engine, uint32_t id, uint8_t index,
                    uint32_t end_period, uint16_t ccr);
bool pwmvid_seq_commit(pwmvid_seq_engine_t *engine, uint32_t id, uint32_t digest,
                       uint16_t min_ccr, uint16_t max_ccr);
/* earliest_unwritten_period is provided by the DMA layer; START may not alter
 * a period already queued into DMA. The chosen start is device-owned. */
bool pwmvid_seq_start(pwmvid_seq_engine_t *engine, uint32_t id,
                      uint64_t start_period, uint64_t earliest_unwritten_period);
bool pwmvid_seq_cancel(pwmvid_seq_engine_t *engine, uint32_t id,
                       uint64_t cancel_period, uint64_t earliest_unwritten_period);
bool pwmvid_seq_render_absolute(const pwmvid_seq_engine_t *engine,
                                uint64_t first_period, uint16_t *out, size_t count);
/* Call with the last period whose output has actually been observed by the
 * timer/DMA layer. Completion or cancellation updates the steady background. */
void pwmvid_seq_observe(pwmvid_seq_engine_t *engine, uint64_t observed_period);
void pwmvid_seq_abort(pwmvid_seq_engine_t *engine);
#endif
