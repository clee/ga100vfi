#ifndef PWMVID_HARDWARE_H
#define PWMVID_HARDWARE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "stm32g4xx_hal.h"
#define PWM_PERIOD 240u
#define PWM_MIN_CCR 16u
#define PWM_MAX_CCR 224u
#define CAPTURE_SAMPLES 128u
/* Timing candidates, not proven latency guarantees. */
#define ARM_LOW_GUARD_TICKS 128u
#define ARM_CLEAR_RELEASE_US 1000u
#define CAPTURE_STALE_MS 5u
#define ADC_STALE_MS 10u

typedef struct {
 uint32_t period_ticks, high_ticks, sample_count, high_sample_count, last_ms, errors;
 bool valid;
} capture_summary_t;
typedef struct {
 uint16_t raw_sense, raw_vref, vdda_mv, rail_mv;
 uint32_t last_ms, errors;
 bool valid;
} adc_summary_t;
void hw_early_safe(void);
void hw_drop_permission(void);
bool hw_boot_init(void);
bool hw_start_acquisition(void);
void hw_stop_acquisition(void);
void hw_poll(uint32_t now);
bool hw_clock_ok(void);
bool hw_options_ok(void);
bool hw_break_active(void);
bool hw_comp_fault(void);
bool hw_run(void);
bool hw_vbus(void);
bool hw_pwm_ready(void);
uint16_t hw_arm_diag(void);
bool hw_set_ccr(uint16_t value);
bool hw_prepare_source(void);
/* heartbeat_ms and lease_ms are the unchanged accepted host-lease origin and
 * duration, in HAL ticks. This checks expiry throughout the physical sequence
 * and reserves one tick at the strobe. A <=1 ms lease cannot produce ARM.
 * The caller must refresh control time before recording the returned result. */
bool hw_arm_pulse(uint32_t expected_usb_epoch, uint32_t heartbeat_ms, uint32_t lease_ms);
void hw_set_permit(bool permit);
void hw_led(unsigned state, unsigned fault_code, uint32_t now_ms);
void hw_watchdog_feed(void);
void hw_capture_summary(unsigned stream, capture_summary_t *s);
void hw_adc_summary(adc_summary_t *s);
size_t hw_raw(char *out, size_t capacity);
uint32_t hw_reset_flags(void);
uint32_t hw_option_word(void);
uint32_t hw_break_events(void);
uint32_t hw_threshold_code(void);
#if defined(PWMVID_SEQUENCE_BENCH) && PWMVID_SEQUENCE_BENCH
uint32_t hw_bench_fault(void);
#endif
void app_fault_isr(void);
void app_usb_lost_isr(bool had_session);
#endif
