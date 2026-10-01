#include "sequence_runtime.h"
#if defined(PWMVID_SEQUENCE) && PWMVID_SEQUENCE
#include "sequence_dma.h"
#include "text_format.h"
#include <string.h>

enum { SEQ_MIN_CCR=2u, SEQ_FEEDBACK_MIN_CCR=16u, SEQ_MAX_CCR=224u,
       SEQ_SUSTAINED_PERIODS=3375u, SEQ_FEEDBACK_DEADLINE=2025u,
       SEQ_SHORT_BURST_MAX=6750u, SEQ_EXPERIMENTAL_LOW_MAX=337500u };
static pwmvid_seq_engine_t engine;
static pwmvid_seq_dma_t queue;
static volatile bool dma_owned;
#if defined(PWMVID_SEQUENCE_BENCH) && PWMVID_SEQUENCE_BENCH
static volatile uint32_t bench_dma_reason, bench_dma_flags, bench_dma_left, bench_dma_events;
static volatile uint32_t bench_feedback_reason, bench_feedback_period, bench_feedback_step;
static volatile uint32_t bench_feedback_target, bench_feedback_measured, bench_feedback_samples;
static volatile uint32_t bench_feedback_timer_ccr, bench_feedback_dma_word, bench_feedback_active;
void seq_runtime_bench_diag(uint32_t *reason, uint32_t *flags,
                            uint32_t *left, uint32_t *events) {
 *reason=bench_dma_reason;*flags=bench_dma_flags;
 *left=bench_dma_left;*events=bench_dma_events;
}
void seq_runtime_bench_feedback(uint32_t *reason, uint32_t *period,
                                uint32_t *step, uint32_t *target,
                                uint32_t *measured, uint32_t *samples,
                                uint32_t *timer_ccr, uint32_t *dma_word,
                                uint32_t *engine_active) {
 *reason=bench_feedback_reason;*period=bench_feedback_period;
 *step=bench_feedback_step;*target=bench_feedback_target;
 *measured=bench_feedback_measured;*samples=bench_feedback_samples;
 *timer_ccr=bench_feedback_timer_ccr;*dma_word=bench_feedback_dma_word;
 *engine_active=bench_feedback_active;
}
#endif
static bool run_pending, bg_verify, verify_started, final_verified, hold_verified, cancel_requested, cancel_applied;
static uint8_t verify_step;
static uint64_t verify_start, final_start;
static uint64_t cancel_target;
static uint16_t verify_ccr, final_ccr;
static uint32_t baseline_period_count, baseline_high_count;
static uint32_t hold_last_match_ms;
static uint32_t completed_id;

