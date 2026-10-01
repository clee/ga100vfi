#include "text_format.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CHECK(...) do { for(size_t n=0;n<120;n++){char a[128],b[128];memset(a,0x5a,sizeof a);memset(b,0x5a,sizeof b);int got=pwm_snprintf(a,n,__VA_ARGS__);int want=snprintf(b,n,__VA_ARGS__);assert(got==want);assert(!memcmp(a,b,sizeof a));} }while(0)
int main(void){
 CHECK("OK %s c=%c v=%u s=%d hex=%08lx short=%04lx %%","STATE",'x',UINT_MAX,INT_MIN,ULONG_MAX,7ul);
 CHECK("%ld,%lu,%x,%d,%u",LONG_MIN,ULONG_MAX,UINT_MAX,0,0u);
 for(unsigned x=0;x<10000;x+=17)CHECK("v=%u hex=%08lx s=%ld",x,(unsigned long)x,-(long)x);
 char b[4];assert(pwm_snprintf(b,sizeof b,"%f",1.0)==-1 && b[0]==0);
 assert(pwm_snprintf(NULL,0,"%lu",ULONG_MAX)>0);
 puts("Bounded protocol formatter matches libc lengths/bytes/truncation");
}
