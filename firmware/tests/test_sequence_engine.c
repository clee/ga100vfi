#include "sequence_engine.h"
#include <assert.h>
#include <stdio.h>

static void upload(pwmvid_seq_engine_t *e, uint32_t id, uint16_t post,
                   const uint32_t *ends, const uint16_t *ccrs,
                   uint8_t count) {
    assert(pwmvid_seq_begin(e, id, count, post, ends[count - 1u]));
    for (uint8_t i = 0; i < count; ++i)
        assert(pwmvid_seq_add(e, id, i, ends[i], ccrs[i]));
    uint32_t crc;
    e->staged.count = count;
    assert(pwmvid_seq_crc32(&e->staged, id, &crc));
    e->staged.count = 0;
    assert(!pwmvid_seq_commit(e, id, crc ^ 1u, 2u, 224u));
    assert(pwmvid_seq_commit(e, id, crc, 2u, 224u));
}
int main(void) {
    pwmvid_seq_engine_t e;
    pwmvid_seq_engine_init(&e, 60u);
    const uint32_t ends[5] = {337500u,1150573u,1150583u,1376371u,1376438u};
    const uint16_t ccrs[5] = {2u,60u,2u,48u,2u};
    upload(&e, 1u, 72u, ends, ccrs, 5u);
    assert(!pwmvid_seq_start(&e, 1u, 1005u, 1008u)); /* Already queued. */
    assert(pwmvid_seq_start(&e, 1u, 1010u, 1008u));
    uint16_t out[16];
    assert(pwmvid_seq_render_absolute(&e, 1007u, out, 8u));
    const uint16_t start_expected[8] = {60u,60u,60u,2u,2u,2u,2u,2u};
    for (unsigned i = 0; i < 8u; ++i) assert(out[i] == start_expected[i]);
    assert(pwmvid_seq_render_absolute(&e, 1010u + 1150572u, out, 13u));
    assert(out[0] == 60u && out[1] == 2u && out[10] == 2u && out[11] == 48u);
    assert(pwmvid_seq_render_absolute(&e, 1010u + 1376437u, out, 3u));
    assert(out[0] == 2u && out[1] == 72u && out[2] == 72u);
    pwmvid_seq_observe(&e, 1010u + 1376438u);
    assert(!e.active && e.background_ccr == 72u);
    assert(pwmvid_seq_render_absolute(&e, 2000000u, out, 2u));
    assert(out[0] == 72u && out[1] == 72u);

    const uint32_t short_ends[2] = {10u, 20u};
    const uint16_t short_ccrs[2] = {24u, 36u};
    upload(&e, 2u, 48u, short_ends, short_ccrs, 2u);
    assert(pwmvid_seq_start(&e, 2u, 200u, 180u));
    assert(!pwmvid_seq_cancel(&e, 2u, 205u, 210u)); /* Already queued. */
    assert(!pwmvid_seq_cancel(&e, 2u, 220u, 210u)); /* Already complete. */
    assert(pwmvid_seq_cancel(&e, 2u, 214u, 210u));
    assert(pwmvid_seq_render_absolute(&e, 208u, out, 8u));
    const uint16_t cancel_expected[8] = {24u,24u,36u,36u,36u,36u,72u,72u};
    for (unsigned i = 0; i < 8u; ++i) assert(out[i] == cancel_expected[i]);
    pwmvid_seq_observe(&e, 214u);
    assert(!e.active && e.background_ccr == 72u);
    assert(pwmvid_seq_set_background(&e, 60u, 2u, 224u));
    upload(&e, 3u, 72u, short_ends, short_ccrs, 2u);
    assert(pwmvid_seq_set_background(&e, 48u, 2u, 224u));
    assert(!pwmvid_seq_start(&e, 3u, 300u, 290u)); /* SETBG invalidated upload. */
    pwmvid_seq_abort(&e);
    assert(!pwmvid_seq_render_absolute(&e, UINT64_MAX - 1u, out, 3u));
    puts("Staged absolute-period sequence engine passed");
    return 0;
}
