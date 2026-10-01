#!/usr/bin/env python3
"""Compile actual hardware.c function bodies against narrow register/HAL models.

These are regression tests for software sequencing and boundaries, not a model
of STM32 bus timing, analog behavior, USB enumeration, or electrical safety.
The separately built target firmware remains the authoritative compile check.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

SOURCE = (Path(__file__).resolve().parents[2] / "src/hardware.c").read_text()

def extract_function(name):
    match = re.search(r"^(?:static )?(?:void|bool) " + re.escape(name) + r"\(", SOURCE, re.M)
    if not match:
        raise RuntimeError("Function definition missing: " + name)
    start = match.start()
    end = SOURCE.index("{", start) + 1
    depth = 1
    while depth:
        if SOURCE[end] == "{":
            depth += 1
        elif SOURCE[end] == "}":
            depth -= 1
        end += 1
    return SOURCE[start:end] + "\n"

CASES = [
    ('hw', ['hw_adc_summary', 'hw_poll', 'arm_lease_live'], r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define __NVIC_PRIO_BITS 4
#define ADC_STALE_MS 10u
#define TIM_SR_CC1OF 1u
#define TIM_SR_CC2OF 2u
#define BENCH_FAULT(code) ((void)0)
typedef struct { uint16_t raw_sense, raw_vref, vdda_mv, rail_mv; uint32_t last_ms, errors; bool valid; } adc_summary_t;
static volatile adc_summary_t adc_result;
static bool acquiring=true, threshold_ready;
static uint32_t threshold_code, acquisition_started, cap_errors[4], tick;
static uint16_t threshold_vdda;
static struct {uint32_t SR;} t2,t3;
#define TIM2 (&t2)
#define TIM3 (&t3)
static struct {uint32_t DHR12R2,DOR2;} dac;
#define DAC3 (&dac)
static uint32_t mask,faults;
static bool replace_on_unmask;
static uint32_t __get_BASEPRI(void){return mask;}
static void __set_BASEPRI_MAX(uint32_t p){if(!mask || p<mask)mask=p;}
static void __set_BASEPRI(uint32_t p){mask=p;if(!p && replace_on_unmask){replace_on_unmask=false;adc_result.valid=false;adc_result.vdda_mv=0;adc_result.raw_vref=0;}}
#define __DSB() ((void)0)
#define __DMB() ((void)0)
#define __ISB() ((void)0)
static void delay_us(uint32_t us){(void)us;dac.DOR2=dac.DHR12R2;}
static uint32_t HAL_GetTick(void){return tick;}
static void app_fault_isr(void){faults++;}
""", r"""
static void reset(void){
 adc_summary_t a={.raw_sense=600,.raw_vref=1500,.vdda_mv=3300,.rail_mv=967,.last_ms=20,.valid=true};
 adc_result=a;threshold_ready=false;threshold_code=0;threshold_vdda=0;tick=20;acquisition_started=0;mask=0;faults=0;dac.DOR2=0;dac.DHR12R2=0;replace_on_unmask=false;
}
int main(void){
 reset(); replace_on_unmask=true; hw_poll(0); assert(!adc_result.valid && adc_result.vdda_mv==0);assert(threshold_ready && threshold_code==776 && threshold_vdda==3300); /* torn producer after snapshot cannot divide by zero */
 reset();adc_result.vdda_mv=0;hw_poll(20);assert(!threshold_ready && !dac.DHR12R2);
 reset();adc_result.raw_vref=0;hw_poll(20);assert(!threshold_ready);
 reset();adc_result.vdda_mv=2999;hw_poll(20);assert(!threshold_ready);
 reset();adc_result.vdda_mv=3601;hw_poll(20);assert(!threshold_ready);
 reset();adc_result.errors=1;hw_poll(20);assert(!threshold_ready);
 reset();adc_result.last_ms=9;hw_poll(20);assert(!threshold_ready);
 reset();acquisition_started=15;hw_poll(0xffffffff);assert(!threshold_ready); /* stale caller time cannot bypass settle */
 reset();hw_poll(0);assert(threshold_ready);adc_result.vdda_mv=3334;hw_poll(20);assert(faults==1);
 reset();mask=8;adc_summary_t a;hw_adc_summary(&a);assert(mask==8 && a.valid && a.vdda_mv==3300);
 tick=100;assert(!arm_lease_live(100,1,1));assert(arm_lease_live(100,2,1));
 tick=101;assert(!arm_lease_live(100,2,1));assert(arm_lease_live(100,2,0));
 tick=102;assert(!arm_lease_live(100,2,0));
 tick=0;assert(arm_lease_live(UINT32_MAX-3,10,1));assert(!arm_lease_live(UINT32_MAX-3,4,0));
 puts("PASS: actual-source ADC snapshot/zero/stale/range/settle/drift/mask and ARM expiry/reserve/wrap tests");
}
"""),
    ('clock', ['hclk_divider', 'low_clock', 'fast_clock', 'hw_clock_ok'], r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define HAL_OK 0
#define RCC_CLOCKTYPE_HCLK 1
#define RCC_CLOCKTYPE_SYSCLK 2
#define RCC_CLOCKTYPE_PCLK1 4
#define RCC_CLOCKTYPE_PCLK2 8
#define RCC_SYSCLKSOURCE_HSI 0
#define RCC_SYSCLKSOURCE_PLLCLK 2
#define RCC_CFGR_SWS_HSI 0
#define RCC_CFGR_SWS_PLL 8
#define RCC_CFGR_SWS 12
#define RCC_CFGR_HPRE 0xf0
#define RCC_SYSCLK_DIV1 0
#define RCC_SYSCLK_DIV2 0x80
#define RCC_CFGR_PPRE1 0x700
#define RCC_CFGR_PPRE2 0x3800
#define RCC_HCLK_DIV1 0
#define FLASH_LATENCY_0 0
#define FLASH_LATENCY_4 4
#define FLASH_ACR_LATENCY 15
#define RCC_CR_PLLON 1
#define RCC_CR_HSEON 2
#define RCC_CR_CSSON 4
#define RCC_CR_PLLRDY 8
#define RCC_CR_HSERDY 16
#define RCC_CRRCR_HSI48RDY 1
#define PWR_CR1_VOS 3
#define PWR_CR5_R1MODE 1
#define PWR_SR2_VOSF 1
#define PWR_REGULATOR_VOLTAGE_SCALE1 1
#define PWR_REGULATOR_VOLTAGE_SCALE1_BOOST 0
#define RCC_OSCILLATORTYPE_HSE 1
#define RCC_HSE_ON 1
#define RCC_PLL_ON 1
#define RCC_PLLSOURCE_HSE 1
#define RCC_PLLM_DIV2 2
#define RCC_PLLR_DIV2 2
#define RCC_PLLP_DIV2 2
#define RCC_PLLQ_DIV2 2
#define __DSB() ((void)0)
#define __ISB() ((void)0)
typedef struct { uint32_t ClockType,SYSCLKSource,AHBCLKDivider,APB1CLKDivider,APB2CLKDivider; } RCC_ClkInitTypeDef;
typedef struct { uint32_t OscillatorType,HSEState;struct{uint32_t PLLState,PLLSource,PLLM,PLLN,PLLR,PLLP,PLLQ;} PLL;} RCC_OscInitTypeDef;
static struct{uint32_t CFGR,CR,CRRCR;} rcc;
#define RCC (&rcc)
static struct{uint32_t ACR;} flash;
#define FLASH (&flash)
static struct{uint32_t CR1,CR5,SR2;} pwr;
#define PWR (&pwr)
static uint32_t SystemCoreClock=16000000u;
static bool high_clock,fail_divider,fail_boost,fail_pll;
static unsigned waits,configs;
static uint32_t wait_hz[4];
static int HAL_RCC_ClockConfig(RCC_ClkInitTypeDef *c,uint32_t latency){
 configs++;if(fail_divider)return 1;
 if(c->ClockType&RCC_CLOCKTYPE_SYSCLK)rcc.CFGR=(rcc.CFGR&~RCC_CFGR_SWS)|(c->SYSCLKSource<<2);
 if(c->ClockType&RCC_CLOCKTYPE_HCLK)rcc.CFGR=(rcc.CFGR&~RCC_CFGR_HPRE)|c->AHBCLKDivider;
 SystemCoreClock=((rcc.CFGR&RCC_CFGR_SWS)==RCC_CFGR_SWS_PLL?162000000u:16000000u)/((rcc.CFGR&RCC_CFGR_HPRE)==RCC_SYSCLK_DIV2?2u:1u);
 flash.ACR=latency;return HAL_OK;
}
static int HAL_PWREx_ControlVoltageScaling(uint32_t n){(void)n;assert(SystemCoreClock==8000000u);if(fail_boost)return 1;pwr.CR1=1;pwr.CR5=0;return HAL_OK;}
static int HAL_RCC_OscConfig(RCC_OscInitTypeDef *o){assert(o->PLL.PLLN==81 && o->PLL.PLLM==2 && o->PLL.PLLR==2);if(fail_pll)return 1;rcc.CR=31;return HAL_OK;}
static void HAL_RCC_EnableCSS(void){rcc.CR|=RCC_CR_CSSON;}
static void delay_us(uint32_t n){assert(n==2u);assert(waits<4);wait_hz[waits++]=SystemCoreClock;}
bool hw_clock_ok(void);
""", r"""
static void reset(void){rcc.CFGR=0;rcc.CR=0;rcc.CRRCR=1;pwr.CR1=1;pwr.CR5=1;pwr.SR2=0;flash.ACR=0;SystemCoreClock=16000000u;high_clock=false;waits=0;configs=0;fail_divider=fail_boost=fail_pll=false;}
int main(void){
 reset();assert(fast_clock());assert(hw_clock_ok());assert(waits==1 && wait_hz[0]==81000000u && configs==3);
 assert(low_clock());assert(!high_clock && SystemCoreClock==16000000u && flash.ACR==0);assert(waits==3 && wait_hz[1]==81000000u && wait_hz[2]==8000000u && configs==6);
 reset();assert(low_clock());assert(waits==0 && configs==1);
 reset();fail_divider=true;assert(!fast_clock() && !high_clock);
 reset();fail_boost=true;assert(!fast_clock() && !high_clock);
 reset();fail_pll=true;assert(!fast_clock() && !high_clock);
 reset();assert(fast_clock());rcc.CFGR|=0x100;assert(!hw_clock_ok());
 puts("PASS: actual-source explicit divided clock phases/readbacks and fail-closed initialization tests");
}
"""),
    ('arm', ['arm_lease_live', 'hw_arm_pulse'], r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define PWMVID_ALLOW_FIXTURE 1
#define __NVIC_PRIO_BITS 4
#define GPIO_PIN_1 2u
#define GPIO_PIN_5 32u
#define GPIO_PIN_8 256u
#define GPIO_PIN_14 16384u
#define GPIO_PIN_15 32768u
#define TIM_BDTR_MOE 1u
#define PWM_PERIOD 240u
#define ARM_LOW_GUARD_TICKS 128u
#define ARM_CLEAR_RELEASE_US 1000u
#define __DSB() ((void)0)
#define __DMB() ((void)0)
#define __ISB() ((void)0)
#define __NOP() ((void)0)
static struct {uint32_t ODR,IDR,BSRR;} gpioa,gpiob;
#define GPIOA (&gpioa)
#define GPIOB (&gpiob)
static struct {uint32_t CNT,BDTR;} timer;
#define TIM1 (&timer)
struct mock_dwt_s {uint32_t CYCCNT;};
static struct mock_dwt_s dwt;
static bool live,connected,break_fault,ready,clock_ok,vbus,refuse_run;
static uint32_t pulse_count,loss_cycle,break_cycle,tick_origin,mask;
static volatile uint8_t arm_diag_stage,arm_diag_post;
static uint16_t current_ccr=64;
static volatile uint32_t cap_count[4],cap_ms[4];
static const uint32_t SystemCoreClock=162000000u;
static void sync_io(void){
 if(gpiob.BSRR==GPIO_PIN_15){pulse_count++;if(!refuse_run && (gpiob.ODR&GPIO_PIN_1) && !break_fault)live=true;gpiob.BSRR=0;}
 if(gpiob.BSRR==(GPIO_PIN_15<<16u))gpiob.BSRR=0;
 if(break_cycle && dwt.CYCCNT>=break_cycle){break_fault=true;live=false;gpiob.ODR=0;}
}
static struct mock_dwt_s *mock_dwt(void){sync_io();dwt.CYCCNT+=162;return &dwt;}
#define DWT mock_dwt()
static uint32_t HAL_GetTick(void){return tick_origin+dwt.CYCCNT/162000u;}
static uint32_t __get_BASEPRI(void){return mask;}
static void __set_BASEPRI_MAX(uint32_t p){if(!mask || p<mask)mask=p;}
static void __set_BASEPRI(uint32_t p){mask=p;}
static bool hw_pwm_ready(void){return ready;}
static bool hw_clock_ok(void){return clock_ok;}
static bool hw_break_active(void){sync_io();return break_fault;}
static bool hw_run(void){sync_io();return live;}
static bool hw_vbus(void){return vbus;}
static bool usb_transport_connected(void){return connected;}
static uint32_t usb_transport_loss_count(void){return loss_cycle && dwt.CYCCNT>=loss_cycle?1:0;}
static void delay_us(uint32_t us){sync_io();dwt.CYCCNT+=162u*us;}
""", r"""
static void reset(void){
 gpioa.IDR=0;gpiob.IDR=GPIO_PIN_5;gpiob.ODR=GPIO_PIN_1;gpiob.BSRR=0;timer.CNT=100;timer.BDTR=TIM_BDTR_MOE;dwt.CYCCNT=0;
 live=false;connected=ready=clock_ok=vbus=true;break_fault=refuse_run=false;
 pulse_count=loss_cycle=break_cycle=mask=0;tick_origin=100;current_ccr=64;
 for(unsigned i=0;i<4;i++){cap_count[i]=99;cap_ms[i]=20;}
}
int main(void){
 reset();assert(hw_arm_pulse(0,100,250));assert(pulse_count==1 && live && mask==0 && cap_count[2]==0 && cap_count[3]==0);
 reset();assert(!hw_arm_pulse(0,100,1));assert(pulse_count==0 && !live && mask==0);
 reset();assert(!hw_arm_pulse(0,100,2));assert(pulse_count==0 && !live && mask==0);
 reset();assert(hw_arm_pulse(0,100,3));assert(pulse_count==1);
 reset();assert(!hw_arm_pulse(0,99,1));assert(pulse_count==0);
 reset();loss_cycle=50000;assert(!hw_arm_pulse(0,100,250));assert(pulse_count==0);
 reset();break_cycle=50000;assert(!hw_arm_pulse(0,100,250));assert(pulse_count==0);
 reset();gpiob.ODR=0;assert(!hw_arm_pulse(0,100,250));assert(pulse_count==0);
 reset();vbus=false;assert(!hw_arm_pulse(0,100,250));assert(pulse_count==0);
 reset();clock_ok=false;assert(!hw_arm_pulse(0,100,250));assert(pulse_count==0);
 reset();timer.CNT=120;assert(!hw_arm_pulse(0,100,250));assert(pulse_count==0 && mask==0);
 reset();refuse_run=true;assert(!hw_arm_pulse(0,100,250));assert(pulse_count==1 && !live);
 reset();tick_origin=UINT32_MAX;assert(hw_arm_pulse(0,UINT32_MAX,250));assert(pulse_count==1);
 puts("PASS: actual-source ARM sequence success/expiry/loss/break/permit/vbus/clock/phase/RUN/wrap tests");
}
"""),
    ('led', ['mode', 'hw_early_safe', 'hw_led'], r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define GPIO_PIN_1 (1u<<1)
#define GPIO_PIN_9 (1u<<9)
#define GPIO_PIN_10 (1u<<10)
#define GPIO_PIN_13 (1u<<13)
#define GPIO_PIN_15 (1u<<15)
#define RCC_AHB2ENR_GPIOAEN 1u
#define RCC_AHB2ENR_GPIOBEN 2u
#define PWM_DISARMED 0u
#define PWM_PREARM 1u
#define PWM_ARMING 2u
#define PWM_ARMED 3u
#define PWM_FAULT 4u
typedef struct {uint32_t MODER,PUPDR,AFR[2],BSRR;} GPIO_TypeDef;
static GPIO_TypeDef gpioa,gpiob;
#define GPIOA (&gpioa)
#define GPIOB (&gpiob)
static struct {uint32_t AHB2ENR;} rcc;
#define RCC (&rcc)
""", r"""
static void expect(unsigned state,unsigned code,uint32_t ms,uint32_t a,uint32_t b){
 gpioa.BSRR=gpiob.BSRR=0;hw_led(state,code,ms);
 assert(gpioa.BSRR==a && gpiob.BSRR==b);
}
int main(void){
 hw_early_safe();
 assert((rcc.AHB2ENR&3u)==3u);
 assert(((gpioa.MODER>>(9u*2u))&3u)==1u);
 assert(((gpioa.MODER>>(10u*2u))&3u)==1u);
 assert(((gpiob.MODER>>(13u*2u))&3u)==1u);
 assert(gpiob.BSRR==GPIO_PIN_13); /* Visible before a USB client exists. */
 expect(PWM_DISARMED,0,0,(GPIO_PIN_9|GPIO_PIN_10)<<16u,GPIO_PIN_13);
 expect(PWM_DISARMED,0,1,(GPIO_PIN_9|GPIO_PIN_10)<<16u,GPIO_PIN_13<<16u);
 expect(PWM_DISARMED,0,129,(GPIO_PIN_9|GPIO_PIN_10)<<16u,GPIO_PIN_13);
 expect(PWM_PREARM,0,0,(GPIO_PIN_9<<16u)|GPIO_PIN_10,GPIO_PIN_13);
 expect(PWM_ARMING,0,0,GPIO_PIN_9|GPIO_PIN_10,GPIO_PIN_13<<16u);
 expect(PWM_ARMED,0,0,GPIO_PIN_9|(GPIO_PIN_10<<16u),GPIO_PIN_13);
 expect(PWM_FAULT,2,0,GPIO_PIN_9|(GPIO_PIN_10<<16u),GPIO_PIN_13<<16u);
 expect(PWM_FAULT,2,592,GPIO_PIN_9|(GPIO_PIN_10<<16u),GPIO_PIN_13<<16u);
 expect(PWM_FAULT,2,600,(GPIO_PIN_9|GPIO_PIN_10)<<16u,GPIO_PIN_13<<16u);
 expect(PWM_FAULT,2,601,(GPIO_PIN_9|GPIO_PIN_10)<<16u,GPIO_PIN_13<<16u);
 expect(PWM_FAULT,2,768,GPIO_PIN_9|(GPIO_PIN_10<<16u),GPIO_PIN_13<<16u);
 expect(PWM_FAULT,2,1024,GPIO_PIN_9|(GPIO_PIN_10<<16u),GPIO_PIN_13<<16u);
 expect(PWM_FAULT,2,1280,GPIO_PIN_9|(GPIO_PIN_10<<16u),GPIO_PIN_13<<16u);
 expect(PWM_FAULT,2,1281,(GPIO_PIN_9|GPIO_PIN_10)<<16u,GPIO_PIN_13<<16u);
 puts("PASS: D1 breathing, state colors, and latched fault pulse code");
}
"""),
 ]

def main():
    with tempfile.TemporaryDirectory(prefix="pwmvid-hardware-tests-") as temporary:
        directory = Path(temporary)
        for name, functions, head, tests in CASES:
            source = directory / (name + ".c")
            binary = directory / name
            source.write_text(head + "".join(extract_function(n) for n in functions) + tests)
            command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(source), "-o", str(binary)]
            subprocess.run(command, check=True)
            environment = dict(os.environ)
            environment["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(binary)], check=True, env=environment)

if __name__ == "__main__":
    main()
