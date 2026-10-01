#include "hardware.h"
#include "usb_transport.h"
#include "control.h"
#include "boot_contract.h"
#include "text_format.h"
#if defined(PWMVID_SEQUENCE) && PWMVID_SEQUENCE
#include "sequence_runtime.h"
#endif
#include <string.h>

#ifndef PWMVID_INTERNAL_SOURCE_SELFTEST
#define PWMVID_INTERNAL_SOURCE_SELFTEST 0
#endif
#ifndef PWMVID_SEQUENCE_BENCH
#define PWMVID_SEQUENCE_BENCH 0
#endif
#if PWMVID_INTERNAL_SOURCE_SELFTEST && !PWMVID_ALLOW_FIXTURE
#error Isolated output self-test requires fixture hardware path
#endif
#if PWMVID_SEQUENCE_BENCH && (!PWMVID_ALLOW_FIXTURE || !defined(PWMVID_SEQUENCE) || !PWMVID_SEQUENCE)
#error CB sequencer bench requires the CB fixture and sequence hardware path
#endif

static pwm_control_t control;
static volatile bool usb_loss_pending;
static volatile bool fatal_led;
static bool acquisition_active;
static char line[256], response[768];
static uint32_t last_tick, dfu_requested_ms;
static bool dfu_waiting;


void app_fault_isr(void) {
 hw_drop_permission();
 ctrl_fault_isr(&control);
}
void app_usb_lost_isr(bool had_session) {
 hw_drop_permission();
 if(had_session) {
  usb_loss_pending=true;
  ctrl_fault_isr(&control);
 }
}
static void fail_stop(void) {
 hw_drop_permission();
 fatal_led=true;
 hw_led((unsigned)PWM_FAULT, 7u, HAL_GetTick());
 /* Deliberately do not refresh IWDG after unrecoverable initialization fault. */
 for(;;){__WFI();}
}
void SysTick_Handler(void) {
 HAL_IncTick();
 if(fatal_led)hw_led((unsigned)PWM_FAULT,7u,HAL_GetTick());
}
void NMI_Handler(void) {
 hw_drop_permission();ctrl_fault_isr(&control);
 if(RCC->CIFR&RCC_CIFR_CSSF)RCC->CICR=RCC_CICR_CSSC;
 fail_stop();
}
void HardFault_Handler(void){fail_stop();}
void MemManage_Handler(void){fail_stop();}
void BusFault_Handler(void){fail_stop();}
void UsageFault_Handler(void){fail_stop();}

