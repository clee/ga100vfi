#include "sequence_engine.h"
#include <string.h>

void pwmvid_seq_engine_init(pwmvid_seq_engine_t *e, uint16_t background_ccr) {
    if (!e) return;
    memset(e, 0, sizeof(*e));
    e->background_ccr = background_ccr;
}
bool pwmvid_seq_set_background(pwmvid_seq_engine_t *e, uint16_t ccr,
                               uint16_t min_ccr, uint16_t max_ccr) {
    if (!e || e->active || ccr < min_ccr || ccr > max_ccr) return false;
    e->background_ccr = ccr;
    /* A staged digest includes its starting background; changing it requires
     * a fresh BEGIN/COMMIT so START never executes a stale upload. */
    e->staging = false;
    e->committed = false;
    return true;
}
bool pwmvid_seq_begin(pwmvid_seq_engine_t *e, uint32_t id, uint8_t count,
                      uint16_t post_ccr, uint32_t total_periods) {
    if (!e || !id || e->active || !count || count > PWMVID_SEQ_MAX_STEPS ||
        !total_periods) return false;
    memset(&e->staged, 0, sizeof(e->staged));
    e->staged.background_ccr = e->background_ccr;
    e->staged.post_ccr = post_ccr;
    e->staged_id = id;
    e->expected_count = count;
    e->expected_total = total_periods;
    e->added_count = 0u;
    e->staging = true;
    e->committed = false;
    return true;
}
bool pwmvid_seq_add(pwmvid_seq_engine_t *e, uint32_t id, uint8_t index,
                    uint32_t end_period, uint16_t ccr) {
    if (!e || !e->staging || e->committed || id != e->staged_id ||
        index != e->added_count || index >= e->expected_count ||
        end_period > e->expected_total ||
        end_period <= (index ? e->staged.steps[index - 1u].end_period : 0u))
        return false;
    e->staged.steps[index].end_period = end_period;
    e->staged.steps[index].ccr = ccr;
    ++e->added_count;
    return true;
}
bool pwmvid_seq_commit(pwmvid_seq_engine_t *e, uint32_t id, uint32_t digest,
                       uint16_t min_ccr, uint16_t max_ccr) {
    if (!e || !e->staging || e->committed || id != e->staged_id ||
        e->added_count != e->expected_count ||
        e->staged.steps[e->added_count - 1u].end_period != e->expected_total)
        return false;
    e->staged.count = e->added_count;
    uint32_t actual;
    if (!pwmvid_seq_validate(&e->staged, min_ccr, max_ccr) ||
        !pwmvid_seq_crc32(&e->staged, id, &actual) || actual != digest)
        return false;
    e->committed = true;
    e->staging = false;
    return true;
}
bool pwmvid_seq_start(pwmvid_seq_engine_t *e, uint32_t id,
                      uint64_t start_period, uint64_t earliest_unwritten_period) {
    if (!e || !e->committed || e->active || id != e->staged_id ||
        start_period < earliest_unwritten_period ||
        UINT64_MAX - start_period < e->expected_total) return false;
    e->running = e->staged;
    e->running_id = id;
    e->start_period = start_period;
    e->active = true;
    e->cancel_pending = false;
    e->committed = false;
    return true;
}
bool pwmvid_seq_cancel(pwmvid_seq_engine_t *e, uint32_t id,
                       uint64_t cancel_period, uint64_t earliest_unwritten_period) {
    if (!e || !e->active || e->cancel_pending || id != e->running_id ||
        cancel_period < earliest_unwritten_period ||
        cancel_period >= e->start_period +
                         e->running.steps[e->running.count - 1u].end_period)
        return false;
    e->cancel_period = cancel_period;
    e->cancel_pending = true;
    return true;
}
bool pwmvid_seq_render_absolute(const pwmvid_seq_engine_t *e,
                                uint64_t first_period, uint16_t *out, size_t count) {
    if (!e || (!out && count) || count > UINT64_MAX - first_period) return false;
    size_t i = 0u;
    while (i < count && (!e->active || first_period + i < e->start_period))
        out[i++] = e->background_ccr;
    size_t active_end = i;
    while (active_end < count &&
           (!e->cancel_pending || first_period + active_end < e->cancel_period))
        ++active_end;
    if (active_end > i &&
        !pwmvid_seq_render(&e->running, first_period + i - e->start_period,
                           out + i, active_end - i)) return false;
    while (active_end < count) out[active_end++] = e->background_ccr;
    return true;
}
void pwmvid_seq_observe(pwmvid_seq_engine_t *e, uint64_t observed_period) {
    if (!e || !e->active) return;
    if (e->cancel_pending && observed_period >= e->cancel_period) {
        e->active = false;
        e->cancel_pending = false;
    } else if (observed_period >= e->start_period +
               e->running.steps[e->running.count - 1u].end_period) {
        e->background_ccr = e->running.post_ccr;
        e->active = false;
    }
}
void pwmvid_seq_abort(pwmvid_seq_engine_t *e) {
    if (!e) return;
    e->active = false;
    e->staging = false;
    e->committed = false;
    e->cancel_pending = false;
}
