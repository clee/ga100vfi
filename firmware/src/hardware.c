/* PWMVID isolated-fixture first build. Fast waveforms are hardware timer outputs.
 * No software function here claims a measured response time. See bench matrix. */
#include "hardware.h"

/* PWMVID_ALLOW_FIXTURE selects the explicit active bench build. The telemetry
 * build still cannot assert PERMIT or ARM. Neither image is GPU-qualified. */
#include "usb_transport.h"
#include "control.h"
#include "text_format.h"
#include <string.h>

#ifndef PWMVID_INTERNAL_SOURCE_SELFTEST
#define PWMVID_INTERNAL_SOURCE_SELFTEST 0
#endif
#ifndef PWMVID_SEQUENCE_BENCH
#define PWMVID_SEQUENCE_BENCH 0
#endif
#if PWMVID_SEQUENCE_BENCH
static volatile uint32_t bench_fault;
uint32_t hw_bench_fault(void) { return bench_fault; }
#define BENCH_FAULT(code) do { if(!bench_fault)bench_fault=(code); } while(0)
#else
#define BENCH_FAULT(code) ((void)0)
#endif

static ADC_HandleTypeDef adc;
static DMA_HandleTypeDef adc_dma;
static volatile uint16_t adc_data[128] __attribute__((aligned(4)));
static volatile uint32_t cap2_period[CAPTURE_SAMPLES], cap2_high[CAPTURE_SAMPLES];
static volatile uint16_t cap3_period[CAPTURE_SAMPLES], cap3_high[CAPTURE_SAMPLES];
static volatile uint32_t cap_count[4], cap_ms[4], cap_errors[4];
static volatile adc_summary_t adc_result;
static volatile uint32_t break_count;
static uint32_t reset_flags;
static uint32_t threshold_code;
static uint16_t threshold_vdda;
static bool high_clock, acquiring, threshold_ready;
static uint32_t acquisition_started;
static uint16_t current_ccr = 120;
static volatile uint8_t arm_diag_stage, arm_diag_post;

static void mode(GPIO_TypeDef *p, unsigned pin, unsigned m, unsigned af) {
 p->MODER=(p->MODER & ~(3u<<(pin*2u))) | (m<<(pin*2u));
 p->PUPDR &= ~(3u<<(pin*2u));
 if(m==2u) p->AFR[pin/8u]=(p->AFR[pin/8u] & ~(15u<<((pin%8u)*4u))) | (af<<((pin%8u)*4u));
}
void hw_drop_permission(void) {
 GPIOB->BSRR=(GPIO_PIN_1|GPIO_PIN_15)<<16u;
 TIM1->BDTR &= ~TIM_BDTR_MOE;
#if defined(PWMVID_SEQUENCE) && PWMVID_SEQUENCE
  extern void seq_runtime_abort_isr(void);
  seq_runtime_abort_isr();
#endif
}
void hw_early_safe(void) {
 RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
 (void)RCC->AHB2ENR;
 GPIOB->BSRR=(GPIO_PIN_1|GPIO_PIN_15|GPIO_PIN_13)<<16u;
 GPIOA->BSRR=(GPIO_PIN_9|GPIO_PIN_10)<<16u;
 mode(GPIOB,1,1,0); mode(GPIOB,15,1,0);
 mode(GPIOA,9,1,0); mode(GPIOA,10,1,0); mode(GPIOB,13,1,0);
 GPIOB->BSRR=GPIO_PIN_13; /* Passive startup is visible before USB has a client. */
 mode(GPIOB,14,0,0); /* RUN_SENSE through 47k: never enable pulls. */
 mode(GPIOB,5,0,0);
 mode(GPIOA,5,3,0); mode(GPIOB,0,3,0); mode(GPIOB,10,3,0); mode(GPIOB,11,3,0);
}
static bool wait_mask(volatile uint32_t *r, uint32_t mask, bool set, uint32_t timeout) {
 uint32_t start=HAL_GetTick();
 while (((*r & mask)!=0u) != set) if((uint32_t)(HAL_GetTick()-start)>timeout) return false;
 return true;
}
static void delay_us(uint32_t us) {
 uint32_t start=DWT->CYCCNT, cycles=(SystemCoreClock/1000000u)*us;
 while((uint32_t)(DWT->CYCCNT-start)<cycles) { __NOP(); }
}
/* HAL_RCC_ClockConfig updates SystemCoreClock and SysTick after each step.
 * Request HCLK alone so HAL cannot undo this explicit intermediate divider. */
