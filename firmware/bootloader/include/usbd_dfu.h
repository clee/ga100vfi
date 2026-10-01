/* Small media ABI retained from the reviewed Cube-backed implementation.
 * This is not the generic ST USB device core or DFU class. */
#ifndef PWMVID_MEDIA_ABI_H
#define PWMVID_MEDIA_ABI_H
#include "stm32g4xx_hal.h"
#define USBD_OK 0u
#define USBD_FAIL 1u
#define USBD_DFU_XFER_SIZE 256u
#define DFU_MEDIA_ERASE 0u
#define DFU_ERROR_WRITE 3u
#define DFU_ERROR_ERASE 4u
#define DFU_ERROR_CHECK_ERASED 5u
#define DFU_ERROR_VERIFY 7u
#define DFU_ERROR_ADDRESS 8u
#define DFU_ERROR_STALLEDPKT 15u
typedef struct {
 const uint8_t *pStrDesc;
 uint16_t (*Init)(void); uint16_t (*DeInit)(void); uint16_t (*Erase)(uint32_t);
 uint16_t (*Write)(uint8_t *,uint8_t *,uint32_t);uint8_t *(*Read)(uint8_t *,uint8_t *,uint32_t);
 uint16_t (*GetStatus)(uint32_t,uint8_t,uint8_t *);
} USBD_DFU_MediaTypeDef;
extern USBD_DFU_MediaTypeDef boot_media;
#endif
