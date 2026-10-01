#ifndef PWMVID_TEXT_FORMAT_H
#define PWMVID_TEXT_FORMAT_H
/* Deliberately small, bounded integer formatter for this protocol only.
 * Supported: %s %c %u %d %x %lu %ld %lx, zero-padded width <=16, %%.
 * snprintf-style length/truncation; no floats, dynamic allocation or locale. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
static inline void pwm_put(char *out,size_t cap,size_t *at,char ch) {
    if(cap && *at < cap-1u)out[*at]=ch;
    ++*at;
}
static inline int pwm_vsnprintf(char *,size_t,const char *,va_list) __attribute__((format(printf,3,0)));
static inline int pwm_vsnprintf(char *out,size_t cap,const char *fmt,va_list ap) {
    size_t at=0;
    if((cap && !out) || !fmt)return -1;
    while(*fmt) {
        if(*fmt!='%'){pwm_put(out,cap,&at,*fmt++);continue;}
        ++fmt;
        if(*fmt=='%'){pwm_put(out,cap,&at,*fmt++);continue;}
        unsigned width=0;bool zero=false,islong=false;
        if(*fmt=='0'){zero=true;++fmt;}
        while(*fmt>='0' && *fmt<='9') {
            width=width*10u+(unsigned)(*fmt++-'0');
            if(width>16u)goto invalid;
        }
        if(*fmt=='l'){islong=true;++fmt;}
        char kind=*fmt;
        if(!kind)goto invalid;
        ++fmt;
        if(kind=='s' && !islong && !width){
            const char *p=va_arg(ap,const char *);if(!p)p="(null)";
            while(*p)pwm_put(out,cap,&at,*p++);
        } else if(kind=='c' && !islong && !width) {
            pwm_put(out,cap,&at,(char)va_arg(ap,int));
        } else if(kind=='u' || kind=='d' || kind=='x') {
            unsigned long value;bool negative=false;
            if(kind=='d') {
                long v=islong?va_arg(ap,long):(long)va_arg(ap,int);
                negative=v<0;value=negative?0ul-(unsigned long)v:(unsigned long)v;
            } else value=islong?va_arg(ap,unsigned long):(unsigned long)va_arg(ap,unsigned int);
            char digits[sizeof(unsigned long)*CHAR_BIT+1u];unsigned count=0;
            unsigned radix=kind=='x'?16u:10u;
            do {digits[count++]="0123456789abcdef"[value%radix];value/=radix;}while(value);
            unsigned len=count+(negative?1u:0u);
            if(negative && zero)pwm_put(out,cap,&at,'-');
            while(width>len){pwm_put(out,cap,&at,zero?'0':' ');--width;}
            if(negative && !zero)pwm_put(out,cap,&at,'-');
            while(count)pwm_put(out,cap,&at,digits[--count]);
        } else goto invalid;
    }
    if(cap)out[at<cap?at:cap-1u]=0;
    return at>(size_t)INT_MAX?-1:(int)at;
invalid:
    if(cap)out[0]=0;
    return -1;
}
static inline int pwm_snprintf(char *,size_t,const char *,...) __attribute__((format(printf,3,4)));
static inline int pwm_snprintf(char *out,size_t cap,const char *fmt,...) {
    va_list ap;va_start(ap,fmt);int n=pwm_vsnprintf(out,cap,fmt,ap);va_end(ap);return n;
}
#endif
