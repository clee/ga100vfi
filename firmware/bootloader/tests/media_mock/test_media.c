#define _GNU_SOURCE
#include <sys/mman.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"
#include "usbd_dfu.h"
extern USBD_DFU_MediaTypeDef boot_media;
static unsigned operations,fail_at;static bool unlocked;
static unsigned char *flash;
static unsigned char package[FLASH_END-APP_BASE];
#define FLASH_BYTES (FLASH_END-0x08000000u)
static unsigned char used[FLASH_BYTES/8];
static bool fail(void){return ++operations==fail_at;}
int HAL_FLASH_Unlock(void){unlocked=true;return HAL_OK;}
int HAL_FLASH_Lock(void){unlocked=false;return HAL_OK;}
int HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *e,uint32_t *error){
 assert(unlocked && e->Banks==FLASH_BANK_1 && e->NbPages==1);
 uint32_t a=0x08000000u+e->Page*PAGE_SIZE;
 assert(a>=APP_BASE && a<=FLASH_END-PAGE_SIZE);*error=0;
 if(fail())return HAL_ERROR;
 memset((void *)(uintptr_t)a,255,PAGE_SIZE);memset(used+(a-0x08000000u)/8,0,PAGE_SIZE/8);return HAL_OK;
}
int HAL_FLASH_Program(uint32_t type,uint32_t a,uint64_t d){
 (void)type;assert(unlocked && a>=APP_BASE && a<=FLASH_END-8 && !(a&7u));
 if(fail())return HAL_ERROR;
 unsigned idx=(a-0x08000000u)/8;if(used[idx])return HAL_ERROR;
 uint64_t old;memcpy(&old,(void *)(uintptr_t)a,8);if(old!=UINT64_MAX)return HAL_ERROR;
 memcpy((void *)(uintptr_t)a,&d,8);used[idx]=1;return HAL_OK;
}
/* optional failure-report hook supplied by loader media */
void boot_usb_error(uint8_t error){(void)error;}
static void reset_empty(void){
 memset(flash,0x5a,APP_BASE-0x08000000u);memset((void *)APP_BASE,255,FLASH_END-APP_BASE);
 memset(used,0,sizeof used);operations=0;fail_at=0;unlocked=false;assert(boot_media.Init()==USBD_OK);
}
static void prepare(void){
 memset(package,255,sizeof package);uint32_t sp=0x200057f8,pc=APP_BASE+9;
 memcpy(package,&sp,4);memcpy(package+4,&pc,4);memset(package+8,0x33,56);
 image_footer_t footer={IMAGE_MAGIC,64,image_crc(package,64),1,COMMIT_MAGIC,~COMMIT_MAGIC,PWMVID_BOARD_ID,~PWMVID_BOARD_ID};
 memcpy(package+sizeof(package)-sizeof footer,&footer,sizeof footer);
}
static bool update(void){
 for(uint32_t a=APP_BASE;a<FLASH_END;a+=PAGE_SIZE)if(boot_media.Erase(a)!=USBD_OK)return false;
 for(uint32_t offset=0;offset<sizeof package;offset+=256){
  uint32_t n=sizeof package-offset;if(n>256)n=256;
  if(boot_media.Write(package+offset,(uint8_t *)(uintptr_t)(APP_BASE+offset),n)!=USBD_OK)return false;
 }
 return true;
}
static void loader_intact(void){for(unsigned i=0;i<APP_BASE-0x08000000u;i++)assert(flash[i]==0x5a);assert(!unlocked);}
int main(void){
 flash=mmap((void *)0x08000000u,FLASH_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 assert(flash!=(void *)-1);prepare();reset_empty();
 assert(boot_media.Erase(APP_BASE-PAGE_SIZE)!=USBD_OK);
 assert(boot_media.Erase(APP_BASE+1)!=USBD_OK);
 assert(boot_media.Erase(FLASH_END)!=USBD_OK);
 assert(boot_media.Erase(UINT32_MAX-2047u)!=USBD_OK);
 assert(boot_media.Write(package,(uint8_t *)APP_BASE,8)!=USBD_OK);assert(operations==0);
 assert(update());assert(image_valid());loader_intact();unsigned count=operations;
 for(unsigned cut=1;cut<=count;cut++){
  reset_empty();fail_at=cut;assert(!update());assert(!image_valid());loader_intact();
  /* Simulated reboot and retry must recover without touching loader. */
  fail_at=0;assert(boot_media.Init()==USBD_OK);assert(update());assert(image_valid());loader_intact();
 }
 /* An interrupted first erase may preserve the previous valid image; once
  * invalidation succeeds no later injected failure may launch the partial one. */
 for(unsigned cut=1;cut<=count;cut++) {
  reset_empty();assert(update());assert(image_valid());assert(boot_media.Init()==USBD_OK);
  operations=0;fail_at=cut;assert(!update());
  if(cut==1)assert(image_valid());else assert(!image_valid());loader_intact();
 }
 reset_empty();package[sizeof package-24]^=1;assert(!update());assert(!image_valid());loader_intact();prepare();
 reset_empty();package[sizeof package-8]^=1;assert(!update());assert(!image_valid());loader_intact();prepare();
 reset_empty();assert(update());assert(boot_media.Erase(APP_BASE)!=USBD_OK);
 assert(boot_media.Write(package,(uint8_t *)APP_BASE,8)!=USBD_OK);assert(image_valid());loader_intact();
 reset_empty();assert(boot_media.Erase(APP_BASE)==USBD_OK);
 assert(boot_media.Write(package,(uint8_t *)(APP_BASE-8),8)!=USBD_OK);
 assert(boot_media.Write(package,(uint8_t *)(FLASH_END-8),16)!=USBD_OK);
 assert(boot_media.Write(package,(uint8_t *)APP_BASE,7)!=USBD_OK);
 assert(boot_media.Write(package,(uint8_t *)(APP_BASE+1),8)!=USBD_OK);
 assert(boot_media.Write(package,(uint8_t *)APP_BASE,UINT32_MAX)!=USBD_OK);
 loader_intact();
 printf("PASS: actual media bounds, successful commit, %u injected flash failures, reboot/retry, loader preservation\n",count);
}
