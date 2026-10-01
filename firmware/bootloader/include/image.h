#ifndef PWMVID_BOOT_IMAGE_H
#define PWMVID_BOOT_IMAGE_H
#include <stdbool.h>
#include "boot_contract.h"
#define BOOT_END PWMVID_APP_BASE
#define APP_BASE PWMVID_APP_BASE
#define FLASH_END PWMVID_FLASH_END
#define FOOTER_BASE PWMVID_FOOTER_BASE
#define IMAGE_MAGIC PWMVID_IMAGE_MAGIC
#define COMMIT_MAGIC PWMVID_COMMIT_MAGIC
#define PAGE_SIZE 2048u
typedef pwmvid_image_footer_t image_footer_t;
uint32_t image_crc(const uint8_t *p,uint32_t n);
bool image_valid_at(const uint8_t *image,const image_footer_t *f);
bool image_valid(void);
#endif
