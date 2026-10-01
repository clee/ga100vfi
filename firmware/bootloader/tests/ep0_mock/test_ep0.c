#include "ep0_dfu.h"
#include "image.h"
#include "usbd_dfu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned sends,receives,stalls,addresses,leaves,erases,writes,reads,inits;
static uint8_t sent[64],last_address;
static uint16_t sent_n;
static uint32_t last_target;
static bool fail_media;
bool image_valid(void){return true;}
void ep0_port_send(const uint8_t *p,uint16_t n){assert(n<=64);sent_n=n;memcpy(sent,p,n);sends++;}
void ep0_port_receive(void){receives++;}
void ep0_port_stall(void){stalls++;}
void ep0_port_address(uint8_t a){addresses++;last_address=a;}
void ep0_port_leave(void){leaves++;}
uint32_t ep0_port_uid(unsigned w){assert(w<3);return 0x12345678u+w;}
static uint16_t init(void){inits++;return 0;}
static uint16_t deinit(void){return 0;}
static uint16_t erase(uint32_t a){last_target=a;erases++;if(fail_media)boot_usb_error(4);return fail_media;}
static uint16_t write(uint8_t *p,uint8_t *a,uint32_t n){
 assert(n && n<=256 && !(n&7));for(unsigned i=0;i<n;i++)assert(p[i]==(uint8_t)i);
 last_target=(uint32_t)(uintptr_t)a;writes++;if(fail_media)boot_usb_error(3);return fail_media;
}
static uint8_t *read_media(uint8_t *a,uint8_t *p,uint32_t n){reads++;last_target=(uint32_t)(uintptr_t)a;memset(p,0xaa,n);return p;}
static uint16_t status(uint32_t a,uint8_t c,uint8_t *p){(void)a;(void)c;(void)p;return 0;}
USBD_DFU_MediaTypeDef boot_media={(const uint8_t *)"@PWMVID-P8P9-APP /0x08002000/12*002Kg",init,deinit,erase,write,read_media,status};
static void setup(uint8_t bm,uint8_t req,uint16_t value,uint16_t index,uint16_t n){
 uint8_t s[]={bm,req,value,value>>8,index,index>>8,n,n>>8};ep0_setup(s);
}
static void finish_status_in(void){assert(sent_n==0);ep0_in_complete();ep0_poll();}
static void finish_read(void){
 /* All callers use <=64-byte responses here. */
 ep0_in_complete();ep0_out(NULL,0);ep0_poll();
}
static void reset(void){ep0_reset();fail_media=false;setup(0,9,1,0,0);finish_status_in();}
static uint8_t get_state(void){setup(0xa1,5,0,0,1);assert(sent_n==1);uint8_t s=sent[0];finish_read();return s;}
static void get_status(uint8_t state,uint8_t error,bool execute){
 setup(0xa1,3,0,0,6);assert(sent_n==6 && sent[0]==error && sent[4]==state);
 unsigned old_erases=erases,old_writes=writes,old_leaves=leaves;
 ep0_in_complete();ep0_poll();
 assert(erases==old_erases && writes==old_writes && leaves==old_leaves);
 ep0_out(NULL,0);if(execute)ep0_poll();
}
static void command(uint8_t c,uint32_t a){
 uint8_t p[]={c,a,a>>8,a>>16,a>>24};setup(0x21,1,0,0,5);ep0_out(p,5);finish_status_in();
 assert(get_state()==3);get_status(4,0,true);assert(get_state()==5);
}
static void data_download(uint16_t n){
 uint8_t p[256];for(unsigned i=0;i<256;i++)p[i]=(uint8_t)i;
 setup(0x21,1,2,0,n);
 for(unsigned i=0;i<n;i+=64)ep0_out(p+i,n-i<64?n-i:64);
 finish_status_in();get_status(4,0,true);assert(get_state()==5);
}
int main(void){
 reset();
 /* Descriptor short reads, full multi-packet string and the exact UID. */
 setup(0x80,6,0x100,0,8);assert(sent_n==8 && sent[0]==18);finish_read();
 setup(0x80,6,0x200,0,255);assert(sent_n==27 && sent[18]==9 && sent[20]==11);finish_read();
 setup(0x80,6,0x304,0x409,2);assert(sent_n==2 && sent[0]==76);finish_read();
 setup(0x80,6,0x304,0x409,255);assert(sent_n==64);ep0_in_complete();assert(sent_n==12);finish_read();
 setup(0x80,6,0x303,0x409,255);assert(sent_n==50);for(unsigned i=0;i<8;i++)assert(sent[2+i*2]=="12345678"[i]);finish_read();
 /* Exactly 64 descriptor bytes needs a terminating ZLP when host asks for more. */
 const uint8_t *old=boot_media.pStrDesc;boot_media.pStrDesc=(const uint8_t *)"1234567890123456789012345678901";
 setup(0x80,6,0x304,0x409,255);assert(sent_n==64);ep0_in_complete();assert(sent_n==0);finish_read();boot_media.pStrDesc=old;
 /* Address becomes effective only after its status packet completes. */
 ep0_reset();unsigned before=addresses;setup(0,5,17,0,0);assert(addresses==before);finish_status_in();assert(addresses==before+1 && last_address==17);
 setup(0,5,25,0,0);setup(0x80,6,0x100,0,18);finish_read();assert(last_address==17);
 reset();command(0x41,APP_BASE);assert(erases==1 && last_target==APP_BASE);
 command(0x21,APP_BASE);data_download(256);assert(writes==1 && last_target==APP_BASE);
 command(0x21,APP_BASE+256);data_download(8);assert(writes==2 && last_target==APP_BASE+256);
 /* Abort, post-commit address selection, and manifestation used by dfu-util. */
 setup(0x21,6,0,0,0);finish_status_in();assert(get_state()==2);
 command(0x21,APP_BASE);setup(0x21,1,2,0,0);finish_status_in();get_status(7,0,true);assert(leaves==1);
 reset();command(0x21,APP_BASE);fail_media=true;
 uint8_t p[8]={0,1,2,3,4,5,6,7};setup(0x21,1,2,0,8);ep0_out(p,8);finish_status_in();get_status(4,0,true);get_status(10,3,true);
 setup(0x21,4,0,0,0);finish_status_in();assert(get_state()==2);
 /* Aborted GETSTATUS cannot orphan BUSY or execute stale payload later. */
  reset();uint8_t cmd[]={0x41,(uint8_t)APP_BASE,(uint8_t)(APP_BASE>>8),(uint8_t)(APP_BASE>>16),(uint8_t)(APP_BASE>>24)};
 setup(0x21,1,0,0,5);ep0_out(cmd,5);finish_status_in();before=erases;
 setup(0xa1,3,0,0,6);setup(0xa1,3,0,0,6);assert(sent[4]==10);finish_read();assert(erases==before);
 reset();setup(0x21,1,0,0,5);ep0_out(cmd,5);finish_status_in();get_status(4,0,false);
 setup(0x80,6,0x100,0,18);finish_read();assert(get_state()==10 && erases==before);
 /* Deconfiguration cancels state and rejects class requests until configured. */
 setup(0,9,0,0,0);finish_status_in();before=stalls;setup(0xa1,5,0,0,1);assert(stalls==before+1);
 reset();before=stalls;setup(0x21,1,2,0,257);assert(stalls==before+1);
 reset();setup(0x21,1,2,0,256);ep0_out(p,8);assert(get_state()==10);
 reset();setup(0x21,1,1,0,8);ep0_out(p,8);assert(get_state()==10);
 reset();uint8_t mass=0x41;setup(0x21,1,0,0,1);ep0_out(&mass,1);assert(get_state()==10);
 reset();command(0x21,FLASH_END-8);setup(0x21,1,2,0,16);ep0_out(p,8);assert(get_state()==10);
 reset();setup(0xa1,2,0,0,255);assert(sent_n==3 && sent[1]==0x21);finish_read();assert(get_state()==2);
 setup(0xa1,2,2,0,256);assert(sent_n==64);for(unsigned i=0;i<4;i++)ep0_in_complete();ep0_out(NULL,0);ep0_poll();assert(reads==1 && get_state()==9);
 /* Unknown and malformed standard requests stall, without writing media. */
 reset();before=writes+erases;for(unsigned bm=0;bm<256;bm++){
  setup((uint8_t)bm,0xff,0xffff,0xffff,0xffff);ep0_poll();
 }assert(writes+erases==before);
 printf("EP0 protocol tests passed: %u IN packets, %u receive arms, %u expected stalls\n",sends,receives,stalls);
}
