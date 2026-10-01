/* Minimal EP0 engine on the pinned, single-buffer ST HAL USB FS driver. */
#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_pcd_ex.h"
#include "ep0_dfu.h"
static PCD_HandleTypeDef pcd;
static uint8_t rx_packet[64];
void ep0_port_send(const uint8_t *p,uint16_t n){
 if(HAL_PCD_EP_Transmit(&pcd,0x80,(uint8_t *)p,n)!=HAL_OK)ep0_port_stall();
}
void ep0_port_receive(void){
 /* Always provide one complete packet of storage, including status OUT. */
 if(HAL_PCD_EP_Receive(&pcd,0,rx_packet,sizeof(rx_packet))!=HAL_OK)ep0_port_stall();
}
void ep0_port_stall(void){HAL_PCD_EP_SetStall(&pcd,0);HAL_PCD_EP_SetStall(&pcd,0x80);}
void ep0_port_address(uint8_t a){
 pcd.USB_Address=0;pcd.Instance->DADDR=USB_DADDR_EF|a;
}
void ep0_port_leave(void){HAL_PCD_Stop(&pcd);NVIC_SystemReset();}
uint32_t ep0_port_uid(unsigned w){return ((volatile uint32_t *)UID_BASE)[w];}
void HAL_PCD_MspInit(PCD_HandleTypeDef *h){
 (void)h;__HAL_RCC_GPIOA_CLK_ENABLE();__HAL_RCC_USB_CLK_ENABLE();
 /* USB DM/DP dedicated additional function; GPIO stays analog/no pull. */
 GPIOA->MODER|=(3u<<22)|(3u<<24);GPIOA->PUPDR&=~((3u<<22)|(3u<<24));
 HAL_NVIC_SetPriority(USB_LP_IRQn,2u,0u);HAL_NVIC_EnableIRQ(USB_LP_IRQn);
}
void USB_LP_IRQHandler(void){
 /* Enumerating hosts can issue the next control request immediately after a
  * status stage. Service EP0 in the IRQ rather than waiting for the main loop. */
 if(pcd.Instance->ISTR&USB_ISTR_RESET){
  /* Reset wins over stale endpoint completions, unlike generic HAL ordering. */
  PCD_CLEAR_RX_EP_CTR(pcd.Instance,0);PCD_CLEAR_TX_EP_CTR(pcd.Instance,0);
  __HAL_PCD_CLEAR_FLAG(&pcd,USB_ISTR_RESET);
  HAL_PCD_ResetCallback(&pcd);ep0_port_address(0);
 }else {
  /* The pinned HAL rearms EP0 on a zero-length status OUT without invoking
   * DataOutStageCallback. Notify the protocol engine before servicing a
   * possibly queued next SETUP, so media actions wait for the full handshake. */
  uint16_t istr=pcd.Instance->ISTR;
  if((istr&USB_ISTR_CTR) && (istr&USB_ISTR_DIR) && !(istr&USB_ISTR_EP_ID)){
   uint16_t ep=PCD_GET_ENDPOINT(pcd.Instance,0);
   if((ep&USB_EP_CTR_RX) && !(ep&USB_EP_SETUP) &&
      !PCD_GET_EP_RX_CNT(pcd.Instance,0))ep0_out(NULL,0);
  }
  HAL_PCD_IRQHandler(&pcd);
 }
}
void boot_usb_poll(void){
 /* Keep application-page erase/program and DFU state transitions outside the
  * USB ISR. Mask only this IRQ while consuming its pending media action. */
 NVIC_DisableIRQ(USB_LP_IRQn);
 if(!(pcd.Instance->ISTR&USB_ISTR_RESET))ep0_poll();
 NVIC_EnableIRQ(USB_LP_IRQn);
}
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *h){
 /* A fresh SETUP supersedes any completion from the previous transfer. */
 PCD_CLEAR_TX_EP_CTR(h->Instance,0);h->USB_Address=0;
 h->OUT_ep[0].xfer_buff=rx_packet;
 ep0_setup((const uint8_t *)h->Setup);
}
void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *h,uint8_t endpoint){
 if(endpoint==0u){
  /* Generic HAL advances this pointer even for a single packet. Reset it
   * before a status phase or an unexpected subsequent packet can arrive. */
  h->OUT_ep[0].xfer_buff=rx_packet;
  ep0_out(rx_packet,(uint16_t)HAL_PCD_EP_GetRxCount(h,0));
 }
}
void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *h,uint8_t endpoint){
 (void)h;if(endpoint==0u)ep0_in_complete();
}
void HAL_PCD_ResetCallback(PCD_HandleTypeDef *h){
 HAL_PCD_EP_Open(h,0,64,EP_TYPE_CTRL);HAL_PCD_EP_Open(h,0x80,64,EP_TYPE_CTRL);
 ep0_reset();
}
bool boot_usb_start(void){
 pcd.Instance=USB;pcd.Init.dev_endpoints=1u;pcd.Init.speed=PCD_SPEED_FULL;
 pcd.Init.phy_itface=PCD_PHY_EMBEDDED;
 if(HAL_PCD_Init(&pcd)!=HAL_OK ||
    HAL_PCDEx_PMAConfig(&pcd,0,PCD_SNG_BUF,0x40)!=HAL_OK ||
    HAL_PCDEx_PMAConfig(&pcd,0x80,PCD_SNG_BUF,0x80)!=HAL_OK ||
    HAL_PCD_Start(&pcd)!=HAL_OK)return false;
 CLEAR_BIT(pcd.Instance->CNTR,USB_CNTR_SOFM);return true;
}
