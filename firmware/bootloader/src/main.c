/* Isolated USB-only updater: never enter ST factory ROM on P8/P9. */
#include "stm32g4xx_hal.h"
#include "image.h"
extern bool boot_usb_start(void);
extern void boot_usb_poll(void);
static volatile bool loader_led_active;
static void safe_pins(void){
 RCC->AHB2ENR|=RCC_AHB2ENR_GPIOAEN|RCC_AHB2ENR_GPIOBEN;(void)RCC->AHB2ENR;
 GPIOB->BSRR=GPIO_PIN_13|((GPIO_PIN_1|GPIO_PIN_15)<<16u);
 GPIOA->BSRR=GPIO_PIN_9|(GPIO_PIN_10<<16u); /* Magenta: custom loader. */
 GPIOB->MODER=(GPIOB->MODER&~((3u<<2)|(3u<<26)|(3u<<30)))|(1u<<2)|(1u<<26)|(1u<<30);
 GPIOB->PUPDR&=~((3u<<2)|(3u<<26)|(3u<<28)|(3u<<30));
 GPIOA->MODER=(GPIOA->MODER&~(3u<<18))|(1u<<18);
 GPIOA->PUPDR&=~(3u<<18);
 const unsigned pins[]={0,2,3,6,7,8};
 for(unsigned i=0;i<sizeof(pins)/sizeof(pins[0]);i++){
  GPIOA->MODER|=3u<<(2u*pins[i]);GPIOA->PUPDR&=~(3u<<(2u*pins[i]));
 }
}
static void recovery_reset(void){
 volatile uint32_t *cookie=(volatile uint32_t *)PWMVID_BOOT_COOKIE_ADDRESS;
 cookie[0]=PWMVID_BOOT_COOKIE_MAGIC;cookie[1]=~PWMVID_BOOT_COOKIE_MAGIC;
 __DSB();NVIC_SystemReset();
}
void SysTick_Handler(void){
 HAL_IncTick();
 if(loader_led_active){
  uint32_t t=HAL_GetTick();
  /* Orange: red on for 512 ms, green at 25% within the on interval.
   * The other 512 ms are dark. No USB or flash loop is delayed. */
  uint32_t a=(t&1023u)<512u?GPIO_PIN_9:0u;
  if(a && (t&7u)<2u)a|=GPIO_PIN_10;
  GPIOA->BSRR=a|(((GPIO_PIN_9|GPIO_PIN_10)&~a)<<16);
 }
}
void NMI_Handler(void){
 safe_pins();
 uint32_t ecc=FLASH->ECCR;
 uint32_t address=0x08000000u+(ecc&FLASH_ECCR_ADDR_ECC);
 /* RM0440 section5.3.2: torn application doublewords may generate ECC NMI.
  * Do not resume or trust that read. Reset with a one-shot USB-recovery cookie,
  * so next boot skips all application reads and waits for a fresh download. */
 if((ecc&FLASH_ECCR_ECCD) && !(ecc&FLASH_ECCR_SYSF_ECC) &&
    address>=APP_BASE && address<FLASH_END)recovery_reset();
 for(;;){} /* Boot-region or unrelated NMI requires ST-Link investigation. */
}
void HardFault_Handler(void){safe_pins();for(;;){} }
__attribute__((naked,noreturn)) static void branch_app(uint32_t sp __attribute__((unused)),uint32_t pc __attribute__((unused))){
 __asm volatile("msr msp, r0\n bx r1\n");
}
static bool watchdog_start(void){
 RCC->CSR|=RCC_CSR_LSION;
 uint32_t t=HAL_GetTick();
 while(!(RCC->CSR&RCC_CSR_LSIRDY))if(HAL_GetTick()-t>100u)return false;
 IWDG->KR=0xcccc;IWDG->KR=0x5555;IWDG->PR=4;IWDG->RLR=2047;
 t=HAL_GetTick();
 while(IWDG->SR)if(HAL_GetTick()-t>100u)return false;
 IWDG->KR=0xaaaa;return true; /* about4s nominal; LSI tolerance applies */
}
static bool take_boot_request(void){
 volatile uint32_t *cookie=(volatile uint32_t *)PWMVID_BOOT_COOKIE_ADDRESS;
 bool requested=(RCC->CSR&RCC_CSR_SFTRSTF) && cookie[0]==PWMVID_BOOT_COOKIE_MAGIC && cookie[1]==~PWMVID_BOOT_COOKIE_MAGIC;
 cookie[0]=0;cookie[1]=0;__DSB();
 return requested;
}
int main(void){
 safe_pins();
 bool requested=take_boot_request();
 FLASH->ECCR|=FLASH_ECCR_ECCC|FLASH_ECCR_ECCD;
 if(!requested && image_valid() && !(FLASH->ECCR&FLASH_ECCR_ECCC)){
  __disable_irq();SCB->VTOR=APP_BASE;__DSB();__ISB();
  uint32_t *v=(uint32_t *)APP_BASE;
  __enable_irq();branch_app(v[0],v[1]);
 }
 RCC->CSR|=RCC_CSR_RMVF; /* Preserve reset history when handing directly to the app. */
 HAL_Init();HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
 __HAL_RCC_PWR_CLK_ENABLE();__HAL_RCC_SYSCFG_CLK_ENABLE();
 if(!(FLASH->OPTR&FLASH_OPTR_IWDG_SW) || !watchdog_start())for(;;){};
 RCC->CRRCR|=RCC_CRRCR_HSI48ON;
 uint32_t start=HAL_GetTick();
 while(!(RCC->CRRCR&RCC_CRRCR_HSI48RDY))if(HAL_GetTick()-start>100u)for(;;){};
 MODIFY_REG(RCC->CCIPR,RCC_CCIPR_CLK48SEL,0);
 __HAL_RCC_CRS_CLK_ENABLE();
 CRS->CFGR=(47999u<<CRS_CFGR_RELOAD_Pos)|(34u<<CRS_CFGR_FELIM_Pos)|(2u<<CRS_CFGR_SYNCSRC_Pos);
 CRS->CR=CRS_CR_AUTOTRIMEN|CRS_CR_CEN|(32u<<CRS_CR_TRIM_Pos);
 if(!boot_usb_start())for(;;){};
 GPIOB->BSRR=GPIO_PIN_13<<16u;
 loader_led_active=true;
 for(;;){boot_usb_poll();IWDG->KR=0xaaaa;__WFI();}
}
