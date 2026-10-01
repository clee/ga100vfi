#include "sequence.h"

bool pwmvid_seq_validate(const pwmvid_seq_plan_t *plan, uint16_t min_ccr,
                         uint16_t max_ccr) {
    if (!plan || min_ccr == 0u || min_ccr > max_ccr || max_ccr >= 240u ||
        plan->count == 0u || plan->count > PWMVID_SEQ_MAX_STEPS ||
        plan->background_ccr < min_ccr || plan->background_ccr > max_ccr ||
        plan->post_ccr < min_ccr || plan->post_ccr > max_ccr) return false;
    uint32_t previous = 0u;
    for (unsigned i = 0; i < plan->count; ++i) {
        const pwmvid_seq_step_t *step = &plan->steps[i];
        if (step->end_period <= previous ||
            step->ccr < min_ccr || step->ccr > max_ccr) return false;
        previous = step->end_period;
    }
    return true;
}

bool pwmvid_seq_render(const pwmvid_seq_plan_t *plan, uint64_t first_period,
                       uint16_t *out, size_t count) {
    if (!plan || !plan->count || plan->count > PWMVID_SEQ_MAX_STEPS ||
        (!out && count) || first_period + count < first_period) return false;
    unsigned step = 0u;
    while (step < plan->count && first_period >= plan->steps[step].end_period) ++step;
    for (size_t i = 0; i < count; ++i) {
        uint64_t period = first_period + i;
        while (step < plan->count && period >= plan->steps[step].end_period) ++step;
        out[i] = step < plan->count ? plan->steps[step].ccr : plan->post_ccr;
    }
    return true;
}

static uint32_t crc_byte(uint32_t crc, uint8_t byte) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8u; ++bit)
        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    return crc;
}
static uint32_t crc_u16(uint32_t crc, uint16_t value) {
    crc = crc_byte(crc, (uint8_t)value);
    return crc_byte(crc, (uint8_t)(value >> 8));
}
static uint32_t crc_u32(uint32_t crc, uint32_t value) {
    crc = crc_u16(crc, (uint16_t)value);
    return crc_u16(crc, (uint16_t)(value >> 16));
}
bool pwmvid_seq_crc32(const pwmvid_seq_plan_t *plan, uint32_t sequence_id,
                      uint32_t *digest) {
    if (!digest || !sequence_id || !pwmvid_seq_validate(plan, 1u, 239u))
        return false;
    uint32_t crc = 0xffffffffu;
    crc = crc_byte(crc, 1u);
    crc = crc_u32(crc, sequence_id);
    crc = crc_u16(crc, plan->background_ccr);
    crc = crc_u16(crc, plan->post_ccr);
    crc = crc_byte(crc, plan->count);
    crc = crc_u32(crc, plan->steps[plan->count - 1u].end_period);
    for (unsigned i = 0; i < plan->count; ++i) {
        crc = crc_u32(crc, plan->steps[i].end_period);
        crc = crc_u16(crc, plan->steps[i].ccr);
    }
    *digest = ~crc;
    return true;
}
