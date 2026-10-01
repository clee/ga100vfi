#include "sequence.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    pwmvid_seq_plan_t plan = {
        .background_ccr = 60, .post_ccr = 60, .count = 3,
        .steps = {{.end_period = 3, .ccr = 16},
                  {.end_period = 5, .ccr = 48},
                  {.end_period = 15, .ccr = 60}}
    };
    assert(pwmvid_seq_validate(&plan, 16, 224));
    uint16_t out[8] = {0};
    assert(pwmvid_seq_render(&plan, 1, out, 8));
    const uint16_t expected[8] = {16,16,48,48,60,60,60,60};
    for (unsigned i = 0; i < 8; ++i) assert(out[i] == expected[i]);
    assert(pwmvid_seq_render(&plan, 14, out, 8));
    for (unsigned i = 0; i < 8; ++i) assert(out[i] == 60);
    plan.steps[1].end_period = 3;
    assert(!pwmvid_seq_validate(&plan, 16, 224));
    plan.steps[1].end_period = 5;
    plan.steps[0].ccr = 2;
    assert(!pwmvid_seq_validate(&plan, 16, 224));
    assert(pwmvid_seq_validate(&plan, 2, 224));
    assert(!pwmvid_seq_render(&plan, UINT64_MAX, out, 8));
    const pwmvid_seq_plan_t example = {
        .background_ccr = 60, .post_ccr = 60, .count = 5,
        .steps = {{337500u,2u},{1150573u,60u},{1150583u,2u},
                  {1376371u,48u},{1376438u,2u}}
    };
    uint32_t digest = 0;
    assert(pwmvid_seq_crc32(&example, 1u, &digest));
    assert(digest == 1555189105u); /* Cross-language host upload fixture. */
    assert(!pwmvid_seq_crc32(&example, 0u, &digest));
    assert(pwmvid_seq_render(&example, 1150572u, out, 8u));
    const uint16_t edge_expected[8] = {60u,2u,2u,2u,2u,2u,2u,2u};
    for (unsigned i = 0; i < 8; ++i) assert(out[i] == edge_expected[i]);
    puts("Absolute-period sequence renderer passed");
    return 0;
}
