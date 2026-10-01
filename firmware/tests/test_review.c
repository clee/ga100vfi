/* Independent boundary/regression review. Synthetic inputs, not calibration. */
#include "control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char answer[256];
static pwm_health_t healthy(uint32_t now) {
 pwm_health_t h={0};
 h.usb_connected=h.clocks_ok=h.adc_valid=h.adc_recent=h.adc_plausible=true;
 h.gpu_capture_valid=h.gpu_capture_recent=h.option_bytes_verified=h.pwm_source_ready=true;
 h.vrm_capture_valid=h.vrm_capture_recent=true;
 h.gpu_ccr=h.vrm_ccr=64;h.gpu_sample_ms=h.vrm_sample_ms=now;h.adc_mv=800;
 return h;
}
static void command(pwm_control_t *c,uint32_t now,const char *verb) {
 char line[256];snprintf(line,sizeof line,"%lu %lu %s",(unsigned long)c->session,(unsigned long)(c->last_seq+1),verb);
 ctrl_command(c,line,answer,sizeof answer,now);
}
static void ok(pwm_control_t *c,uint32_t now,const char *verb) {
 command(c,now,verb); if(strncmp(answer,"OK ",3)) fprintf(stderr,"%s: %s\n",verb,answer);assert(!strncmp(answer,"OK ",3));
}
static void prearm(pwm_control_t *c,uint32_t now) {
 pwm_health_t h=healthy(now);ctrl_init(c);ctrl_tick(c,now,&h);
 ctrl_command(c,"HELLO 77",answer,sizeof answer,now);
 ok(c,now,"PROFILE 200 16 1100 224 1000000 0 700 900 100 1200");
 ok(c,now,"QUALIFY FIXTURE_ISOLATED");ok(c,now,"PREARM");
 assert(c->state==PWM_PREARM && !ctrl_tick(c,now,&h).permit);
}
static void armed(pwm_control_t *c,uint32_t now) {
 prearm(c,now);ok(c,now,"ARM 300");
 pwm_health_t h=healthy(now);assert(ctrl_tick(c,now,&h).arm_request);
 ctrl_arm_result(c,true,now);assert(c->state==PWM_ARMED);
}
static void lease_boundary(void) {
 pwm_control_t c;prearm(&c,100);ok(&c,100,"ARM 10");pwm_health_t h=healthy(100);
 assert(ctrl_tick(&c,100,&h).arm_request);ctrl_arm_result(&c,true,110);
 assert(c.state==PWM_FAULT && c.fault==PWM_FAULT_LEASE);
 prearm(&c,UINT32_MAX-5u);ok(&c,UINT32_MAX-5u,"ARM 10");h=healthy(UINT32_MAX-5u);
 assert(ctrl_tick(&c,UINT32_MAX-5u,&h).arm_request);ctrl_arm_result(&c,true,3);
 assert(c.state==PWM_ARMED);h=healthy(4);h.run_feedback=true;
 assert(!ctrl_tick(&c,4,&h).permit && c.fault==PWM_FAULT_LEASE);
}
static void fault_epoch_survives_boolean_loss(void) {
 pwm_control_t c;armed(&c,100);ctrl_fault_isr(&c);c.fault_pending=false;
 pwm_health_t h=healthy(101);h.run_feedback=true;
 assert(!ctrl_tick(&c,101,&h).permit && c.state==PWM_FAULT);
 h.run_feedback=false;ctrl_tick(&c,102,&h);ok(&c,102,"RESET");
 assert(c.state==PWM_DISARMED && c.acknowledged_fault_epoch==c.fault_epoch);
 ctrl_fault_isr(&c);c.fault_pending=false;
 assert(!ctrl_tick(&c,103,&h).permit && c.state==PWM_FAULT);
}
static void nonextendable_target(void) {
 pwm_control_t c;armed(&c,100);pwm_health_t h=healthy(160);h.run_feedback=true;
 assert(ctrl_tick(&c,160,&h).permit);ok(&c,160,"SETCCR 100");
 command(&c,161,"SETCCR 110");assert(!strcmp(answer,"ERR TARGET_PENDING"));
 assert(c.feedback_started_ms==160 && c.requested_ccr==100);
 h.vrm_sample_ms=161; /* valid waveform, still wrong duty */
 assert(ctrl_tick(&c,161,&h).permit && c.feedback_pending);
 assert(!ctrl_tick(&c,163,&h).permit && c.state==PWM_FAULT);
}
static void fresh_feedback_required(void) {
 pwm_control_t c;armed(&c,100);pwm_health_t h=healthy(160);h.run_feedback=true;
 ctrl_tick(&c,160,&h);ok(&c,160,"SETCCR 100");h.vrm_ccr=100;h.vrm_sample_ms=160;
 ctrl_tick(&c,160,&h);assert(c.feedback_pending);
 h.vrm_sample_ms=161;assert(ctrl_tick(&c,161,&h).permit && !c.feedback_pending);
 ok(&c,161,"SETCCR 120");assert(c.feedback_started_ms==161);
}
static void late_feedback_cannot_recover(void) {
 pwm_control_t c;armed(&c,100);pwm_health_t h=healthy(160);h.run_feedback=true;
 ctrl_tick(&c,160,&h);ok(&c,160,"SETCCR 100");
 h.vrm_ccr=100;h.vrm_sample_ms=163;
 assert(!ctrl_tick(&c,163,&h).permit && c.state==PWM_FAULT);
}
static void settled_mismatch_faults(void) {
 pwm_control_t c;armed(&c,100);pwm_health_t h=healthy(160);h.run_feedback=true;
 h.vrm_ccr=200;
 assert(!ctrl_tick(&c,160,&h).permit && c.state==PWM_FAULT);
}
static void unqualified_options_reset_stays_passive(void) {
 pwm_control_t c;prearm(&c,100);pwm_health_t h=healthy(101);h.option_bytes_verified=false;
 ctrl_fault_isr(&c);ctrl_tick(&c,101,&h);ok(&c,101,"RESET");
 assert(c.state==PWM_DISARMED && !c.qualified && !c.target_valid);
 assert(!ctrl_tick(&c,102,&h).permit);
 ok(&c,102,"QUALIFY FIXTURE_ISOLATED");ok(&c,102,"PREARM");
 command(&c,102,"ARM 100");assert(!strcmp(answer,"ERR HEALTH"));
 assert(!ctrl_tick(&c,102,&h).permit);
}
static void initial_duty_window(void) {
 pwm_control_t c;prearm(&c,100);pwm_health_t h=healthy(101);h.gpu_ccr=120;
 ctrl_tick(&c,101,&h);command(&c,101,"ARM 100");
 assert(!strcmp(answer,"ERR ARM_LOW_PHASE_WINDOW") && c.state==PWM_PREARM);
}
int main(void) {
#if PWMVID_ALLOW_FIXTURE != 1
#error "Independent active-control regressions require PWMVID_ALLOW_FIXTURE=1"
#endif
 lease_boundary();fault_epoch_survives_boolean_loss();nonextendable_target();fresh_feedback_required();late_feedback_cannot_recover();settled_mismatch_faults();unqualified_options_reset_stays_passive();initial_duty_window();
 puts("Independent review boundary tests passed");return 0;
}
