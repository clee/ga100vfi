#ifndef PWMVID_USBD_CONF_H
#define PWMVID_USBD_CONF_H

#include <stdint.h>
#include <string.h>
#include "stm32g4xx_hal.h"

#define USBD_MAX_NUM_INTERFACES          2U
#define USBD_MAX_NUM_CONFIGURATION       1U
#define USBD_MAX_SUPPORTED_CLASS         1U
#define USBD_MAX_STR_DESC_SIZ            128U
#define USBD_DEBUG_LEVEL                 0U
#define USBD_LPM_ENABLED                 0U
#define USBD_SELF_POWERED                0U
/* USB descriptor units are 2 mA: 50 means 100 mA, pending board measurements. */
#define USBD_MAX_POWER                   50U
#define USBD_SUPPORT_USER_STRING_DESC    0U
#define USBD_CLASS_USER_STRING_DESC      0U
#define DEVICE_FS                        0U

void *USBD_static_malloc(uint32_t size);
void USBD_static_free(void *pointer);
#define USBD_malloc  USBD_static_malloc
#define USBD_free    USBD_static_free
#define USBD_memset  memset
#define USBD_memcpy  memcpy
#define USBD_Delay   HAL_Delay
#define USBD_UsrLog(...) do { } while (0)
#define USBD_ErrLog(...) do { } while (0)
#define USBD_DbgLog(...) do { } while (0)

#endif
