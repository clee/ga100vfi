#ifndef PWMVID_CONTROL_H
#define PWMVID_CONTROL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PWMVID_ALLOW_FIXTURE
#define PWMVID_ALLOW_FIXTURE 0
#endif
#ifndef PWMVID_USB_UPDATER
#define PWMVID_USB_UPDATER 0
#endif
#define CTRL_LINE_MAX 255u
#define CTRL_LEASE_MAX_MS 500u
#define CTRL_SETTLE_MS 50u
#define CTRL_HEALTH_MAX_AGE_MS 20u
#define CTRL_CCR_MIN 16u
#define CTRL_CCR_MAX 224u

typedef enum { PWM_DISARMED, PWM_PREARM, PWM_ARMING, PWM_ARMED, PWM_FAULT } pwm_state_t;
typedef enum {
    PWM_FAULT_NONE, PWM_FAULT_ISR, PWM_FAULT_USB, PWM_FAULT_HEALTH,
    PWM_FAULT_LEASE, PWM_FAULT_ARM, PWM_FAULT_UNEXPECTED_RUN
} pwm_fault_t;
typedef struct {
    bool usb_connected, clocks_ok, adc_valid, adc_recent, adc_plausible;
    bool gpu_capture_valid, gpu_capture_recent;
    bool break_active, comparator_fault, option_bytes_verified;
    bool pwm_source_ready, run_feedback, usb_update_ready;
    uint16_t gpu_ccr, vrm_ccr;
    uint32_t gpu_sample_ms, vrm_sample_ms;
    bool vrm_capture_valid, vrm_capture_recent;
    /* Nominal rail mV (including fitted divider), BEFORE profile gain/offset. */
    uint16_t adc_mv;
} pwm_health_t;
typedef struct {
    uint16_t requested_ccr;
    /* Informational only. JP1 CONFIG_VALID is neither driven nor read by MCU. */
    bool software_config_valid, permit, arm_request;
} pwm_outputs_t;
typedef struct {
    uint16_t mv1, ccr1, mv2, ccr2;
    uint32_t gain_ppm;
    int32_t offset_mv;
    uint16_t boot_min_mv, boot_max_mv, load_min_mv, load_max_mv;
} pwm_profile_t;
typedef struct {
    pwm_state_t state;
    pwm_fault_t fault;
    volatile bool fault_pending;
    volatile uint32_t fault_epoch;
    uint32_t acknowledged_fault_epoch;
    bool session_valid, qualified, profile_valid, target_valid;
    bool arm_request_pending, arm_request_issued, health_valid;
    bool feedback_pending, dfu_requested, sequence_mode;
    uint32_t feedback_started_ms;
    uint32_t session, last_seq, now_ms, health_ms, lease_ms, heartbeat_ms, armed_ms;
    uint16_t requested_ccr;
    pwm_profile_t profile;
    pwm_health_t health;
} pwm_control_t;

void ctrl_init(pwm_control_t *c);
pwm_outputs_t ctrl_tick(pwm_control_t *c, uint32_t now_ms, const pwm_health_t *health);
void ctrl_command(pwm_control_t *c, const char *line, char *response,
                  size_t response_size, uint32_t now_ms);
void ctrl_usb_lost(pwm_control_t *c);
/* ISR MUST drop physical gate/MOE itself before calling this minimal latch. */
void ctrl_fault_isr(pwm_control_t *c);
/* success must certify the hardware arm sequence observed RUN high. */
void ctrl_arm_result(pwm_control_t *c, bool success, uint32_t now_ms);
const char *ctrl_state_name(pwm_state_t state);
int32_t ctrl_calibrated_mv(const pwm_control_t *c);
#endif
