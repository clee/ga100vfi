/* USB-only application updater. Writes are restricted to the fixed application
 * partition; every image needs a verified CRC, board tag and final commit. */
#include "image.h"
#include "usbd_dfu.h"
#include <string.h>
static bool begun,committed;
static uint8_t erased_pages[(PWMVID_FLASH_END-PWMVID_APP_BASE+PAGE_SIZE-1u)/PAGE_SIZE/8u+1u];
static void mark_erased(uint32_t page){erased_pages[page>>3]|=(uint8_t)(1u<<(page&7u));}
static bool was_erased(uint32_t page){return (erased_pages[page>>3]&(1u<<(page&7u)))!=0u;}
extern void boot_usb_error(uint8_t status);
static uint16_t fail(uint8_t status){begun=false;boot_usb_error(status);return USBD_FAIL;}
static uint16_t media_init(void){begun=false;committed=false;memset(erased_pages,0,sizeof(erased_pages));return USBD_OK;}
static uint16_t media_deinit(void){return USBD_OK;}
static bool span(uint32_t a,uint32_t n){return a>=APP_BASE && a<FLASH_END && n<=FLASH_END-a;}
static bool erase_page(uint32_t a){
 FLASH_EraseInitTypeDef e={0};uint32_t error=0;
 e.TypeErase=FLASH_TYPEERASE_PAGES;e.Banks=FLASH_BANK_1;
 e.Page=(a-0x08000000u)/PAGE_SIZE;e.NbPages=1;
 if(HAL_FLASH_Unlock()!=HAL_OK)return false;
 bool ok=HAL_FLASHEx_Erase(&e,&error)==HAL_OK;
 HAL_FLASH_Lock();return ok;
}
static uint16_t media_erase(uint32_t a){
 if(committed || !span(a,PAGE_SIZE) || (a&(PAGE_SIZE-1u)))return fail(DFU_ERROR_ADDRESS);
 /* Invalidate the old commit before erasing any payload. Interrupted updates
  * stay in this USB-only loader; no half-written application is started. */
 if(!begun){
  memset(erased_pages,0,sizeof(erased_pages));
  if(!erase_page(FLASH_END-PAGE_SIZE))return fail(DFU_ERROR_ERASE);
  mark_erased((FLASH_END-PAGE_SIZE-APP_BASE)/PAGE_SIZE);begun=true;
 }
 if(!erase_page(a))return fail(DFU_ERROR_ERASE);
 mark_erased((a-APP_BASE)/PAGE_SIZE);return USBD_OK;
}
static bool program(uint32_t a,const uint8_t *p,uint32_t n){
 if((a&7u)||(n&7u)||!span(a,n)||HAL_FLASH_Unlock()!=HAL_OK)return false;
 bool ok=true;
 for(uint32_t i=0;i<n;i+=8){
  uint64_t d;memcpy(&d,p+i,8);
  if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,a+i,d)!=HAL_OK ||
     memcmp((void *)(a+i),p+i,8)!=0){ok=false;break;}
 }
 HAL_FLASH_Lock();return ok;
}
static uint16_t media_write(uint8_t *src,uint8_t *dest,uint32_t n){
 uint32_t a=(uint32_t)dest;
 if(!begun || committed || !n || n>USBD_DFU_XFER_SIZE ||
    !span(a,n) || (a&7u) || (n&7u))return fail(DFU_ERROR_ADDRESS);
 for(uint32_t page=(a-APP_BASE)/PAGE_SIZE;page<=(a+n-1u-APP_BASE)/PAGE_SIZE;page++)
  if(!was_erased(page))return fail(DFU_ERROR_CHECK_ERASED);
 if(a+n<=FOOTER_BASE)return program(a,src,n)?USBD_OK:fail(DFU_ERROR_WRITE);
 /* Final download block must include all 32 footer bytes at the fixed end. */
 if(a>FOOTER_BASE || a+n!=FLASH_END)return fail(DFU_ERROR_ADDRESS);
 uint32_t payload=FOOTER_BASE-a;
 if(payload && !program(a,src,payload))return fail(DFU_ERROR_WRITE);
 image_footer_t f;memcpy(&f,src+payload,sizeof(f));
 if(!image_valid_at((const uint8_t *)APP_BASE,&f))return fail(DFU_ERROR_VERIFY);
 /* Commit doubleword is last, after all image bytes, metadata, CRC and vectors.
  * Reserved footer bytes remain erased. No option-byte writes exist here. */
 if(!program(FOOTER_BASE,(const uint8_t *)&f,16u))return fail(DFU_ERROR_WRITE);
 if(!program(FOOTER_BASE+24u,(const uint8_t *)&f+24u,8u))return fail(DFU_ERROR_WRITE);
 if(!program(FOOTER_BASE+16u,(const uint8_t *)&f+16u,8u))return fail(DFU_ERROR_WRITE);
 if(!image_valid())return fail(DFU_ERROR_VERIFY);
 committed=true;return USBD_OK;
}
static uint8_t *media_read(uint8_t *src,uint8_t *dest,uint32_t n){
 if(n>USBD_DFU_XFER_SIZE)return NULL;
 if(span((uint32_t)src,n))memcpy(dest,src,n);else {memset(dest,0xff,n);boot_usb_error(DFU_ERROR_ADDRESS);}
 return dest;
}
static uint16_t media_status(uint32_t a,uint8_t cmd,uint8_t *b){
 (void)a;uint32_t ms=cmd==DFU_MEDIA_ERASE?100u:10u;
 b[1]=(uint8_t)ms;b[2]=(uint8_t)(ms>>8);b[3]=(uint8_t)(ms>>16);return USBD_OK;
}
#define APP_DFU_MAP "@PWMVID-P8P9-APP /0x08002000/60*002Kg"
USBD_DFU_MediaTypeDef boot_media={
 (const uint8_t *)APP_DFU_MAP,
 media_init,media_deinit,media_erase,media_write,media_read,media_status
};
