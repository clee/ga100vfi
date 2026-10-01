#include "image.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t image[64];
static image_footer_t footer;
static void valid_image(void){
 memset(image,0,sizeof(image));uint32_t sp=PWMVID_BOOT_COOKIE_ADDRESS,pc=APP_BASE+9u;
 memcpy(image,&sp,4);memcpy(image+4,&pc,4);
 footer=(image_footer_t){IMAGE_MAGIC,sizeof(image),image_crc(image,sizeof(image)),1u,COMMIT_MAGIC,~COMMIT_MAGIC,PWMVID_BOARD_ID,~PWMVID_BOARD_ID};
}
int main(void){
 assert(image_crc((const uint8_t *)"123456789",9)==0xcbf43926u);
 valid_image();assert(image_valid_at(image,&footer));
 image[40]^=1;assert(!image_valid_at(image,&footer));
 valid_image();footer.commit=0xffffffffu;assert(!image_valid_at(image,&footer));
 valid_image();footer.commit_inverse=0xffffffffu;assert(!image_valid_at(image,&footer));
 valid_image();footer.length=0xffffffffu;assert(!image_valid_at(image,&footer));
 valid_image();footer.length=7;assert(!image_valid_at(image,&footer));
 valid_image();footer.format=0;assert(!image_valid_at(image,&footer));
 valid_image();uint32_t bad=0x08000001u;memcpy(image+4,&bad,4);footer.crc=image_crc(image,sizeof(image));assert(!image_valid_at(image,&footer));
 valid_image();bad=0x20005808u;memcpy(image,&bad,4);footer.crc=image_crc(image,sizeof(image));assert(!image_valid_at(image,&footer));
 valid_image();bad=APP_BASE+8u;memcpy(image+4,&bad,4);footer.crc=image_crc(image,sizeof(image));assert(!image_valid_at(image,&footer));
 puts("PASS: image CRC, committed footer, length and vector gates");
}
