#ifndef PWMVID_SEQUENCE_RUNTIME_MOCK_H
#define PWMVID_SEQUENCE_RUNTIME_MOCK_H
#include "control.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define PWM_PERIOD 240u
#define CAPTURE_STALE_MS 5u
#define TIM_DIER_UDE 1u
#define DMA_CCR_MINC 1u
#define DMA_CCR_CIRC 2u
#define DMA_CCR_HTIE 4u
#define DMA_CCR_TCIE 8u
#define DMA_CCR_TEIE 16u
#define DMA_CCR_PL_1 32u
#define DMA_CCR_PSIZE_0 64u
#define DMA_CCR_MSIZE_0 128u
#define DMA_CCR_EN 256u
#define DMA_CCR_DIR 512u
#define DMA_REQUEST_TIM1_UP 1u
#define DMA1_Channel6_IRQn 1
#define __NVIC_PRIO_BITS 4u
#define __HAL_RCC_DMA1_CLK_ENABLE() ((void)0)
#define __HAL_RCC_DMAMUX1_CLK_ENABLE() ((void)0)
#define __DSB() ((void)0)
#define __ISB() ((void)0)
#define __DMB() ((void)0)

typedef struct { uint32_t CNDTR, CCR, CPAR, CMAR; } mock_dma_channel_t;
typedef struct { uint32_t ISR, IFCR; } mock_dma_t;
typedef struct { uint32_t CCR; } mock_mux_channel_t;
typedef struct { uint32_t DIER, CCR1; } mock_timer_t;
extern mock_dma_channel_t *DMA1_Channel6;
extern mock_dma_t *DMA1;
extern mock_mux_channel_t *DMAMUX1_Channel5;
extern mock_timer_t *TIM1;
static inline uint32_t __get_BASEPRI(void) { return 0; }
static inline void __set_BASEPRI_MAX(uint32_t value) { (void)value; }
static inline void __set_BASEPRI(uint32_t value) { (void)value; }
static inline void NVIC_SetPriority(int irq, int priority) { (void)irq; (void)priority; }
static inline void HAL_NVIC_EnableIRQ(int irq) { (void)irq; }

typedef struct {
 uint32_t period_ticks, high_ticks, sample_count, high_sample_count, last_ms;
 bool valid;
} capture_summary_t;
bool hw_run(void);
bool hw_pwm_ready(void);
void hw_drop_permission(void);
void app_fault_isr(void);
void ctrl_fault_isr(pwm_control_t *control);
#endif