static bool hclk_divider(uint32_t divider, uint32_t latency) {
 RCC_ClkInitTypeDef c={0};
 c.ClockType=RCC_CLOCKTYPE_HCLK; c.AHBCLKDivider=divider;
 if(HAL_RCC_ClockConfig(&c,latency)!=HAL_OK)return false;
 __DSB(); __ISB();
 return (RCC->CFGR&RCC_CFGR_HPRE)==divider &&
        (FLASH->ACR&FLASH_ACR_LATENCY)==latency;
}
static bool low_clock(void) {
 high_clock=false;
 RCC_ClkInitTypeDef c={0};
 c.ClockType=RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
 c.SYSCLKSource=RCC_SYSCLKSOURCE_HSI;
 c.APB1CLKDivider=RCC_HCLK_DIV1; c.APB2CLKDivider=RCC_HCLK_DIV1;
 if(SystemCoreClock>80000000u) {
  /* RM0440 Rev 9 sections 6.1.5 and 7.2.7: retain the divided clock for
   * >=1 us on the downward transition too. Keep boost and Flash margin. */
  if(!hclk_divider(RCC_SYSCLK_DIV2,FLASH_LATENCY_4))return false;
  delay_us(2);
  c.AHBCLKDivider=RCC_SYSCLK_DIV2;
  if(HAL_RCC_ClockConfig(&c,FLASH_LATENCY_4)!=HAL_OK)return false;
  if((RCC->CFGR&RCC_CFGR_SWS)!=RCC_CFGR_SWS_HSI ||
     (RCC->CFGR&RCC_CFGR_HPRE)!=RCC_SYSCLK_DIV2)return false;
  delay_us(2);
  if(!hclk_divider(RCC_SYSCLK_DIV1,FLASH_LATENCY_0))return false;
 } else {
  c.AHBCLKDivider=RCC_SYSCLK_DIV1;
  if(HAL_RCC_ClockConfig(&c,FLASH_LATENCY_0)!=HAL_OK)return false;
 }
 RCC->CR &= ~(RCC_CR_PLLON|RCC_CR_HSEON|RCC_CR_CSSON);
 return SystemCoreClock==16000000u &&
        (RCC->CFGR&(RCC_CFGR_SWS|RCC_CFGR_HPRE))==RCC_CFGR_SWS_HSI;
}
static bool fast_clock(void) {
 RCC_OscInitTypeDef o={0}; RCC_ClkInitTypeDef c={0};
 high_clock=false;
 /* RM0440 Rev 9 section 6.1.5, p233: divide first, then select Range 1
  * boost and raise Flash latency before switching to the 162 MHz PLL. */
 if(!hclk_divider(RCC_SYSCLK_DIV2,FLASH_LATENCY_4))return false;
 if(HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST)!=HAL_OK)return false;
 if((PWR->CR1&PWR_CR1_VOS)!=PWR_REGULATOR_VOLTAGE_SCALE1 ||
    (PWR->CR5&PWR_CR5_R1MODE) || (PWR->SR2&PWR_SR2_VOSF))return false;
 o.OscillatorType=RCC_OSCILLATORTYPE_HSE; o.HSEState=RCC_HSE_ON;
 o.PLL.PLLState=RCC_PLL_ON; o.PLL.PLLSource=RCC_PLLSOURCE_HSE;
 o.PLL.PLLM=RCC_PLLM_DIV2; o.PLL.PLLN=81; o.PLL.PLLR=RCC_PLLR_DIV2;
 o.PLL.PLLP=RCC_PLLP_DIV2; o.PLL.PLLQ=RCC_PLLQ_DIV2;
 if(HAL_RCC_OscConfig(&o)!=HAL_OK)return false;
 c.ClockType=RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
 c.SYSCLKSource=RCC_SYSCLKSOURCE_PLLCLK; c.AHBCLKDivider=RCC_SYSCLK_DIV2;
 c.APB1CLKDivider=RCC_HCLK_DIV1; c.APB2CLKDivider=RCC_HCLK_DIV1;
 if(HAL_RCC_ClockConfig(&c,FLASH_LATENCY_4)!=HAL_OK)return false;
 if(SystemCoreClock!=81000000u || (RCC->CFGR&RCC_CFGR_SWS)!=RCC_CFGR_SWS_PLL ||
    (RCC->CFGR&RCC_CFGR_HPRE)!=RCC_SYSCLK_DIV2)return false;
 /* DWT runs at the verified 81 MHz HCLK. This explicit 162-cycle minimum
  * exceeds the required 1 us; incidental HAL instruction time is not relied on. */
 delay_us(2);
 if(!hclk_divider(RCC_SYSCLK_DIV1,FLASH_LATENCY_4))return false;
 HAL_RCC_EnableCSS(); high_clock=true;
 return hw_clock_ok();
}
bool hw_boot_init(void) {
 reset_flags=RCC->CSR;
 HAL_Init(); HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
 /* CMSIS priority setters below use logical priorities directly: all four
  * implemented bits are preemption bits under this fixed grouping. */
 hw_early_safe();
 CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; DWT->CYCCNT=0; DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
 __HAL_RCC_PWR_CLK_ENABLE(); __HAL_RCC_SYSCFG_CLK_ENABLE();
 RCC->CSR |= RCC_CSR_LSION;
 if(!wait_mask(&RCC->CSR,RCC_CSR_LSIRDY,true,100)) return false;
 /* Approximately 250ms at nominal LSI; actual tolerance must be measured. */
 DBGMCU->APB1FZR1 &= ~DBGMCU_APB1FZR1_DBG_IWDG_STOP;
 IWDG->KR=0xcccc; /* RM0440: start watchdog before configuration/wait. */
 IWDG->KR=0x5555; IWDG->PR=4; IWDG->RLR=124;
 if(!wait_mask(&IWDG->SR,IWDG_SR_PVU|IWDG_SR_RVU,false,100)) return false;
 IWDG->KR=0xaaaa;
 RCC->CSR |= RCC_CSR_RMVF;
 RCC->CRRCR |= RCC_CRRCR_HSI48ON;
 if(!wait_mask(&RCC->CRRCR,RCC_CRRCR_HSI48RDY,true,100)) return false;
 MODIFY_REG(RCC->CCIPR,RCC_CCIPR_CLK48SEL,0); /* HSI48 */
 __HAL_RCC_CRS_CLK_ENABLE();
 CRS->CFGR=(47999u<<CRS_CFGR_RELOAD_Pos)|(34u<<CRS_CFGR_FELIM_Pos)|(2u<<CRS_CFGR_SYNCSRC_Pos);
 CRS->CR=CRS_CR_AUTOTRIMEN|CRS_CR_CEN|(32u<<CRS_CR_TRIM_Pos);
 return low_clock();
}
void hw_watchdog_feed(void) { IWDG->KR=0xaaaa; }
bool hw_options_ok(void) {
 const uint32_t op=FLASH->OPTR;
#if PWMVID_INTERNAL_SOURCE_SELFTEST
 /* Temporary, GPU-disconnected physical output check only. Match this
  * board's read-only option word exactly; do not reprogram option bytes. */
 return op==0xFFEFF8AAu;
#else
 /* Software BOOT0 selection + nBOOT0=1 means boot main Flash. No writes. */
 return ((op & FLASH_OPTR_BOR_LEV)==(4u<<FLASH_OPTR_BOR_LEV_Pos)) &&
        ((op & FLASH_OPTR_NRST_MODE)==FLASH_OPTR_NRST_MODE) &&
        ((op & FLASH_OPTR_IRHEN)!=0) && ((op & FLASH_OPTR_nSWBOOT0)==0) &&
        ((op & FLASH_OPTR_nBOOT0)!=0) && ((op & FLASH_OPTR_SRAM_PE)!=0);
#endif
}
bool hw_vbus(void) { return (GPIOB->IDR&GPIO_PIN_5)!=0; }
bool hw_run(void) { return (GPIOB->IDR&GPIO_PIN_14)!=0; }
bool hw_comp_fault(void) { return (COMP2->CSR&COMP_CSR_VALUE)!=0; }
bool hw_break_active(void) { return !(GPIOB->IDR&GPIO_PIN_12) || hw_comp_fault(); }
bool hw_clock_ok(void) {
 return high_clock && SystemCoreClock==162000000u &&
  (RCC->CFGR&(RCC_CFGR_SWS|RCC_CFGR_HPRE|RCC_CFGR_PPRE1|RCC_CFGR_PPRE2))==RCC_CFGR_SWS_PLL &&
  (FLASH->ACR&FLASH_ACR_LATENCY)==FLASH_LATENCY_4 &&
  (PWR->CR1&PWR_CR1_VOS)==PWR_REGULATOR_VOLTAGE_SCALE1 && !(PWR->CR5&PWR_CR5_R1MODE) &&
  (RCC->CR&RCC_CR_PLLRDY) && (RCC->CR&RCC_CR_HSERDY) && (RCC->CRRCR&RCC_CRRCR_HSI48RDY);
}
uint32_t hw_reset_flags(void) { return reset_flags; }
uint32_t hw_option_word(void) { return FLASH->OPTR; }
uint32_t hw_break_events(void) { return break_count; }
uint32_t hw_threshold_code(void) { return threshold_code; }

