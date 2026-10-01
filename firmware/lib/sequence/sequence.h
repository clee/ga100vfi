#ifndef PWMVID_SEQUENCE_H
#define PWMVID_SEQUENCE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Absolute-period plans: portable off-board tests and the CB DMA executor.
 * The installed C6 application does not link this library. */
#define PWMVID_SEQ_MAX_STEPS 32u
typedef struct {
    uint32_t end_period; /* Exclusive, relative to device start boundary. */
    uint16_t ccr;
} pwmvid_seq_step_t;
typedef struct {
    uint16_t background_ccr, post_ccr;
    uint8_t count;
    pwmvid_seq_step_t steps[PWMVID_SEQ_MAX_STEPS];
} pwmvid_seq_plan_t;

bool pwmvid_seq_validate(const pwmvid_seq_plan_t *plan, uint16_t min_ccr,
                         uint16_t max_ccr);
/* Render consecutive CCR preload values for a prospective DMA buffer. Period
 * zero is segment zero; periods at/after the final end use post_ccr. */
bool pwmvid_seq_render(const pwmvid_seq_plan_t *plan, uint64_t first_period,
                       uint16_t *out, size_t count);
/* Standard reflected CRC-32 of canonical upload bytes:
 * version:u8=1, id:u32le, background:u16le, post:u16le, count:u8,
 * total_periods:u32le, then each {end_period:u32le, ccr:u16le}.
 * The host emits this digest for the CB SEQ COMMIT command. */
bool pwmvid_seq_crc32(const pwmvid_seq_plan_t *plan, uint32_t sequence_id,
                      uint32_t *digest);
#endif
