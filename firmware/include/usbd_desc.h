#ifndef PWMVID_USBD_DESC_H
#define PWMVID_USBD_DESC_H

#include "usbd_def.h"

/* ST's demonstration ID is a bench-only placeholder, not a licensed product ID. */
#ifndef PWMVID_USB_VID
#define PWMVID_USB_VID 0x0483U
#endif
#ifndef PWMVID_USB_PID
#define PWMVID_USB_PID 0x5740U
#endif

extern USBD_DescriptorsTypeDef pwmvid_usb_descriptors;

#endif
