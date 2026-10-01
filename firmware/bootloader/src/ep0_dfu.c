/* USB 2.0 EP0 and the bounded subset of ST DfuSe used by dfu-util 0.11.
 * Sources: USB 2.0 chapter 9, USB DFU 1.1, ST AN3156, dfu-util dfuse.c.
 * Only application-page erase, bounded download/upload and reset are exposed.
 * No factory-ROM jump, mass erase, unprotect or option-byte command exists. */
#include "ep0_dfu.h"
#include "image.h"
#include "usbd_dfu.h"
#include <string.h>
#ifndef DFU_ERROR_STALLEDPKT
#define DFU_ERROR_STALLEDPKT 15u
#endif
extern USBD_DFU_MediaTypeDef boot_media;
enum { IDLE=2, DNLOAD_SYNC=3, BUSY=4, DNLOAD_IDLE=5,
 MANIFEST_SYNC=6, MANIFEST=7, UPLOAD_IDLE=9, DFU_ERROR_STATE=10 };
enum { WAIT_SETUP, DATA_IN, DATA_OUT, STATUS_IN, STATUS_OUT };
enum { NO_ACTION, EXECUTE, LEAVE };
static uint8_t phase,configuration,address,dfu_state,dfu_status,after_in,pending;
static uint8_t status_reply[6],buffer[256],string_buffer[128];
static uint16_t block,length,received,tx_left;
static const uint8_t *tx_ptr;
static bool tx_zlp;
static uint32_t data_address;
static const uint8_t device_descriptor[]={
 18,1,0,2,0,0,0,64,0x83,0x04,0x11,0xdf,1,0,1,2,3,1
};
static const uint8_t configuration_descriptor[]={
 9,2,27,0,1,1,0,0x80,50,
 9,4,0,0,0,0xfe,1,2,4,
 /* Can download/upload, will detach; manifestation requires reset. */
 9,0x21,0x0b,0xff,0,0,1,0x1a,1
};
static const uint8_t language_descriptor[]={4,3,9,4};
static uint16_t le16(const uint8_t *p){return p[0]|((uint16_t)p[1]<<8);}
static uint32_t le32(const uint8_t *p){return le16(p)|((uint32_t)le16(p+2)<<16);}
static void stall(void){phase=WAIT_SETUP;after_in=NO_ACTION;ep0_port_stall();}
void boot_usb_error(uint8_t status){dfu_status=status;dfu_state=DFU_ERROR_STATE;pending=NO_ACTION;}
static void dfu_stall(void){boot_usb_error(DFU_ERROR_STALLEDPKT);stall();}
static void send_next(void){
 uint16_t n=tx_left>64u?64u:tx_left;
 ep0_port_send(tx_ptr,n);tx_ptr+=n;tx_left-=n;
}
static void reply(const uint8_t *p,uint16_t actual,uint16_t requested){
 if(!requested){phase=STATUS_IN;ep0_port_send(buffer,0);return;}
 uint16_t n=actual<requested?actual:requested;
 tx_zlp=n<requested && (n&63u)==0u;tx_ptr=p;tx_left=n;phase=DATA_IN;
 if(n)send_next();else {tx_zlp=false;ep0_port_send(p,0);}
}
static void status_in(void){phase=STATUS_IN;ep0_port_send(buffer,0);}
static void text_reply(const char *s,uint16_t requested){
 unsigned n=0;
 while(s[n] && n<63u){string_buffer[2u+2u*n]=(uint8_t)s[n];string_buffer[3u+2u*n]=0;n++;}
 string_buffer[0]=(uint8_t)(2u+2u*n);string_buffer[1]=3;
 reply(string_buffer,string_buffer[0],requested);
}
void ep0_reset(void){
 phase=WAIT_SETUP;configuration=address=0;dfu_state=IDLE;dfu_status=0;
 after_in=pending=NO_ACTION;length=received=tx_left=0;tx_zlp=false;
 data_address=APP_BASE;boot_media.Init();
}
static bool target(uint16_t b,uint16_t n,uint32_t *a){
 uint32_t offset=(uint32_t)(b-2u)*256u;
 if(b<2u || data_address<APP_BASE || data_address>=FLASH_END ||
    offset>=FLASH_END-data_address || n>FLASH_END-data_address-offset)return false;
 *a=data_address+offset;return true;
}
static bool valid_download(void){
 if(block==0u){
  if(length==1u)return buffer[0]==0u;
  if(length!=5u || (buffer[0]!=0x21u && buffer[0]!=0x41u))return false;
  uint32_t a=le32(buffer+1);
  return a>=APP_BASE && a<FLASH_END && (buffer[0]!=0x41u || !(a&(PAGE_SIZE-1u)));
 }
 uint32_t a;return target(block,length,&a) && !((a|length)&7u);
}
static bool standard(uint8_t bm,uint8_t request,uint16_t value,uint16_t index,uint16_t n){
 if(request==6u && bm==0x80u){
  uint8_t type=value>>8,which=value;
  if(type==1u && !which && !index)reply(device_descriptor,sizeof(device_descriptor),n);
  else if(type==2u && !which && !index)reply(configuration_descriptor,sizeof(configuration_descriptor),n);
  else if(type==3u && ((!which && !index) || (which && index==0x0409u))){
   if(!which)reply(language_descriptor,sizeof(language_descriptor),n);
   else if(which==1u)text_reply("PWMVID Prototype",n);
   else if(which==2u)text_reply("PWMVID G431CB USB Updater",n);
   else if(which==4u)text_reply((const char *)boot_media.pStrDesc,n);
   else if(which==3u){
    char serial[25];static const char hex[]="0123456789ABCDEF";
    for(unsigned w=0;w<3;w++){
     uint32_t uid=ep0_port_uid(w);
     for(unsigned d=0;d<8;d++)serial[w*8+d]=hex[(uid>>(28u-4u*d))&15u];
    }
    serial[24]=0;text_reply(serial,n);
   }else return false;
  }else return false;
 }else if(request==6u && bm==0x81u && value==0x2100u && !index && configuration){
  reply(configuration_descriptor+18,9,n);
 }else if(request==5u && bm==0u && value<=127u && !index && !n && !configuration){
  address=(uint8_t)value;status_in();
  /* HAL applies a nonzero pending address after the status IN ACK. Address 0
   * is applied immediately by HAL, so defer all addresses to completion. */
  after_in=3u;
 }else if(request==9u && bm==0u && value<=1u && !index && !n){
  ep0_reset();configuration=(uint8_t)value;status_in();
 }else if(request==8u && bm==0x80u && !value && !index && n==1u){
  reply(&configuration,1,n);
 }else if(request==0u && !value && n==2u &&
  ((bm==0x80u && !index) || (bm==0x81u && !index && configuration) ||
   (bm==0x82u && (index==0u || index==0x80u)))){
  status_reply[0]=status_reply[1]=0;reply(status_reply,2,n);
 }else if(request==10u && bm==0x81u && !value && !index && n==1u && configuration){
  status_reply[0]=0;reply(status_reply,1,n);
 }else if(request==11u && bm==1u && !value && !index && !n && configuration){status_in();
 }else return false;
 return true;
}
static bool dfu_request(uint8_t bm,uint8_t request,uint16_t value,uint16_t index,uint16_t n){
 if(index || !configuration)return false;
 if(request==1u && bm==0x21u && n<=256u && (dfu_state==IDLE || dfu_state==DNLOAD_IDLE)){
  block=value;length=n;received=0;
  if(n){phase=DATA_OUT;ep0_port_receive();}
  else {
   if(!image_valid()){boot_usb_error(DFU_ERROR_VERIFY);status_in();}
   else {dfu_state=MANIFEST_SYNC;status_in();}
  }
 }else if(request==3u && bm==0xa1u && !value && n==6u){
  uint32_t wait=0;
  if(dfu_state==DNLOAD_SYNC){dfu_state=BUSY;after_in=EXECUTE;wait=100u;}
  else if(dfu_state==MANIFEST_SYNC){dfu_state=MANIFEST;after_in=LEAVE;wait=100u;}
  status_reply[0]=dfu_status;status_reply[1]=(uint8_t)wait;status_reply[2]=0;
  status_reply[3]=0;status_reply[4]=dfu_state;status_reply[5]=0;reply(status_reply,6,n);
 }else if(request==5u && bm==0xa1u && !value && n==1u){reply(&dfu_state,1,n);
 }else if(request==4u && bm==0x21u && !value && !n && dfu_state==DFU_ERROR_STATE){
  dfu_status=0;dfu_state=IDLE;status_in();
 }else if(request==6u && bm==0x21u && !value && !n &&
  (dfu_state==IDLE || dfu_state==DNLOAD_SYNC || dfu_state==DNLOAD_IDLE || dfu_state==UPLOAD_IDLE)){
  dfu_state=IDLE;status_in();
 }else if(request==2u && bm==0xa1u && n && n<=256u && (dfu_state==IDLE || dfu_state==UPLOAD_IDLE)){
  uint16_t actual=n;
  if(!value){buffer[0]=0;buffer[1]=0x21;buffer[2]=0x41;actual=3;}
  else{
   uint32_t a;
   if(!target(value,n,&a)){boot_usb_error(DFU_ERROR_ADDRESS);return false;}
   if(!boot_media.Read((uint8_t *)a,buffer,n) || dfu_state==DFU_ERROR_STATE)return false;
  }
  dfu_state=actual<n?IDLE:UPLOAD_IDLE;reply(buffer,actual,n);
 }else return false;
 return true;
}
void ep0_setup(const uint8_t s[8]){
 /* A new SETUP aborts the old control transfer. Pending media work cannot be
  * interleaved: this callback and ep0_poll execute in one foreground context. */
 if(after_in==EXECUTE || after_in==LEAVE || pending!=NO_ACTION)
  boot_usb_error(DFU_ERROR_STALLEDPKT);
 phase=WAIT_SETUP;after_in=NO_ACTION;tx_zlp=false;
 uint8_t bm=s[0],request=s[1];uint16_t value=le16(s+2),index=le16(s+4),n=le16(s+6);
 if((bm&0x60u)==0u){if(!standard(bm,request,value,index,n))stall();}
 else if((bm&0x60u)==0x20u){if(!dfu_request(bm,request,value,index,n))dfu_stall();}
 else stall();
}
void ep0_out(const uint8_t *p,uint16_t n){
 if(phase==STATUS_OUT && !n){
  phase=WAIT_SETUP;
  /* The IN data packet alone does not complete a control read. Wait until
   * the host's zero-length OUT status packet has been received and ACKed
   * before erasing/programming Flash or resetting for manifestation. */
  if(after_in!=NO_ACTION){pending=after_in;after_in=NO_ACTION;}
  return;
 }
 if(phase!=DATA_OUT || !n || n>64u || n>length-received || (n<64u && n!=length-received)){
  dfu_stall();return;
 }
 memcpy(buffer+received,p,n);received+=n;
 if(received<length){ep0_port_receive();return;}
 if(!valid_download()){boot_usb_error(DFU_ERROR_ADDRESS);stall();return;}
 dfu_state=DNLOAD_SYNC;status_in();
}
void ep0_in_complete(void){
 if(phase==DATA_IN){
  if(tx_left){send_next();return;}
  if(tx_zlp){tx_zlp=false;ep0_port_send(buffer,0);return;}
  phase=STATUS_OUT;ep0_port_receive();
  return;
 }
 if(phase==STATUS_IN)phase=WAIT_SETUP;
 else return;
 if(after_in==3u){ep0_port_address(address);after_in=NO_ACTION;}
 else if(after_in!=NO_ACTION){pending=after_in;after_in=NO_ACTION;}
}
void ep0_poll(void){
 uint8_t action=pending;pending=NO_ACTION;
 if(action==LEAVE){ep0_port_leave();return;}
 if(action!=EXECUTE || dfu_state!=BUSY)return;
 if(block==0u){
  if(buffer[0]==0x21u)data_address=le32(buffer+1);
  else if(buffer[0]==0x41u && boot_media.Erase(le32(buffer+1))!=USBD_OK)
   boot_usb_error(DFU_ERROR_ERASE);
 }else{
  uint32_t a;
  if(!target(block,length,&a))boot_usb_error(DFU_ERROR_ADDRESS);
  else if(boot_media.Write(buffer,(uint8_t *)a,length)!=USBD_OK && dfu_state!=DFU_ERROR_STATE)
   boot_usb_error(DFU_ERROR_WRITE);
 }
 if(dfu_state!=DFU_ERROR_STATE)dfu_state=DNLOAD_IDLE;
}
