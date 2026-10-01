#ifndef MOCK_DFU_H
#define MOCK_DFU_H
#include <stdint.h>
#include <stddef.h>
#define USBD_OK 0
#define USBD_FAIL 1
#define USBD_DFU_XFER_SIZE 256u
#define DFU_MEDIA_ERASE 0
#define DFU_ERROR_ADDRESS 8
#define DFU_ERROR_ERASE 4
#define DFU_ERROR_CHECK_ERASED 5
#define DFU_ERROR_WRITE 3
#define DFU_ERROR_VERIFY 7
#define FLASH_TYPEERASE_PAGES 0
#define FLASH_BANK_1 1
#define FLASH_TYPEPROGRAM_DOUBLEWORD 0
#define HAL_OK 0
#define HAL_ERROR 1
typedef struct { uint32_t TypeErase,Banks,Page,NbPages; } FLASH_EraseInitTypeDef;
typedef struct {
 const uint8_t *pStrDesc;
 uint16_t (*Init)(void); uint16_t (*DeInit)(void); uint16_t (*Erase)(uint32_t);
 uint16_t (*Write)(uint8_t *,uint8_t *,uint32_t);uint8_t *(*Read)(uint8_t *,uint8_t *,uint32_t);
 uint16_t (*GetStatus)(uint32_t,uint8_t,uint8_t *);
} USBD_DFU_MediaTypeDef;
int HAL_FLASH_Unlock(void);int HAL_FLASH_Lock(void);int HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *,uint32_t *);
int HAL_FLASH_Program(uint32_t,uint32_t,uint64_t);
#endif
