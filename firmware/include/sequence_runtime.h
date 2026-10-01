#ifndef PWMVID_SEQUENCE_RUNTIME_H
#define PWMVID_SEQUENCE_RUNTIME_H
#include "control.h"
#include "hardware.h"

/* STM32G431CB timed-output adapter. All application calls run in foreground;
 * only the DMA IRQ refills the CCR preload ring. */
void seq_runtime_init(void);
bool seq_runtime_arm(uint16_t background_ccr);
void seq_runtime_abort(void);
void seq_runtime_abort_isr(void);
bool seq_runtime_owns_ccr(void);
void seq_runtime_poll(pwm_control_t *control, uint32_t now_ms,
                      const capture_summary_t *vrm);
void seq_runtime_command(pwm_control_t *control, char **tokens, size_t count,
                         char *reply, size_t capacity, uint32_t now_ms);
void seq_runtime_telemetry(char *out, size_t capacity);
#if defined(PWMVID_SEQUENCE_BENCH) && PWMVID_SEQUENCE_BENCH
void seq_runtime_bench_diag(uint32_t *reason, uint32_t *flags,
                            uint32_t *left, uint32_t *events);
void seq_runtime_bench_feedback(uint32_t *reason, uint32_t *period,
                                uint32_t *step, uint32_t *target,
                                uint32_t *measured, uint32_t *samples,
                                uint32_t *timer_ccr, uint32_t *dma_word,
                                uint32_t *engine_active);
#endif
#endif
