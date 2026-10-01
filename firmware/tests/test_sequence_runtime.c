#include <assert.h>
#include <stdio.h>

#include "sequence_runtime.h"
static mock_dma_channel_t channel;
static mock_dma_t dma;
static mock_mux_channel_t mux;
static mock_timer_t timer;
mock_dma_channel_t *DMA1_Channel6 = &channel;
mock_dma_t *DMA1 = &dma;
mock_mux_channel_t *DMAMUX1_Channel5 = &mux;
mock_timer_t *TIM1 = &timer;
static unsigned drops, faults;
bool hw_run(void) { return true; }
bool hw_pwm_ready(void) { return true; }
void hw_drop_permission(void) { ++drops; }
void app_fault_isr(void) { ++faults; }
void ctrl_fault_isr(pwm_control_t *control) { control->fault_pending = true; ++faults; }

/* Include the actual adapter so the test can set the DMA-owned period and
 * observe its internal verifier state without replacing production logic. */
#include "../src/sequence_runtime.c"

static void at_period(uint64_t period) {
 uint64_t transfers=period+1u;
 uint32_t remainder=(uint32_t)(transfers%128u);
 queue.completed_transfers=transfers-remainder;
 if(remainder<64u){
  queue.expect_half=true;
  channel.CNDTR=128u-remainder;
 }else{
  queue.completed_transfers+=64u;
  queue.expect_half=false;
  channel.CNDTR=128u-remainder;
 }
}

int main(void) {
 pwm_control_t control={0};
 seq_runtime_init();
 control.state=PWM_ARMED;
 control.sequence_mode=true;
 dma_owned=true;
 run_pending=true;
 final_verified=false;
 engine.running_id=7u;
 engine.background_ccr=60u;
 engine.start_period=100u;
 engine.running.count=1u;
 engine.running.steps[0].end_period=1000u;
 engine.running.steps[0].ccr=2u;
 verify_step=0u;
 cancel_requested=true;
 cancel_target=200u;
 capture_summary_t vrm={.period_ticks=240u,.high_ticks=60u,
                        .sample_count=1000u,.high_sample_count=1000u,
                        .last_ms=10u,.valid=true};
 at_period(200u);
 seq_runtime_poll(&control,10u,&vrm);
 assert(run_pending && cancel_applied && final_start==200u);
 vrm.sample_count+=100u;
 vrm.high_sample_count+=100u;
 vrm.last_ms=11u;
 at_period(875u);
 seq_runtime_poll(&control,11u,&vrm);
 assert(!run_pending && final_verified && completed_id==7u);
 assert(control.requested_ccr==60u && !control.sequence_mode);
 assert(!drops && !faults);
 seq_runtime_init();
 assert(seq_runtime_arm(64u));
 assert((channel.CCR&DMA_CCR_DIR)!=0u);
 seq_runtime_abort();
 /* STM32 channel status uses HTIF=bit 2 and TCIF=bit 1. Both boundaries
  * must refill the correct half of the CCR ring without faulting. */
 seq_runtime_init();
 pwmvid_seq_engine_init(&engine,64u);
 assert(pwmvid_seq_dma_init(&queue,&engine));
 dma_owned=true;
 channel.CNDTR=64u;dma.ISR=4u<<20u;
 DMA1_Channel6_IRQHandler();
 assert(queue.completed_transfers==64u && !queue.expect_half);
 channel.CNDTR=128u;dma.ISR=2u<<20u;
 DMA1_Channel6_IRQHandler();
 assert(queue.completed_transfers==128u && queue.expect_half);
 assert(!drops && !faults);
 puts("Cancel verifies post-duty after one baseline transition");
 return 0;
}
