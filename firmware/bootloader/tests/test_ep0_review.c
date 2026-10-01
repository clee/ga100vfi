/* Independent protocol sequencing tests. Registers/physical bus not modeled. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ep0_dfu.h"
#include "image.h"
#include "usbd_dfu.h"
static unsigned stalls,sends,receives,erases,writes,leaves,addresses;
static uint16_t sent_size;static uint8_t sent[64];
void ep0_port_send(const uint8_t *p,uint16_t n){assert(n<=64);sent_size=n;if(n)memcpy(sent,p,n);sends++;}
void ep0_port_receive(void){receives++;}
void ep0_port_stall(void){stalls++;}
void ep0_port_address(uint8_t a){assert(a<=127);addresses++;}
void ep0_port_leave(void){leaves++;}
uint32_t ep0_port_uid(unsigned w){return w;}
static bool image_is_valid;
bool image_valid(void){return image_is_valid;}
static uint16_t init(void){return USBD_OK;}
static uint16_t erase(uint32_t a){assert(a>=APP_BASE&&a<=FLASH_END-PAGE_SIZE&&!(a&(PAGE_SIZE-1)));erases++;return USBD_OK;}
static uint16_t write_data(uint8_t *s,uint8_t *d,uint32_t n){(void)s;assert((uintptr_t)d>=APP_BASE&&(uintptr_t)d<FLASH_END&&n<=256&&n<=FLASH_END-(uintptr_t)d&&!(((uintptr_t)d|n)&7u));writes++;return USBD_OK;}
static uint8_t *read_data(uint8_t *s,uint8_t *d,uint32_t n){assert((uintptr_t)s>=APP_BASE&&(uintptr_t)s<FLASH_END&&n<=256&&n<=FLASH_END-(uintptr_t)s);memset(d,0x55,n);return d;}
static uint16_t status(uint32_t a,uint8_t cmd,uint8_t*b){(void)a;(void)cmd;(void)b;return USBD_OK;}
USBD_DFU_MediaTypeDef boot_media={(const uint8_t *)"mock",init,init,erase,write_data,read_data,status};
#include "../src/ep0_dfu.c"
static void setup(uint8_t bm,uint8_t req,uint16_t value,uint16_t idx,uint16_t len){
 uint8_t s[8]={bm,req,value,value>>8,idx,idx>>8,len,len>>8};ep0_setup(s);
}
static void fresh(void){image_is_valid=false;stalls=sends=receives=erases=writes=leaves=addresses=0;ep0_reset();setup(0,9,1,0,0);ep0_in_complete();}
static void download_ready(void){uint8_t p[8]={1,2,3,4,5,6,7,8};setup(0x21,1,2,0,8);ep0_out(p,8);ep0_in_complete();assert(dfu_state==DNLOAD_SYNC);}
static uint32_t fuzz_state;
static uint32_t random_word(void){
 uint32_t x=fuzz_state;x^=x<<13;x^=x>>17;x^=x<<5;return fuzz_state=x;
}
static void fuzz_protocol(void){
 static const uint8_t bm_values[]={0,1,0x21,0x80,0x81,0x82,0xa1};
 static const uint8_t requests[]={0,1,2,3,4,5,6,8,9,10,11};
 static const uint16_t lengths[]={0,1,2,3,5,6,7,8,18,27,63,64,65,127,128,255,256,257,65535};
 uint8_t packet[64];
 for(unsigned seed=1;seed<=4;seed++){
  fresh();fuzz_state=0x5a13c9e1u*seed;
  for(unsigned step=0;step<100000;step++){
   uint32_t r=random_word();
   switch(r%10u){
    case 0:case 1:case 2:{
     uint8_t bm=(r&0x100u)?bm_values[random_word()%(sizeof bm_values)]:random_word();
     uint8_t req=(r&0x200u)?requests[random_word()%(sizeof requests)]:random_word();
     uint16_t val=(r&0x400u)?random_word():random_word()%5u;
     if(req==6 && (bm==0x80 || bm==0x81))val=(uint16_t)((1u+random_word()%4u)<<8)|(random_word()%5u);
     uint16_t idx=(r&0x800u)?random_word():((r&0x1000u)?0:0x409);
     uint16_t n=(r&0x2000u)?random_word():lengths[random_word()%(sizeof lengths/sizeof *lengths)];
     setup(bm,req,val,idx,n);break;
    }
    case 3:case 4:{
     for(unsigned j=0;j<64;j++)packet[j]=random_word();
     uint16_t n=(r&0x100u)?random_word():lengths[random_word()%(sizeof lengths/sizeof *lengths)];
     ep0_out(n?packet:NULL,n);break;
    }
    case 5:case 6:ep0_in_complete();break;
    case 7:ep0_poll();break;
    case 8:ep0_reset();break;
    case 9:
     /* Re-seed useful DFU states so fuzzing is not limited to stalled requests. */
     setup(0,9,1,0,0);ep0_in_complete();
     if(r&0x100u){download_ready();setup(0xa1,3,0,0,6);}
     break;
   }
   assert(configuration<=1 && phase<=STATUS_OUT && received<=256);
   assert(dfu_state==IDLE||dfu_state==DNLOAD_SYNC||dfu_state==BUSY||dfu_state==DNLOAD_IDLE||
          dfu_state==MANIFEST_SYNC||dfu_state==MANIFEST||dfu_state==UPLOAD_IDLE||dfu_state==DFU_ERROR_STATE);
  }
 }
 puts("PASS: 400000 deterministic malformed EP0/order/reset fuzz events with media bounds assertions");
}
int main(void){
 fresh();download_ready();setup(0xa1,3,0,0,6);assert(sent[4]==BUSY&&writes==0);
 ep0_poll();assert(writes==0);
 ep0_in_complete();ep0_poll();assert(writes==0);
 ep0_out(NULL,0);ep0_poll();assert(writes==1&&dfu_state==DNLOAD_IDLE);
 fresh();download_ready();setup(0xa1,3,0,0,6);setup(0xa1,5,0,0,1);ep0_in_complete();ep0_poll();assert(writes==0&&dfu_state!=BUSY);
 fresh();download_ready();setup(0xa1,3,0,0,6);ep0_in_complete();setup(0xa1,5,0,0,1);ep0_in_complete();ep0_poll();assert(dfu_state!=BUSY&&writes<=1);
 fresh();download_ready();setup(0xa1,3,0,0,6);ep0_in_complete();setup(0,9,0,0,0);ep0_in_complete();ep0_poll();assert(configuration==0&&dfu_state==IDLE&&writes==0);
 fresh();setup(0x80,6,0x100,0,0);assert(phase==STATUS_IN);
 fresh();setup(0x21,1,2,0,257);assert(stalls&&dfu_state==DFU_ERROR_STATE&&writes==0);
 fresh();uint8_t p[64]={0};setup(0x21,1,2,0,128);ep0_out(p,10);assert(stalls&&dfu_state==DFU_ERROR_STATE&&writes==0);
 fresh();reply(p,64,65);assert(sent_size==64);ep0_in_complete();assert(sent_size==0);ep0_in_complete();assert(phase==STATUS_OUT);
 fresh();setup(0,9,0,0,0);ep0_in_complete();setup(0,5,42,0,0);assert(!addresses);ep0_in_complete();assert(addresses==1);
 fresh();download_ready();setup(0xa1,3,0,0,6);ep0_in_complete();ep0_reset();ep0_poll();assert(writes==0&&configuration==0&&dfu_state==IDLE);
 fresh();setup(0x21,1,2,0,0);ep0_in_complete();setup(0xa1,3,0,0,6);assert(sent[0]==DFU_ERROR_VERIFY && sent[4]==DFU_ERROR_STATE);ep0_in_complete();ep0_poll();assert(leaves==0);
 fresh();image_is_valid=true;setup(0x21,1,2,0,0);ep0_in_complete();setup(0xa1,3,0,0,6);assert(sent[4]==MANIFEST && leaves==0);
 ep0_in_complete();ep0_poll();assert(leaves==0);
 ep0_out(NULL,0);ep0_poll();assert(leaves==1);
 fuzz_protocol();
 puts("PASS: independent EP0 action timing/aborts/configuration/reset/bounds/ZLP/address protocol checks");
}