static pwm_health_t health(uint32_t *time_ms) {
 capture_summary_t gpu,vrm;adc_summary_t a;
 hw_capture_summary(0,&gpu);hw_capture_summary(1,&vrm);hw_adc_summary(&a);
 uint32_t now=HAL_GetTick(); *time_ms=now; /* Time follows all ISR-owned snapshots. */
 pwm_health_t h={0};
 h.usb_connected=usb_transport_connected() && hw_vbus();
 h.clocks_ok=hw_clock_ok();h.adc_valid=a.valid;h.adc_recent=(uint32_t)(now-a.last_ms)<=ADC_STALE_MS;
 h.adc_plausible=a.rail_mv>=100u && a.rail_mv<=1250u;h.adc_mv=a.rail_mv;
 h.gpu_capture_valid=gpu.valid;h.gpu_capture_recent=(uint32_t)(now-gpu.last_ms)<=CAPTURE_STALE_MS;
 /* Never extend a matching grace on each target. Control owns one bounded
  * transaction; raw capture validity here remains independent of target. */
 h.gpu_ccr=gpu.period_ticks ? (uint16_t)((gpu.high_ticks*PWM_PERIOD+gpu.period_ticks/2u)/gpu.period_ticks) : 0;
#if PWMVID_INTERNAL_SOURCE_SELFTEST || PWMVID_SEQUENCE_BENCH
 /* Isolated-board experiment only: synthesize a fixed 64/240 capture so the
  * single Hantek source can supply 0.8 V sense instead. Presence of any real
  * GPU capture blocks the synthetic source; this image must be removed after
  * the output check. The real rail, break, RUN and VRM feedback remain live. */
 if(gpu.valid && (uint32_t)(now-gpu.last_ms)<=CAPTURE_STALE_MS) {
  h.gpu_capture_valid=false;h.gpu_capture_recent=false;
 } else {
  h.gpu_capture_valid=true;h.gpu_capture_recent=true;
  h.gpu_ccr=64u;
 }
#endif
 h.vrm_ccr=vrm.period_ticks ? (uint16_t)((vrm.high_ticks*PWM_PERIOD+vrm.period_ticks/2u)/vrm.period_ticks) : 0;
 h.gpu_sample_ms=gpu.last_ms;h.vrm_sample_ms=vrm.last_ms;
#if PWMVID_INTERNAL_SOURCE_SELFTEST || PWMVID_SEQUENCE_BENCH
 if(h.gpu_capture_valid)h.gpu_sample_ms=now;
#endif
 h.vrm_capture_valid=vrm.valid;
 h.vrm_capture_recent=(uint32_t)(now-vrm.last_ms)<=CAPTURE_STALE_MS;
 h.break_active=hw_break_active();h.comparator_fault=hw_comp_fault();h.option_bytes_verified=hw_options_ok();
 h.pwm_source_ready=hw_pwm_ready();h.run_feedback=hw_run();
 h.usb_update_ready=(FLASH->OPTR&FLASH_OPTR_IWDG_SW)!=0u;
 return h;
}
static void source_mode(void) {
 if(control.state==PWM_DISARMED || control.state==PWM_FAULT) hw_drop_permission();
 else if(control.state==PWM_PREARM && !hw_run() && acquisition_active && !hw_pwm_ready()) {
  hw_set_permit(false);
  (void)hw_set_ccr(control.requested_ccr);
  (void)hw_prepare_source(); /* Threshold startup may still be settling; ARM requires ready. */
 }
}
static void diagnostics(void) {
#if PWMVID_SEQUENCE_BENCH
 char selftest_suffix[288]="";
#else
 char selftest_suffix[64]="";
#endif
#if defined(PWMVID_SEQUENCE) && PWMVID_SEQUENCE
#if PWMVID_SEQUENCE_BENCH
 char sequence_suffix[160];seq_runtime_telemetry(sequence_suffix,sizeof(sequence_suffix));
#else
 char sequence_suffix[128];seq_runtime_telemetry(sequence_suffix,sizeof(sequence_suffix));
#endif
#else
 const char *sequence_suffix="";
#endif
#if PWMVID_INTERNAL_SOURCE_SELFTEST || PWMVID_SEQUENCE_BENCH
 uint16_t arm_diag=hw_arm_diag();
#if PWMVID_SEQUENCE_BENCH
 uint32_t dma_reason,dma_flags,dma_left,dma_events;
 uint32_t feedback_reason,feedback_period,feedback_step,feedback_target,feedback_measured,feedback_samples;
 uint32_t feedback_timer_ccr,feedback_dma_word,feedback_engine_active;
 seq_runtime_bench_diag(&dma_reason,&dma_flags,&dma_left,&dma_events);
 seq_runtime_bench_feedback(&feedback_reason,&feedback_period,&feedback_step,
                            &feedback_target,&feedback_measured,&feedback_samples,
                            &feedback_timer_ccr,&feedback_dma_word,&feedback_engine_active);
 pwm_snprintf(selftest_suffix,sizeof(selftest_suffix),
              " selftest=1 arm_stage=%u arm_post=%u hwf=%lu dmaf=%lu dflags=%lu dleft=%lu devents=%lu sfb=%lu sfp=%lu sfi=%lu sft=%lu sfm=%lu sfn=%lu sfc=%lu sfw=%lu sfa=%lu",
              (unsigned)(arm_diag&255u),(unsigned)(arm_diag>>8),
              (unsigned long)hw_bench_fault(),(unsigned long)dma_reason,
              (unsigned long)dma_flags,(unsigned long)dma_left,(unsigned long)dma_events,
              (unsigned long)feedback_reason,(unsigned long)feedback_period,
              (unsigned long)feedback_step,(unsigned long)feedback_target,
              (unsigned long)feedback_measured,(unsigned long)feedback_samples,
              (unsigned long)feedback_timer_ccr,(unsigned long)feedback_dma_word,
              (unsigned long)feedback_engine_active);
#else
 pwm_snprintf(selftest_suffix,sizeof(selftest_suffix),
              " selftest=1 arm_stage=%u arm_post=%u",
              (unsigned)(arm_diag&255u),(unsigned)(arm_diag>>8));
#endif
#endif
 capture_summary_t g,v;adc_summary_t a;hw_capture_summary(0,&g);hw_capture_summary(1,&v);hw_adc_summary(&a);
 uint32_t now=HAL_GetTick();
 pwm_snprintf(response,sizeof(response),
  "OK TELEM ms=%lu state=%s rev=%04lx optr=%08lx reset=%08lx ccr=%u adc=%u vref=%u vdda_mv=%u rail_mv=%u cal_mv=%ld adc_ok=%u adc_ms=%lu gpu_ms=%lu vrm_ms=%lu preview=RECONSTRUCTED source_ready=%u gpu_p=%lu gpu_h=%lu gpu_n=%lu gpu_ok=%u vrm_p=%lu vrm_h=%lu vrm_n=%lu vrm_ok=%u run=%u break=%u breaks=%lu dac=%lu usb_loss=%lu rx_overflow=%lu tx_drop=%lu%s%s\n",
  (unsigned long)now,ctrl_state_name(control.state),(unsigned long)(DBGMCU->IDCODE>>16),
  (unsigned long)hw_option_word(),(unsigned long)hw_reset_flags(),control.requested_ccr,
  a.raw_sense,a.raw_vref,a.vdda_mv,a.rail_mv,(long)ctrl_calibrated_mv(&control),
  a.valid && (uint32_t)(now-a.last_ms)<=ADC_STALE_MS,(unsigned long)a.last_ms,(unsigned long)g.last_ms,(unsigned long)v.last_ms,hw_pwm_ready(),
  (unsigned long)g.period_ticks,(unsigned long)g.high_ticks,(unsigned long)g.sample_count,g.valid && (uint32_t)(now-g.last_ms)<=CAPTURE_STALE_MS,
  (unsigned long)v.period_ticks,(unsigned long)v.high_ticks,(unsigned long)v.sample_count,v.valid && (uint32_t)(now-v.last_ms)<=CAPTURE_STALE_MS,
  hw_run(),hw_break_active(),(unsigned long)hw_break_events(),(unsigned long)hw_threshold_code(),
  (unsigned long)usb_transport_loss_count(),(unsigned long)usb_transport_rx_overflow_count(),
  (unsigned long)usb_transport_tx_drop_count(),selftest_suffix,sequence_suffix);
}
int main(void) {
 hw_early_safe(); ctrl_init(&control);
#if defined(PWMVID_SEQUENCE) && PWMVID_SEQUENCE
 seq_runtime_init();
#endif
 if(!hw_boot_init() || !usb_transport_init())fail_stop();
 for(;;) {
  uint32_t now=HAL_GetTick();
  if(dfu_waiting) {
   /* ACK has been queued. Hold all control outputs off while USB drains;
    * host closing DTR is expected and must not cancel the approved reset. */
   hw_drop_permission();
   if(hw_run() || !(FLASH->OPTR&FLASH_OPTR_IWDG_SW)) {
    dfu_waiting=false;ctrl_fault_isr(&control);
   } else if((uint32_t)(now-dfu_requested_ms)>=100u) {
    __disable_irq();hw_drop_permission();
    volatile uint32_t *cookie=(volatile uint32_t *)PWMVID_BOOT_COOKIE_ADDRESS;
    cookie[0]=PWMVID_BOOT_COOKIE_MAGIC;cookie[1]=~PWMVID_BOOT_COOKIE_MAGIC;__DSB();
    NVIC_SystemReset();
   }
   hw_watchdog_feed();__WFI();continue;
  }
  if(usb_loss_pending) {
   uint32_t old=__get_BASEPRI();__set_BASEPRI(2u<<(8u-__NVIC_PRIO_BITS));
   usb_loss_pending=false;ctrl_usb_lost(&control);__set_BASEPRI(old);
  }
  bool connected=usb_transport_connected() && hw_vbus();
  if(connected && !acquisition_active) {
   if(!hw_start_acquisition())fail_stop();
   acquisition_active=true;
  } else if(!connected && acquisition_active) {
   hw_stop_acquisition();acquisition_active=false;ctrl_usb_lost(&control);
  }
  if(now!=last_tick) {
   last_tick=now;hw_poll(now);
   source_mode();
   pwm_health_t h=health(&now);pwm_outputs_t o=ctrl_tick(&control,now,&h);
#if defined(PWMVID_SEQUENCE) && PWMVID_SEQUENCE
   capture_summary_t vrm;hw_capture_summary(1,&vrm);
   seq_runtime_poll(&control,now,&vrm);
   if(control.fault_pending){hw_drop_permission();o.permit=false;}
#endif
   if(!o.permit)hw_set_permit(false);
   hw_led((unsigned)control.state,(unsigned)control.fault,now);
  }
  /* Bounded input service; line assembly and endpoint transmission never wait.
   * A USB-loss epoch invalidates any line taken from the previous connection. */
  for(unsigned n=0;n<2;n++) {
   uint32_t epoch=usb_transport_loss_count();
   if(!usb_transport_poll_line(line,sizeof(line)))break;
   if(usb_loss_pending || epoch!=usb_transport_loss_count())break;
   if(strcmp(line,"TELEM")==0)diagnostics();
   else if(strcmp(line,"RAW")==0)hw_raw(response,sizeof(response));
   else ctrl_command(&control,line,response,sizeof(response),HAL_GetTick());
#if defined(PWMVID_SEQUENCE) && PWMVID_SEQUENCE
   if(control.state!=PWM_ARMED && seq_runtime_owns_ccr())seq_runtime_abort();
#endif
   size_t response_length=strlen(response);
   if(response_length && response[response_length-1]!='\n' && response_length+1<sizeof(response)) {
    response[response_length]='\n';response[response_length+1]='\0';
   }
   bool queued=false;
   if(epoch==usb_transport_loss_count() && !usb_loss_pending)queued=usb_transport_write(response);
   if(control.dfu_requested) {
    control.dfu_requested=false;
    if(queued) {hw_drop_permission();dfu_requested_ms=HAL_GetTick();dfu_waiting=true;}
    break;
   }
  }
  source_mode();now=HAL_GetTick();pwm_health_t h=health(&now);pwm_outputs_t o=ctrl_tick(&control,now,&h);
  uint32_t epoch=usb_transport_loss_count();
  /* GPIO/CCR updates and the one-shot ARM phase search mask USB/DMA/SysTick;
   * each ARM search interval is bounded to two PWM periods plus one small
   * deadline-check block. Priority-0 break
   * remains live and ISRs clear permit before signaling software. */
  uint32_t old=__get_BASEPRI();__set_BASEPRI(1u<<(8u-__NVIC_PRIO_BITS));__DSB();
  bool allowed=!usb_loss_pending && !control.fault_pending && epoch==usb_transport_loss_count();
#if defined(PWMVID_SEQUENCE) && PWMVID_SEQUENCE
  if(!seq_runtime_owns_ccr() && o.requested_ccr>=PWM_MIN_CCR && o.requested_ccr<=PWM_MAX_CCR)hw_set_ccr(o.requested_ccr);
#else
  if(o.requested_ccr>=PWM_MIN_CCR && o.requested_ccr<=PWM_MAX_CCR)hw_set_ccr(o.requested_ccr);
#endif
  hw_set_permit(allowed && o.permit);
  __set_BASEPRI(old);
  if(o.arm_request) {
   bool success=allowed && hw_arm_pulse(epoch,control.heartbeat_ms,control.lease_ms);
   ctrl_arm_result(&control,success,HAL_GetTick());
#if defined(PWMVID_SEQUENCE) && PWMVID_SEQUENCE
   if(success && control.state==PWM_ARMED && !seq_runtime_arm(control.requested_ccr)){
    hw_drop_permission();ctrl_fault_isr(&control);success=false;
   }
#endif
   if(!success || control.state!=PWM_ARMED)hw_drop_permission();
  }
  /* Only completed foreground iterations refresh IWDG, never a timer/USB ISR. */
  hw_watchdog_feed();
  __WFI(); /* Wake on USB/DMA/SysTick; no blocking serial or polling spin. */
 }
}