static void dma_capture(DMA_Channel_TypeDef *ch, DMAMUX_Channel_TypeDef *mux, uint32_t req, volatile void *periph, volatile void *buffer, bool word) {
 ch->CCR=0; ch->CPAR=(uint32_t)periph; ch->CMAR=(uint32_t)buffer; ch->CNDTR=CAPTURE_SAMPLES;
 mux->CCR=req;
 ch->CCR=DMA_CCR_MINC|DMA_CCR_CIRC|DMA_CCR_HTIE|DMA_CCR_TCIE|DMA_CCR_TEIE|DMA_CCR_PL_1|
          (word?(DMA_CCR_PSIZE_1|DMA_CCR_MSIZE_1):(DMA_CCR_PSIZE_0|DMA_CCR_MSIZE_0))|DMA_CCR_EN;
}
static void capture_timer(TIM_TypeDef *t, bool wide) {
 t->CR1=0; t->PSC=0; t->ARR=wide?0xffffffffu:0xffffu;
 /* TI1 direct rising period; TI1 indirect falling high time. Prescale captures
  * by 8, but TI1FP1 reset trigger remains every rising edge. */
 t->CCMR1=TIM_CCMR1_CC1S_0|TIM_CCMR1_IC1PSC|TIM_CCMR1_CC2S_1|TIM_CCMR1_IC2PSC;
 t->CCER=TIM_CCER_CC1E|TIM_CCER_CC2E|TIM_CCER_CC2P;
 t->SMCR=(5u<<TIM_SMCR_TS_Pos)|(4u<<TIM_SMCR_SMS_Pos);
 t->DIER=TIM_DIER_CC1DE|TIM_DIER_CC2DE;
 t->EGR=TIM_EGR_UG; t->SR=0; t->CR1=TIM_CR1_CEN;
}
static void capture_init(void) {
 __HAL_RCC_DMA1_CLK_ENABLE(); __HAL_RCC_DMAMUX1_CLK_ENABLE();
 __HAL_RCC_TIM2_CLK_ENABLE(); __HAL_RCC_TIM3_CLK_ENABLE();
 mode(GPIOA,0,2,1); mode(GPIOA,6,2,2);
 DMA1->IFCR=0xeeeeu; /* Clear individual flags for channels1..4, never CGIF. */
 memset((void *)cap_count,0,sizeof(cap_count)); memset((void *)cap_ms,0,sizeof(cap_ms)); memset((void *)cap_errors,0,sizeof(cap_errors));
 dma_capture(DMA1_Channel1,DMAMUX1_Channel0,DMA_REQUEST_TIM2_CH1,&TIM2->CCR1,cap2_period,true);
 dma_capture(DMA1_Channel2,DMAMUX1_Channel1,DMA_REQUEST_TIM2_CH2,&TIM2->CCR2,cap2_high,true);
 dma_capture(DMA1_Channel3,DMAMUX1_Channel2,DMA_REQUEST_TIM3_CH1,&TIM3->CCR1,cap3_period,false);
 dma_capture(DMA1_Channel4,DMAMUX1_Channel3,DMA_REQUEST_TIM3_CH2,&TIM3->CCR2,cap3_high,false);
 const IRQn_Type irqs[]={DMA1_Channel1_IRQn,DMA1_Channel2_IRQn,DMA1_Channel3_IRQn,DMA1_Channel4_IRQn};
 for(unsigned i=0;i<4;i++){NVIC_SetPriority(irqs[i],1); HAL_NVIC_EnableIRQ(irqs[i]);}
 capture_timer(TIM2,true); capture_timer(TIM3,false);
}
static bool adc_init(void) {
 __HAL_RCC_ADC12_CLK_ENABLE(); mode(GPIOA,2,3,0);
 adc.Instance=ADC1; adc.Init.ClockPrescaler=ADC_CLOCK_SYNC_PCLK_DIV4;
 adc.Init.Resolution=ADC_RESOLUTION_12B; adc.Init.DataAlign=ADC_DATAALIGN_RIGHT;
 adc.Init.GainCompensation=0; adc.Init.ScanConvMode=ADC_SCAN_ENABLE;
 adc.Init.EOCSelection=ADC_EOC_SEQ_CONV; adc.Init.LowPowerAutoWait=DISABLE;
 adc.Init.ContinuousConvMode=ENABLE; adc.Init.NbrOfConversion=4;
 adc.Init.DiscontinuousConvMode=DISABLE; adc.Init.ExternalTrigConv=ADC_SOFTWARE_START;
 adc.Init.ExternalTrigConvEdge=ADC_EXTERNALTRIGCONVEDGE_NONE; adc.Init.DMAContinuousRequests=ENABLE;
 adc.Init.Overrun=ADC_OVR_DATA_OVERWRITTEN; adc.Init.OversamplingMode=DISABLE;
 if(HAL_ADC_Init(&adc)!=HAL_OK || HAL_ADCEx_Calibration_Start(&adc,ADC_SINGLE_ENDED)!=HAL_OK) return false;
 ADC_ChannelConfTypeDef c={0}; c.SamplingTime=ADC_SAMPLETIME_247CYCLES_5; c.SingleDiff=ADC_SINGLE_ENDED;
 c.OffsetNumber=ADC_OFFSET_NONE; c.Offset=0;
 const uint32_t ranks[]={ADC_REGULAR_RANK_1,ADC_REGULAR_RANK_2,ADC_REGULAR_RANK_3,ADC_REGULAR_RANK_4};
 for(unsigned i=0;i<4;i++){c.Channel=i<2?ADC_CHANNEL_3:ADC_CHANNEL_VREFINT;c.Rank=ranks[i];if(HAL_ADC_ConfigChannel(&adc,&c)!=HAL_OK)return false;}
 adc_dma.Instance=DMA1_Channel5; adc_dma.Init.Request=DMA_REQUEST_ADC1;
 adc_dma.Init.Direction=DMA_PERIPH_TO_MEMORY; adc_dma.Init.PeriphInc=DMA_PINC_DISABLE; adc_dma.Init.MemInc=DMA_MINC_ENABLE;
 adc_dma.Init.PeriphDataAlignment=DMA_PDATAALIGN_HALFWORD; adc_dma.Init.MemDataAlignment=DMA_MDATAALIGN_HALFWORD;
 adc_dma.Init.Mode=DMA_CIRCULAR; adc_dma.Init.Priority=DMA_PRIORITY_LOW;
 if(HAL_DMA_Init(&adc_dma)!=HAL_OK)return false;
 __HAL_LINKDMA(&adc,DMA_Handle,adc_dma);
 NVIC_SetPriority(DMA1_Channel5_IRQn,1); HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);
 NVIC_SetPriority(ADC1_2_IRQn,1); HAL_NVIC_EnableIRQ(ADC1_2_IRQn);
 memset((void *)&adc_result,0,sizeof(adc_result));
 /* Each pair retains its first and discards second (ES0431 scan erratum).
  * Initial DMA half-buffer discards many initial scans after calibration.
  * Continuous operation also avoids
  * >1ms ADC idle erratum; VREFINT startup wait is deliberately conservative. */
 delay_us(100);
 return HAL_ADC_Start_DMA(&adc,(uint32_t *)adc_data,128)==HAL_OK;
}
static void protection_init(void) {
 __HAL_RCC_DAC3_CLK_ENABLE(); __HAL_RCC_TIM1_CLK_ENABLE();
 mode(GPIOA,3,3,0); mode(GPIOA,7,2,8); mode(GPIOB,12,2,6);
 DAC3->CR=0; DAC3->MCR=(3u<<DAC_MCR_MODE2_Pos)|(2u<<DAC_MCR_HFSEL_Pos);
 threshold_code=776; DAC3->DHR12R2=threshold_code; DAC3->CR=DAC_CR_EN2;
 delay_us(10);
 COMP2->CSR=(1u<<COMP_CSR_INPSEL_Pos)|(4u<<COMP_CSR_INMSEL_Pos)|COMP_CSR_EN;
 delay_us(10);
 TIM1->CR1=0; TIM1->PSC=0; TIM1->ARR=239; TIM1->CCR1=current_ccr;
 TIM1->CR2=0; /* OIS1=0: inactive low; no inverted outputs. */
 TIM1->CCMR1=(6u<<TIM_CCMR1_OC1M_Pos)|TIM_CCMR1_OC1PE;
 TIM1->CCER=TIM_CCER_CC1E;
 TIM1->AF1=TIM1_AF1_BKINE|TIM1_AF1_BKINP|TIM1_AF1_BKCMP2E;
 /* Single unidirectional break: physical low OR internal comparator high.
  * BKF=0 asynchronous, AOE=0, BKBID=0, no LOCK until board qualification. */
 TIM1->BDTR=TIM_BDTR_BKE|TIM_BDTR_BKP|TIM_BDTR_OSSI|TIM_BDTR_OSSR;
 TIM1->EGR=TIM_EGR_UG; TIM1->SR=0; TIM1->DIER=0;
 GPIOA->OSPEEDR=(GPIOA->OSPEEDR&~(3u<<16u))|(1u<<16u); /* medium slew candidate */
 mode(GPIOA,8,2,6);
 TIM1->CR1=TIM_CR1_ARPE|TIM_CR1_CEN;
 NVIC_SetPriority(TIM1_BRK_TIM15_IRQn,0); HAL_NVIC_EnableIRQ(TIM1_BRK_TIM15_IRQn);
}
bool hw_start_acquisition(void) {
 hw_drop_permission(); threshold_ready=false; threshold_vdda=0;
 if(!fast_clock())return false;
 capture_init(); protection_init();
 if(!adc_init())return false;
 acquisition_started=HAL_GetTick(); acquiring=true; return true;
}
void hw_stop_acquisition(void) {
 hw_drop_permission();
 TIM1->DIER=0; TIM1->CR1=0; TIM2->CR1=0; TIM3->CR1=0;
 DMA1_Channel1->CCR=0; DMA1_Channel2->CCR=0; DMA1_Channel3->CCR=0; DMA1_Channel4->CCR=0;
 if(acquiring) HAL_ADC_Stop_DMA(&adc);
 COMP2->CSR=0; DAC3->CR=0; acquiring=false; threshold_ready=false;
 low_clock();
}
void hw_poll(uint32_t now) {
 if(!acquiring)return;
 const uint32_t ov=TIM_SR_CC1OF|TIM_SR_CC2OF;
 if(TIM2->SR&ov){cap_errors[0]++;TIM2->SR=~ov;BENCH_FAULT(1);app_fault_isr();}
 if(TIM3->SR&ov){cap_errors[2]++;TIM3->SR=~ov;BENCH_FAULT(2);app_fault_isr();}
 adc_summary_t a;
 hw_adc_summary(&a);
 now=HAL_GetTick(); /* Timestamp after the ISR-owned snapshot, never before it. */
 /* All division operands belong to one IRQ-protected record. Validate the
  * local VDDA again, even if a future producer accidentally sets valid=true. */
 bool usable=a.valid && !a.errors && a.raw_vref>0u && a.vdda_mv>=3000u && a.vdda_mv<=3600u &&
             (uint32_t)(now-a.last_ms)<=ADC_STALE_MS;
 if(!threshold_ready && usable && (uint32_t)(now-acquisition_started)>=10u) {
  threshold_vdda=a.vdda_mv;
  /* 1.250V nominal referred to rail / fitted 2:1 divider. Offset, DAC gain,
   * sense ratio and dynamic overshoot remain unqualified, not a ceiling. */
  threshold_code=(625u*4096u+threshold_vdda/2u)/threshold_vdda;
  if(threshold_code==0u || threshold_code>4095u){BENCH_FAULT(3);app_fault_isr();return;}
  DAC3->DHR12R2=threshold_code; delay_us(10);
  threshold_ready=DAC3->DOR2==threshold_code;
  if(!threshold_ready){BENCH_FAULT(4);app_fault_isr();}
 }
 if(threshold_ready && usable &&
   (a.vdda_mv>threshold_vdda+33u || a.vdda_mv+33u<threshold_vdda)){BENCH_FAULT(5);app_fault_isr();}
}
bool hw_prepare_source(void) {
 if(!acquiring || !threshold_ready || !hw_clock_ok() || hw_break_active() || hw_run() ||
    (GPIOB->ODR&GPIO_PIN_1))return false;
 TIM1->SR=~TIM_SR_BIF; TIM1->DIER|=TIM_DIER_BIE;
 TIM1->BDTR|=TIM_BDTR_MOE; /* U4 still disabled by HOST_PERMIT=0. */
 return (TIM1->BDTR&TIM_BDTR_MOE)!=0;
}
bool hw_pwm_ready(void) { return threshold_ready && (TIM1->BDTR&TIM_BDTR_MOE) && (TIM1->CR1&TIM_CR1_CEN); }
uint16_t hw_arm_diag(void) { return (uint16_t)arm_diag_stage | ((uint16_t)arm_diag_post << 8); }
bool hw_set_ccr(uint16_t value) {
 if(value<PWM_MIN_CCR || value>PWM_MAX_CCR)return false;
 TIM1->CCR1=value; current_ccr=value; /* OC1PE transfers only at next update. */
 return true;
}
void hw_set_permit(bool permit) {
#if PWMVID_ALLOW_FIXTURE
 if(permit && hw_pwm_ready() && !hw_break_active())GPIOB->BSRR=GPIO_PIN_1;
 else GPIOB->BSRR=(GPIO_PIN_1|GPIO_PIN_15)<<16u;
#else
 (void)permit; GPIOB->BSRR=(GPIO_PIN_1|GPIO_PIN_15)<<16u;
#endif
}
#if PWMVID_ALLOW_FIXTURE
static bool arm_lease_live(uint32_t heartbeat_ms,uint32_t lease_ms,uint32_t reserve_ms) {
 uint32_t elapsed=(uint32_t)(HAL_GetTick()-heartbeat_ms);
 return lease_ms>reserve_ms && elapsed<lease_ms-reserve_ms;
}
#endif
bool hw_arm_pulse(uint32_t epoch,uint32_t heartbeat_ms,uint32_t lease_ms) {
#if PWMVID_ALLOW_FIXTURE
 arm_diag_stage=1; arm_diag_post=0;
 if(!hw_pwm_ready() || !hw_clock_ok() || hw_break_active() || hw_run() ||
    !hw_vbus() || !usb_transport_connected() || !arm_lease_live(heartbeat_ms,lease_ms,0u))return false;
 arm_diag_stage=2;
 /* Allow clear release and source edge-watchdogs to settle, permission already
  * high but latch remains cleared until this one explicit rising clock edge. */
 uint32_t t=DWT->CYCCNT;
 while((uint32_t)(DWT->CYCCNT-t)<(SystemCoreClock/1000000u)*ARM_CLEAR_RELEASE_US) {
  if(usb_transport_loss_count()!=epoch || hw_break_active() || !(GPIOB->ODR&GPIO_PIN_1) ||
     !hw_vbus() || !arm_lease_live(heartbeat_ms,lease_ms,0u))return false;
 }
 arm_diag_stage=3;
 /* Each search watches up to two 240-tick PWM periods. Repeated full foreground
  * prechecks at one instruction cadence can alias the narrow safe window and
  * miss it on every attempt. CCR>=104 cannot satisfy the guarded window. */
 for(unsigned attempt=0;attempt<16;attempt++) {
  uint32_t old=__get_BASEPRI(); __set_BASEPRI_MAX(1u<<(8u-__NVIC_PRIO_BITS)); __DSB(); __ISB();
  bool eligible=hw_pwm_ready() && !hw_break_active() && !hw_run() &&
                hw_vbus() && (GPIOB->ODR&GPIO_PIN_1) && usb_transport_loss_count()==epoch;
  /* Reserve one tick for SysTick and this at-most-one-period masked wait.
   * Priority-0 break remains live and independently drops PERMIT. */
  eligible=eligible && arm_lease_live(heartbeat_ms,lease_ms,1u);
  if(!eligible) {__set_BASEPRI(old);return false;}
  if(arm_diag_stage<4u)arm_diag_stage=4;
  uint32_t phase_start=DWT->CYCCNT;
  for(unsigned poll=0;poll<256u;poll++) {
   if((poll&31u)==31u && (uint32_t)(DWT->CYCCNT-phase_start)>=PWM_PERIOD*2u)break;
   /* Priority-0 break drops PERMIT even while other interrupts are masked.
    * These fast register checks precede the timer sample so no long helper
    * chain consumes the narrow low-phase window after it is seen. */
   if(!(GPIOB->ODR&GPIO_PIN_1) || !(TIM1->BDTR&TIM_BDTR_MOE) ||
      (GPIOB->IDR&GPIO_PIN_14) || !(GPIOB->IDR&GPIO_PIN_5)) {
    __set_BASEPRI(old);return false;
   }
   uint32_t cnt=TIM1->CNT;
   if(cnt<=current_ccr+8u || cnt>=PWM_PERIOD-ARM_LOW_GUARD_TICKS)continue;
   if(GPIOA->IDR&GPIO_PIN_8) {arm_diag_stage=8;continue;}
   arm_diag_stage=5;
   GPIOB->BSRR=GPIO_PIN_15;
   uint32_t pulse=DWT->CYCCNT; while((uint32_t)(DWT->CYCCNT-pulse)<16u) {__NOP();}
   GPIOB->BSRR=GPIO_PIN_15<<16u;
   arm_diag_stage=6;
   __set_BASEPRI(old); delay_us(10);
   /* Discard earlier VRM-side observations. At least one new completed DMA
    * half per edge is required after this explicit gate-enable operation. */
   uint32_t p=__get_BASEPRI(); __set_BASEPRI_MAX(1u<<(8u-__NVIC_PRIO_BITS)); __DSB(); __ISB();
   cap_count[2]=0; cap_count[3]=0; cap_ms[2]=0; cap_ms[3]=0;
   __DMB(); __set_BASEPRI(p);
   bool run=hw_run(), no_break=!hw_break_active(), permit=(GPIOB->ODR&GPIO_PIN_1)!=0;
   bool vbus=hw_vbus(), usb=usb_transport_connected(), same_epoch=usb_transport_loss_count()==epoch;
   bool lease=arm_lease_live(heartbeat_ms,lease_ms,0u);
   arm_diag_post=(run?1u:0u)|(no_break?2u:0u)|(permit?4u:0u)|(vbus?8u:0u)|
                 (usb?16u:0u)|(same_epoch?32u:0u)|(lease?64u:0u);
   arm_diag_stage=7;
   return run && no_break && permit && vbus && usb && same_epoch && lease;
  }
  __set_BASEPRI(old);
 }
 return false;
#else
 (void)epoch; (void)heartbeat_ms; (void)lease_ms; return false;
#endif
}
static void cap_irq(unsigned i) {
 const uint32_t shift=i*4u, flags=(DMA1->ISR>>shift)&15u;
 DMA1->IFCR=(flags & 14u)<<shift; /* ES0431: no global CGIF clear. */
 if(flags&8u){cap_errors[i]++;BENCH_FAULT(6u+i);app_fault_isr();}
 if(flags&4u){cap_count[i]+=CAPTURE_SAMPLES/2;cap_ms[i]=HAL_GetTick();}
 if(flags&2u){cap_count[i]+=CAPTURE_SAMPLES/2;cap_ms[i]=HAL_GetTick();}
}
void DMA1_Channel1_IRQHandler(void){cap_irq(0);}
void DMA1_Channel2_IRQHandler(void){cap_irq(1);}
void DMA1_Channel3_IRQHandler(void){cap_irq(2);}
void DMA1_Channel4_IRQHandler(void){cap_irq(3);}
void DMA1_Channel5_IRQHandler(void){HAL_DMA_IRQHandler(&adc_dma);}
void ADC1_2_IRQHandler(void){HAL_ADC_IRQHandler(&adc);}
static void adc_snapshot(unsigned offset) {
 uint16_t raw=adc_data[offset], vr=adc_data[offset+2];
 uint32_t vdda=vr?((uint32_t)(*VREFINT_CAL_ADDR)*3000u/vr):0;
 uint32_t rail=(uint32_t)raw*vdda*2u/4095u;
 adc_result.raw_sense=raw; adc_result.raw_vref=vr;
 adc_result.vdda_mv=(uint16_t)vdda;adc_result.rail_mv=(uint16_t)rail;adc_result.last_ms=HAL_GetTick();
 adc_result.valid=!adc_result.errors && vr>0 && raw<4090u && vdda>=3000u && vdda<=3600u && rail<=1400u;
}
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *h){if(h==&adc)adc_snapshot(60);}
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *h){if(h==&adc)adc_snapshot(124);}
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *h){if(h==&adc){adc_result.errors++;adc_result.valid=false;BENCH_FAULT(10);app_fault_isr();}}
void hw_adc_summary(adc_summary_t *s) {
 uint32_t p=__get_BASEPRI();__set_BASEPRI_MAX(1u<<(8u-__NVIC_PRIO_BITS));__DSB();__ISB();
 memcpy(s,(const void *)&adc_result,sizeof(*s));__DMB();__set_BASEPRI(p);
}
void hw_capture_summary(unsigned stream,capture_summary_t *s) {
 uint32_t irq=__get_BASEPRI();__set_BASEPRI_MAX(1u<<(8u-__NVIC_PRIO_BITS));__DSB();__ISB();
 DMA_Channel_TypeDef *p=stream?DMA1_Channel3:DMA1_Channel1;
 DMA_Channel_TypeDef *h=stream?DMA1_Channel4:DMA1_Channel2;
 unsigned base=stream?2:0;
 uint32_t pi=(CAPTURE_SAMPLES-p->CNDTR+CAPTURE_SAMPLES-1)%CAPTURE_SAMPLES;
 uint32_t hi=(CAPTURE_SAMPLES-h->CNDTR+CAPTURE_SAMPLES-1)%CAPTURE_SAMPLES;
 s->period_ticks=stream?cap3_period[pi]:cap2_period[pi]; s->high_ticks=stream?cap3_high[hi]:cap2_high[hi];
 s->sample_count=cap_count[base];s->high_sample_count=cap_count[base+1];
 s->last_ms=(int32_t)(cap_ms[base]-cap_ms[base+1])<0?cap_ms[base]:cap_ms[base+1];
 s->errors=cap_errors[base]+cap_errors[base+1];
 s->valid=cap_count[base]>=64 && cap_count[base+1]>=64 && !s->errors &&
  s->period_ticks>=216 && s->period_ticks<=264 && s->high_ticks>0 && s->high_ticks<s->period_ticks;
 /* TIM3 is connected after the external output gate. A disabled node does
  * not qualify the internal source, even if old DMA observations remain. */
 if(stream && !hw_run())s->valid=false;
 __DMB();__set_BASEPRI(irq);
}
size_t hw_raw(char *out,size_t n) {
 /* 8 decimated period/high observations per stream. Independent DMA channels
  * are deliberately NOT presented as coherent paired cycles or all raw edges. */
 uint32_t values[2][2][8];
 for(unsigned stream=0;stream<2;stream++) {
  DMA_Channel_TypeDef *pd=stream?DMA1_Channel3:DMA1_Channel1;
  DMA_Channel_TypeDef *hd=stream?DMA1_Channel4:DMA1_Channel2;
  unsigned pi=(CAPTURE_SAMPLES-pd->CNDTR)%CAPTURE_SAMPLES;
  unsigned hi=(CAPTURE_SAMPLES-hd->CNDTR)%CAPTURE_SAMPLES;
  for(unsigned j=0;j<8;j++) {
   unsigned px=(pi+CAPTURE_SAMPLES-8+j)%CAPTURE_SAMPLES;
   unsigned hx=(hi+CAPTURE_SAMPLES-8+j)%CAPTURE_SAMPLES;
   values[stream][0][j]=stream?cap3_period[px]:cap2_period[px];
   values[stream][1][j]=stream?cap3_high[hx]:cap2_high[hx];
  }
 }
 if(!out || n==0u)return 0;
 int initial=pwm_snprintf(out,n,"OK RAW decimation=8 unpaired=1");
 if(initial<0 || (size_t)initial>=n)return 0;
 size_t at=(size_t)initial;
 for(unsigned stream=0;stream<2;stream++) for(unsigned edge=0;edge<2;edge++) {
  int k=pwm_snprintf(out+at,n-at," s%u_%c=",stream,edge?'h':'p');
  if(k<0 || (size_t)k>=n-at)return 0;
  at+=(size_t)k;
  for(unsigned j=0;j<8;j++) {
   k=pwm_snprintf(out+at,n-at,j?",%lu":"%lu",(unsigned long)values[stream][edge][j]);
   if(k<0 || (size_t)k>=n-at)return 0;
  at+=(size_t)k;
  }
 }
 int k=pwm_snprintf(out+at,n-at,"\n");if(k<0 || (size_t)k>=n-at)return 0;return at+(size_t)k;
}
void TIM1_BRK_TIM15_IRQHandler(void) {
 if(TIM1->SR&TIM_SR_BIF){BENCH_FAULT(11);hw_drop_permission();TIM1->DIER&=~TIM_DIER_BIE;TIM1->SR=~TIM_SR_BIF;break_count++;app_fault_isr();}
}
void hw_led(unsigned state, unsigned fault_code, uint32_t now_ms) {
 uint32_t a=0u,b=0u;
 if(state==PWM_DISARMED) {
  /* 125 Hz software PWM, eight brightness levels, 2.048 s breathing cycle.
   * Minimum is 1/8, so PASSIVE remains visible without a USB client. */
  uint32_t ramp=(now_ms>>7)&15u;
  uint32_t level=1u+(ramp<8u?ramp:15u-ramp);
  if((now_ms&7u)<level)b=GPIO_PIN_13;
 } else if(state==PWM_PREARM){a=GPIO_PIN_10;b=GPIO_PIN_13;}
 else if(state==PWM_ARMING)a=GPIO_PIN_9|GPIO_PIN_10;
 else if(state==PWM_ARMED){a=GPIO_PIN_9;b=GPIO_PIN_13;}
 else {
  /* 4.096 s repeating red code: one 512 ms long header, then N 128 ms
   * shorts separated by 128 ms. The spaces glow at 1/16 duty, 62.5 Hz, so
   * the fault remains visible without losing the bright long/short code.
   * Invalid/initialization code is seven. */
  unsigned code=(fault_code>=1u && fault_code<=6u)?fault_code:7u;
  uint32_t phase=now_ms&4095u, short_phase=phase-768u;
  if(phase<512u || (phase>=768u && (short_phase>>8)<code &&
                     (short_phase&255u)<128u) || (now_ms&15u)==0u)a=GPIO_PIN_9;
 }
 GPIOA->BSRR=a|(((GPIO_PIN_9|GPIO_PIN_10)&~a)<<16);
 GPIOB->BSRR=b|((GPIO_PIN_13&~b)<<16);
}
