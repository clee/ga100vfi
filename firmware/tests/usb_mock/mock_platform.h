#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "usb_transport.h"
#define USE_HAL_PCD_REGISTER_CALLBACKS 0U
#define CDC_DATA_FS_MAX_PACKET_SIZE 64U
#define CDC_IN_EP 0x81U
#define CDC_OUT_EP 0x01U
#define CDC_CMD_EP 0x82U
#define __NVIC_PRIO_BITS 4U
#define USBD_STATE_CONFIGURED 3U
#define USBD_OK 0U
#define USBD_FAIL 1U
#define DEVICE_FS 0U
#define CDC_SET_LINE_CODING 0x20U
#define CDC_GET_LINE_CODING 0x21U
#define CDC_SET_CONTROL_LINE_STATE 0x22U
#define CDC_SEND_BREAK 0x23U
#define __DSB() ((void)0)
#define __ISB() ((void)0)
#define __DMB() ((void)0)
static uint32_t fake_basepri;
static uint32_t __get_BASEPRI(void) { return fake_basepri; }
static void __set_BASEPRI_MAX(uint32_t x) { if (!fake_basepri || x < fake_basepri) fake_basepri=x; }
static void __set_BASEPRI(uint32_t x) { fake_basepri=x; }
typedef struct { uint8_t dev_state; } USBD_HandleTypeDef;
typedef struct { int unused; } PCD_HandleTypeDef;
typedef struct { uint32_t data[128]; volatile uint32_t TxState; } USBD_CDC_HandleTypeDef;
typedef struct { uint8_t bmRequest, bRequest; uint16_t wValue, wIndex, wLength; } USBD_SetupReqTypedef;
typedef struct { int8_t (*Init)(void); int8_t (*DeInit)(void); int8_t (*Control)(uint8_t,uint8_t*,uint16_t); int8_t (*Receive)(uint8_t*,uint32_t*); } USBD_CDC_ItfTypeDef;
static int pwmvid_usb_descriptors, USBD_CDC;
static uint8_t USBD_CDC_SetTxBuffer(USBD_HandleTypeDef*a,uint8_t*b,uint16_t c) {(void)a;(void)b;(void)c;return 0;}
static uint8_t USBD_CDC_SetRxBuffer(USBD_HandleTypeDef*a,uint8_t*b) {(void)a;(void)b;return 0;}
static uint8_t USBD_CDC_ReceivePacket(USBD_HandleTypeDef*a) {(void)a;return 0;}
static uint8_t USBD_CDC_TransmitPacket(USBD_HandleTypeDef*a) {(void)a;return 0;}
static uint8_t USBD_Init(USBD_HandleTypeDef*a,int*b,uint8_t c) {(void)a;(void)b;(void)c;return 0;}
static uint8_t USBD_RegisterClass(USBD_HandleTypeDef*a,int*b) {(void)a;(void)b;return 0;}
static uint8_t USBD_CDC_RegisterInterface(USBD_HandleTypeDef*a,USBD_CDC_ItfTypeDef*b) {(void)a;(void)b;return 0;}
static uint8_t USBD_DeInit(USBD_HandleTypeDef*a) {(void)a;return 0;}
static uint8_t USBD_Start(USBD_HandleTypeDef*a) {(void)a;return 0;}
static unsigned safety_calls, fault_calls;
void app_usb_lost_isr(bool had_session) { ++safety_calls; if (had_session) ++fault_calls; }
