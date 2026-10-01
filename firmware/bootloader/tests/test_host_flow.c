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
static bool __attribute__((unused)) update(void){
 for(uint32_t a=APP_BASE;a<FLASH_END;a+=PAGE_SIZE)if(boot_media.Erase(a)!=USBD_OK)return false;
 for(uint32_t offset=0;offset<sizeof package;offset+=256){
  uint32_t n=sizeof package-offset;if(n>256)n=256;
  if(boot_media.Write(package+offset,(uint8_t *)(uintptr_t)(APP_BASE+offset),n)!=USBD_OK)return false;
 }
 return true;
}
static void loader_intact(void){for(unsigned i=0;i<APP_BASE-0x08000000u;i++)assert(flash[i]==0x5a);assert(!unlocked);}

#include "ep0_dfu.h"
static unsigned sent_n,port_stalls,port_resets;
static uint8_t packet[64];
void ep0_port_send(const uint8_t *p,uint16_t n){assert(n<=64);sent_n=n;if(n)memcpy(packet,p,n);}
void ep0_port_receive(void){}
void ep0_port_stall(void){port_stalls++;}
void ep0_port_address(uint8_t a){assert(a==13);}
void ep0_port_leave(void){port_resets++;}
uint32_t ep0_port_uid(unsigned w){return 0x12345678u+w;}
static void setup(uint8_t bm,uint8_t req,uint16_t val,uint16_t idx,uint16_t n){
 uint8_t s[]={bm,req,val,val>>8,idx,idx>>8,n,n>>8};ep0_setup(s);
}
static void status_in(void){assert(sent_n==0);ep0_in_complete();ep0_poll();}
static void read_done(void){ep0_in_complete();ep0_out(NULL,0);ep0_poll();}
static void enumerate(void){
 ep0_reset();setup(0x80,6,0x100,0,8);assert(sent_n==8 && packet[0]==18);read_done();
 setup(0,5,13,0,0);status_in();
 setup(0x80,6,0x200,0,9);assert(sent_n==9 && packet[2]==27);read_done();
 setup(0x80,6,0x200,0,27);assert(sent_n==27 && packet[17]==4);read_done();
 setup(0x80,6,0x304,0x409,2);unsigned descriptor_n=packet[0];assert(sent_n==2);read_done();
 setup(0x80,6,0x304,0x409,descriptor_n);uint8_t desc[128];unsigned got=0;
 do {assert(sent_n && got+sent_n<=sizeof(desc));memcpy(desc+got,packet,sent_n);got+=sent_n;ep0_in_complete();} while(got<descriptor_n);
 ep0_out(NULL,0);ep0_poll();
 const uint8_t *text=boot_media.pStrDesc;
 assert(got==2+2*strlen((const char *)text));
 for(unsigned i=0;text[i];i++)assert(desc[2+2*i]==text[i] && !desc[3+2*i]);
 setup(0,9,1,0,0);status_in();
}
static void get_status(uint8_t *error,uint8_t *state){
 setup(0xa1,3,0,0,6);assert(sent_n==6);*error=packet[0];*state=packet[4];read_done();
}
static bool download(uint16_t block,const uint8_t *p,uint16_t n){
 setup(0x21,1,block,0,n);
 for(unsigned i=0;i<n;i+=64)ep0_out(p+i,n-i<64?n-i:64);
 status_in();uint8_t error,state;get_status(&error,&state);assert(!error && state==4);
 get_status(&error,&state);return !error && state==5;
}
static bool command(uint8_t command,uint32_t a){
 uint8_t p[]={command,a,a>>8,a>>16,a>>24};return download(0,p,5);
}
static bool host_update(void){
 /* Matches dfu-util 0.11 dfuse_do_bin_dnload: all page erases first,
  * SET_ADDRESS before each 256-byte transaction 2, final fixed footer. */
 for(uint32_t a=APP_BASE;a<FLASH_END;a+=PAGE_SIZE)if(!command(0x41,a))return false;
 for(unsigned i=0;i<sizeof(package);i+=256){
  if(!command(0x21,APP_BASE+i) || !download(2,package+i,256))return false;
 }
 return true;
}
static void leave(bool valid){
 uint8_t error,state;
 if(valid){setup(0x21,6,0,0,0);status_in();assert(command(0x21,APP_BASE));}
 else {setup(0x21,4,0,0,0);status_in();}
 setup(0x21,1,2,0,0);status_in();unsigned before=port_resets;
 setup(0xa1,3,0,0,6);assert(sent_n==6);error=packet[0];state=packet[4];
 assert(port_resets==before);
 if(valid)assert(!error && state==7);else assert(error==7 && state==10);
 read_done();assert(port_resets==before+(valid?1u:0u));
}
int main(void){
 flash=mmap((void *)0x08000000u,FLASH_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 assert(flash!=(void *)-1);prepare();reset_empty();enumerate();assert(host_update());assert(image_valid());loader_intact();
 unsigned total=operations;leave(true);assert(port_resets==1);
 /* Replay representative failures through the real USB protocol and media. */
 unsigned cuts[]={1,total/2,total};
 for(unsigned c=0;c<3;c++){
  reset_empty();enumerate();fail_at=cuts[c];assert(!host_update());assert(!image_valid());loader_intact();leave(false);
  fail_at=0;enumerate();assert(host_update());assert(image_valid());loader_intact();leave(true);
 }
 printf("PASS: complete dfu-util style enumeration, %zu-byte download, committed CRC, leave and interrupted-update recovery (%u flash operations)\n",sizeof(package),total);
 assert(!port_stalls);munmap(flash,FLASH_BYTES);
}
