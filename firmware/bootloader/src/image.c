#include "image.h"
#include <string.h>
uint32_t image_crc(const uint8_t *p,uint32_t n) {
 uint32_t crc=0xffffffffu;
 while(n--){crc^=*p++;for(unsigned b=0;b<8;b++)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}
 return ~crc;
}
bool image_valid_at(const uint8_t *image,const image_footer_t *f) {
 if(f->magic!=IMAGE_MAGIC || f->format!=PWMVID_IMAGE_FORMAT || f->commit!=COMMIT_MAGIC || f->commit_inverse!=~COMMIT_MAGIC || f->board!=PWMVID_BOARD_ID || f->board_inverse!=~PWMVID_BOARD_ID ||
    f->length<8u || f->length>FOOTER_BASE-APP_BASE || (f->length&7u))return false;
 uint32_t sp,pc;memcpy(&sp,image,4);memcpy(&pc,image+4,4);
 if((sp&7u) || sp<=0x20000000u || sp>PWMVID_BOOT_COOKIE_ADDRESS || !(pc&1u) ||
    (pc&~1u)<APP_BASE || (pc&~1u)>=APP_BASE+f->length)return false;
 return image_crc(image,f->length)==f->crc;
}
bool image_valid(void){return image_valid_at((const uint8_t *)APP_BASE,(const image_footer_t *)FOOTER_BASE);}
