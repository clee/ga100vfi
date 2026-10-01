#include "control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char result[256];
static void cmd(pwm_control_t *c,uint32_t now,const char *verb){char line[256];snprintf(line,sizeof line,"%lu %lu %s",(unsigned long)c->session,(unsigned long)(c->last_seq+1),verb);ctrl_command(c,line,result,sizeof result,now);}
static pwm_health_t begin(pwm_control_t *c){
 pwm_health_t h={0};h.usb_connected=h.clocks_ok=h.usb_update_ready=true;
 h.gpu_capture_valid=h.gpu_capture_recent=true;h.gpu_ccr=64;
 ctrl_init(c);ctrl_tick(c,100,&h);ctrl_command(c,"HELLO 7",result,sizeof result,100);return h;
}
int main(void){
 pwm_control_t c;pwm_health_t h=begin(&c);cmd(&c,100,"DFU");
 assert(!strcmp(result,"OK DFU_PENDING") && c.dfu_requested);
 assert(!c.qualified && !c.target_valid && !ctrl_tick(&c,100,&h).permit);
 cmd(&c,100,"STOP"); /* STOP's unframed form is covered separately. */
 ctrl_command(&c,"STOP",result,sizeof result,100);assert(!c.dfu_requested);
 h=begin(&c);h.usb_update_ready=false;ctrl_tick(&c,100,&h);cmd(&c,100,"DFU");assert(!strcmp(result,"ERR DFU_UNSAFE"));
 h=begin(&c);h.run_feedback=true;ctrl_tick(&c,100,&h);cmd(&c,100,"DFU");assert(!strcmp(result,"ERR DFU_UNSAFE"));
 begin(&c);cmd(&c,121,"DFU");assert(!strcmp(result,"ERR DFU_UNSAFE"));
 begin(&c);cmd(&c,100,"PREARM");assert(c.state==PWM_PREARM);cmd(&c,100,"DFU");assert(!strcmp(result,"ERR DFU_UNSAFE"));
 begin(&c);ctrl_fault_isr(&c);cmd(&c,100,"DFU");assert(!strcmp(result,"ERR DFU_UNSAFE"));
 puts("DFU request gates: passive/fresh/session/software-watchdog/no RUN; no output authorization");
}