static uint32_t mask_dma_irq(void) {
 uint32_t prior=__get_BASEPRI();
 __set_BASEPRI_MAX(1u<<(8u-__NVIC_PRIO_BITS));__DSB();__ISB();
 return prior;
}
static void restore_irq(uint32_t prior) {__DMB();__set_BASEPRI(prior);}
static bool number(const char *s,uint32_t *out) {
 uint32_t n=0;
 if(!s || !*s)return false;
 for(;*s;s++){
  if(*s<'0'||*s>'9'||n>(UINT32_MAX-(uint32_t)(*s-'0'))/10u)return false;
  n=n*10u+(uint32_t)(*s-'0');
 }
 *out=n;return true;
}
static void error(char *out,size_t n,const char *reason){pwm_snprintf(out,n,"ERR %s",reason);}
static uint64_t current_period_locked(void) {
 uint32_t left=DMA1_Channel6->CNDTR;
 if(!left || left>PWMVID_SEQ_DMA_WORDS)return 0;
 uint32_t since=queue.expect_half ? PWMVID_SEQ_DMA_WORDS-left :
                 (PWMVID_SEQ_DMA_HALF-left)&(PWMVID_SEQ_DMA_WORDS-1u);
 uint64_t transfers=queue.completed_transfers+since;
 return transfers ? transfers-1u : 0u;
}
static uint64_t current_period(void) {
 uint32_t prior=mask_dma_irq();
 uint64_t period=dma_owned?current_period_locked():0u;
 restore_irq(prior);return period;
}
void seq_runtime_init(void) {
 memset(&engine,0,sizeof(engine));memset(&queue,0,sizeof(queue));
 dma_owned=false;run_pending=false;bg_verify=false;final_verified=true;cancel_requested=false;cancel_applied=false;
 completed_id=0;
#if defined(PWMVID_SEQUENCE_BENCH) && PWMVID_SEQUENCE_BENCH
 bench_dma_reason=0;bench_dma_flags=0;bench_dma_left=0;bench_dma_events=0;
 bench_feedback_reason=0;bench_feedback_period=0;bench_feedback_step=0;
 bench_feedback_target=0;bench_feedback_measured=0;bench_feedback_samples=0;
 bench_feedback_timer_ccr=0;bench_feedback_dma_word=0;bench_feedback_active=0;
#endif
}
void seq_runtime_abort_isr(void) {
 if(!dma_owned)return;
 TIM1->DIER&=~TIM_DIER_UDE;
 DMA1_Channel6->CCR=0;
 DMA1->IFCR=14u<<20u; /* Channel 6 individual HT/TC/TE flags. */
 dma_owned=false;
}
void seq_runtime_abort(void) {
 uint32_t prior=mask_dma_irq();
 seq_runtime_abort_isr();
 pwmvid_seq_dma_abort(&queue);
 run_pending=false;bg_verify=false;verify_started=false;hold_verified=false;final_verified=false;cancel_requested=false;cancel_applied=false;
 restore_irq(prior);
}
bool seq_runtime_owns_ccr(void){return dma_owned;}
bool seq_runtime_arm(uint16_t background_ccr) {
 if(dma_owned || background_ccr<SEQ_FEEDBACK_MIN_CCR ||
    background_ccr>SEQ_MAX_CCR || !hw_run() || !hw_pwm_ready())return false;
 pwmvid_seq_engine_init(&engine,background_ccr);
 if(!pwmvid_seq_dma_init(&queue,&engine))return false;
 __HAL_RCC_DMA1_CLK_ENABLE();__HAL_RCC_DMAMUX1_CLK_ENABLE();
 TIM1->DIER&=~TIM_DIER_UDE;DMA1_Channel6->CCR=0;
 DMA1->IFCR=14u<<20u;
 DMAMUX1_Channel5->CCR=DMA_REQUEST_TIM1_UP;
 DMA1_Channel6->CPAR=(uint32_t)&TIM1->CCR1;
 DMA1_Channel6->CMAR=(uint32_t)queue.words;
 DMA1_Channel6->CNDTR=PWMVID_SEQ_DMA_WORDS;
 DMA1_Channel6->CCR=DMA_CCR_DIR|DMA_CCR_MINC|DMA_CCR_CIRC|DMA_CCR_HTIE|
                     DMA_CCR_TCIE|DMA_CCR_TEIE|DMA_CCR_PL_1|
                     DMA_CCR_PSIZE_0|DMA_CCR_MSIZE_0|DMA_CCR_EN;
 NVIC_SetPriority(DMA1_Channel6_IRQn,1);HAL_NVIC_EnableIRQ(DMA1_Channel6_IRQn);
 run_pending=false;bg_verify=true;verify_started=false;hold_verified=false;final_verified=false;cancel_requested=false;cancel_applied=false;
 final_start=0;final_ccr=background_ccr;
 dma_owned=true;__DMB();TIM1->DIER|=TIM_DIER_UDE;
 return true;
}
void DMA1_Channel6_IRQHandler(void) {
 uint32_t flags=(DMA1->ISR>>20u)&15u;
 DMA1->IFCR=(flags&14u)<<20u;
 if(!dma_owned)return;
 /* STM32 DMA channel flags: GIF bit 0, TCIF bit 1, HTIF bit 2, TEIF bit 3.
  * Reversing TC and HT makes the first half-ring IRQ look one half late. */
 bool half=(flags&4u)!=0u, full=(flags&2u)!=0u, error_flag=(flags&8u)!=0u;
 uint32_t left=DMA1_Channel6->CNDTR;
 bool late=(half && (left==0u || left>PWMVID_SEQ_DMA_HALF)) ||
           (full && (left<=PWMVID_SEQ_DMA_HALF || left>PWMVID_SEQ_DMA_WORDS));
 bool ok=!late && pwmvid_seq_dma_event(&queue,half,full,error_flag);
#if defined(PWMVID_SEQUENCE_BENCH) && PWMVID_SEQUENCE_BENCH
 bench_dma_events++;
 if(!ok && !bench_dma_reason){
  bench_dma_reason=late?1u:(error_flag?2u:3u);
  bench_dma_flags=flags;bench_dma_left=left;
 }
#endif
 if(!ok){
  hw_drop_permission();app_fault_isr();
 }
}
static bool plan_feedback_bounds(const pwmvid_seq_plan_t *plan) {
 if(!plan || !pwmvid_seq_validate(plan,SEQ_MIN_CCR,SEQ_MAX_CCR) ||
    plan->background_ccr<SEQ_FEEDBACK_MIN_CCR ||
    plan->post_ccr<SEQ_FEEDBACK_MIN_CCR)return false;
 uint32_t prior=0,short_span=0;
 for(unsigned i=0;i<plan->count;i++){
  uint32_t duration=plan->steps[i].end_period-prior;
  if(plan->steps[i].ccr<SEQ_FEEDBACK_MIN_CCR){
   if(duration>SEQ_EXPERIMENTAL_LOW_MAX)return false;
   if(duration<SEQ_SUSTAINED_PERIODS){
    if(duration>SEQ_SHORT_BURST_MAX-short_span)return false;
    short_span+=duration;
   }else short_span=0; /* Long experimental low-duty hold is rail-checked. */
  }else if(duration<SEQ_SUSTAINED_PERIODS){
   if(duration>SEQ_SHORT_BURST_MAX-short_span)return false;
   short_span+=duration;
  }else short_span=0;
  prior=plan->steps[i].end_period;
 }
 return true;
}
static bool profile_bounds(const pwm_control_t *c,const pwmvid_seq_plan_t *plan){
 if(!c->profile_valid)return false;
 uint16_t lo=c->profile.ccr1<c->profile.ccr2?c->profile.ccr1:c->profile.ccr2;
 uint16_t hi=c->profile.ccr1>c->profile.ccr2?c->profile.ccr1:c->profile.ccr2;
 if(plan->background_ccr<lo || plan->background_ccr>hi ||
    plan->post_ccr<lo || plan->post_ccr>hi)return false;
 for(unsigned i=0;i<plan->count;i++){
  uint16_t ccr=plan->steps[i].ccr;
  if(ccr>=SEQ_FEEDBACK_MIN_CCR && (ccr<lo || ccr>hi))return false;
 }
 return true;
}
static void fail_feedback(pwm_control_t *c,uint32_t reason,uint64_t period,
                          const capture_summary_t *vrm){
#if defined(PWMVID_SEQUENCE_BENCH) && PWMVID_SEQUENCE_BENCH
 if(!bench_feedback_reason){
  bench_feedback_reason=reason;bench_feedback_period=(uint32_t)period;
  bench_feedback_step=verify_step;bench_feedback_target=verify_ccr;
  bench_feedback_measured=vrm && vrm->period_ticks ?
   (vrm->high_ticks*PWM_PERIOD+vrm->period_ticks/2u)/vrm->period_ticks : 0u;
  bench_feedback_samples=vrm?vrm->sample_count-baseline_period_count:0u;
  bench_feedback_timer_ccr=TIM1->CCR1;
  uint32_t left=DMA1_Channel6->CNDTR;
  if(left<=PWMVID_SEQ_DMA_WORDS)
   bench_feedback_dma_word=queue.words[(PWMVID_SEQ_DMA_WORDS-left-1u)&(PWMVID_SEQ_DMA_WORDS-1u)];
  bench_feedback_active=engine.active;
 }
#else
 (void)reason;(void)period;(void)vrm;
#endif
 hw_drop_permission();ctrl_fault_isr(c);
 run_pending=false;bg_verify=false;hold_verified=false;final_verified=false;cancel_requested=false;cancel_applied=false;
}
static bool fresh_match(const capture_summary_t *vrm,uint32_t now) {
 if(!vrm->valid || (uint32_t)(now-vrm->last_ms)>CAPTURE_STALE_MS ||
    (uint32_t)(vrm->sample_count-baseline_period_count)<64u ||
    (uint32_t)(vrm->high_sample_count-baseline_high_count)<64u ||
    !vrm->period_ticks)return false;
 int32_t ccr=(int32_t)((vrm->high_ticks*PWM_PERIOD+vrm->period_ticks/2u)/vrm->period_ticks);
 int32_t delta=ccr-(int32_t)verify_ccr;
 return delta>=-12 && delta<=12;
}
static bool verify_hold(pwm_control_t *c,uint64_t period,uint64_t start,
                        uint16_t target,uint32_t now,const capture_summary_t *vrm){
 if(period<start)return false;
 if(!verify_started || verify_start!=start || verify_ccr!=target){
  verify_started=true;verify_start=start;verify_ccr=target;
  baseline_period_count=vrm->sample_count;
  baseline_high_count=vrm->high_sample_count;
 }
 if(fresh_match(vrm,now)){verify_started=false;return true;}
 if(period>=start+SEQ_FEEDBACK_DEADLINE)fail_feedback(c,1u,period,vrm);
 return false;
}
void seq_runtime_poll(pwm_control_t *c,uint32_t now,const capture_summary_t *vrm){
 if(!c || !vrm)return;
 if(c->state!=PWM_ARMED){if(dma_owned)seq_runtime_abort();return;}
 if(!dma_owned){fail_feedback(c,2u,0u,vrm);return;}
 if(!run_pending && !bg_verify)return;
 uint64_t period=current_period();
 if(bg_verify){
  if(verify_hold(c,period,final_start,final_ccr,now,vrm)){
   bg_verify=false;final_verified=true;c->requested_ccr=final_ccr;c->sequence_mode=false;
  }
  return;
 }
 if(cancel_requested && !cancel_applied && period>=cancel_target){
  final_start=cancel_target;final_ccr=engine.background_ccr;
  verify_step=engine.running.count;verify_started=false;hold_verified=false;
  cancel_applied=true;
 }
 while(verify_step<engine.running.count){
  uint64_t start=engine.start_period+(verify_step?engine.running.steps[verify_step-1u].end_period:0u);
  uint64_t end=engine.start_period+engine.running.steps[verify_step].end_period;
  uint32_t duration=(uint32_t)(end-start);
  uint16_t ccr=engine.running.steps[verify_step].ccr;
  if(period<start)return;
  if(duration<SEQ_SUSTAINED_PERIODS || ccr<SEQ_FEEDBACK_MIN_CCR){
   if(period<end)return;
   verify_step++;verify_started=false;hold_verified=false;continue;
  }
  if(!hold_verified){
   if(period>=end){fail_feedback(c,3u,period,vrm);return;}
   if(!verify_hold(c,period,start,ccr,now,vrm))return;
   hold_verified=true;hold_last_match_ms=now;
  }
  if(period<end){
   if(!vrm->valid || (uint32_t)(now-vrm->last_ms)>CAPTURE_STALE_MS){fail_feedback(c,4u,period,vrm);return;}
   if(vrm->valid && (uint32_t)(now-vrm->last_ms)<=CAPTURE_STALE_MS &&
      (uint32_t)(vrm->sample_count-baseline_period_count)>=64u &&
      (uint32_t)(vrm->high_sample_count-baseline_high_count)>=64u){
    verify_ccr=ccr;
    if(!fresh_match(vrm,now)){fail_feedback(c,5u,period,vrm);return;}
    baseline_period_count=vrm->sample_count;
    baseline_high_count=vrm->high_sample_count;
    hold_last_match_ms=now;
   }
   if((uint32_t)(now-hold_last_match_ms)>=3u){fail_feedback(c,6u,period,vrm);return;}
   return;
  }
  verify_step++;verify_started=false;hold_verified=false;
 }
 if(!final_start){
  final_start=engine.start_period+engine.running.steps[engine.running.count-1u].end_period;
  final_ccr=engine.running.post_ccr;verify_started=false;
 }
 if(period<final_start)return;
 if(verify_hold(c,period,final_start,final_ccr,now,vrm)){
  run_pending=false;final_verified=true;completed_id=engine.running_id;
  cancel_requested=false;cancel_applied=false;
  c->requested_ccr=final_ccr;c->sequence_mode=false;
 }
}
void seq_runtime_command(pwm_control_t *c,char **t,size_t nt,char *out,size_t n,uint32_t now){
 (void)now;
 if(!c || !t || !out || !n){return;}
 if(!strcmp(t[2],"SETBG")){
  uint32_t ccr;
  if(nt!=4 || !number(t[3],&ccr) || ccr<SEQ_FEEDBACK_MIN_CCR || ccr>SEQ_MAX_CCR){error(out,n,"SETBG_RANGE");return;}
  uint16_t lo=c->profile.ccr1<c->profile.ccr2?c->profile.ccr1:c->profile.ccr2;
  uint16_t hi=c->profile.ccr1>c->profile.ccr2?c->profile.ccr1:c->profile.ccr2;
  if(!c->profile_valid || ccr<lo || ccr>hi){error(out,n,"SETBG_PROFILE");return;}
  if(c->state!=PWM_ARMED || !dma_owned || run_pending || bg_verify || !final_verified){error(out,n,"SEQ_STATE");return;}
  uint32_t prior=mask_dma_irq();
  uint64_t start=pwmvid_seq_dma_earliest(&queue);
  bool ok=pwmvid_seq_set_background(&engine,(uint16_t)ccr,SEQ_FEEDBACK_MIN_CCR,SEQ_MAX_CCR);
  if(ok){final_start=start;final_ccr=(uint16_t)ccr;verify_started=false;
         bg_verify=true;final_verified=false;c->sequence_mode=true;}
  restore_irq(prior);
  if(!ok){error(out,n,"SETBG");return;}
  pwm_snprintf(out,n,"OK SETBG ccr=%lu period_hi=%lu period_lo=%lu",(unsigned long)ccr,(unsigned long)(start>>32),(unsigned long)start);return;
 }
 if(nt<4){error(out,n,"SEQ_COMMAND");return;}
 if(!strcmp(t[3],"STATUS") && nt==4){
  uint64_t p=current_period();
  uint16_t scheduled=0;
  uint32_t prior=mask_dma_irq();
  (void)pwmvid_seq_render_absolute(&engine,p,&scheduled,1u);
  restore_irq(prior);
  pwm_snprintf(out,n,"OK SEQ_STATUS id=%lu period_hi=%lu period_lo=%lu active=%u staged=%u running=%u feedback=%u sched_ccr=%u bg=%u run=%u",
   (unsigned long)(run_pending?engine.running_id:completed_id),(unsigned long)(p>>32),(unsigned long)p,
   dma_owned,engine.committed,run_pending,final_verified,scheduled,engine.background_ccr,hw_run());return;
 }
 if(c->state!=PWM_ARMED || !dma_owned || c->fault_pending){error(out,n,"SEQ_STATE");return;}
 uint32_t id,a,b,d;
 if(!strcmp(t[3],"BEGIN") && nt==8){
  if(run_pending || bg_verify || !final_verified || !number(t[4],&id) ||
     !number(t[5],&a) || !number(t[6],&b) || !number(t[7],&d) ||
     a>PWMVID_SEQ_MAX_STEPS || b<SEQ_FEEDBACK_MIN_CCR || b>SEQ_MAX_CCR ||
     !pwmvid_seq_begin(&engine,id,(uint8_t)a,(uint16_t)b,d)){
   error(out,n,"SEQ_BEGIN");return;
  }
  pwm_snprintf(out,n,"OK SEQ_BEGIN id=%lu",(unsigned long)id);return;
 }
 if(!strcmp(t[3],"ADD") && nt==8){
  if(!number(t[4],&id)||!number(t[5],&a)||!number(t[6],&b)||!number(t[7],&d)||
     a>255u || d<SEQ_MIN_CCR || d>SEQ_MAX_CCR ||
     !pwmvid_seq_add(&engine,id,(uint8_t)a,b,(uint16_t)d)){
   error(out,n,"SEQ_ADD");return;
  }
  pwm_snprintf(out,n,"OK SEQ_ADD index=%lu",(unsigned long)a);return;
 }
 if(!strcmp(t[3],"COMMIT") && nt==6){
  if(!number(t[4],&id)||!number(t[5],&d)||
     engine.added_count!=engine.expected_count){error(out,n,"SEQ_COMMIT");return;}
  pwmvid_seq_plan_t candidate=engine.staged;
  candidate.count=engine.expected_count;
  if(!plan_feedback_bounds(&candidate) || !profile_bounds(c,&candidate) ||
     !pwmvid_seq_commit(&engine,id,d,SEQ_MIN_CCR,SEQ_MAX_CCR)){
   error(out,n,"SEQ_COMMIT");return;
  }
  pwm_snprintf(out,n,"OK SEQ_COMMIT id=%lu",(unsigned long)id);return;
 }
 if(!strcmp(t[3],"START") && nt==5){
  if(!number(t[4],&id)||run_pending||bg_verify||!final_verified||
     !engine.committed || !hw_run() || c->feedback_pending){error(out,n,"SEQ_START");return;}
  uint32_t prior=mask_dma_irq();
  uint64_t start=pwmvid_seq_dma_earliest(&queue);
  bool ok=pwmvid_seq_start(&engine,id,start,start);
  if(ok){run_pending=true;final_verified=false;verify_step=0;hold_verified=false;cancel_requested=false;cancel_applied=false;
         final_start=0;verify_started=false;c->sequence_mode=true;}
  restore_irq(prior);
  if(!ok){error(out,n,"SEQ_START");return;}
  pwm_snprintf(out,n,"OK SEQ_START id=%lu period_hi=%lu period_lo=%lu",(unsigned long)id,(unsigned long)(start>>32),(unsigned long)start);return;
 }
 if(!strcmp(t[3],"CANCEL") && nt==5){
  if(!number(t[4],&id)||!run_pending){error(out,n,"SEQ_CANCEL");return;}
  uint32_t prior=mask_dma_irq();
  uint64_t stop=pwmvid_seq_dma_earliest(&queue);
  bool ok=pwmvid_seq_cancel(&engine,id,stop,stop);
  if(ok){cancel_requested=true;cancel_applied=false;cancel_target=stop;}
  restore_irq(prior);
  if(!ok){error(out,n,"SEQ_CANCEL");return;}
  pwm_snprintf(out,n,"OK SEQ_CANCEL period_hi=%lu period_lo=%lu",
               (unsigned long)(stop>>32),(unsigned long)stop);return;
 }
 error(out,n,"SEQ_COMMAND");
}
void seq_runtime_telemetry(char *out,size_t n){
 uint64_t period=current_period();
 pwm_snprintf(out,n," seq=%u seq_id=%lu seq_period_hi=%lu seq_period_lo=%lu seq_verified=%u seq_bg=%u",
  run_pending,(unsigned long)(run_pending?engine.running_id:completed_id),
  (unsigned long)(period>>32),(unsigned long)period,final_verified,engine.background_ccr);
}
#endif
