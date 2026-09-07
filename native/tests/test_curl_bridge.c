#include "../src/curl_bridge.h"
#include <curl/curl.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static uint32_t cursor=0x30001000;static char received[64];static unsigned received_size;
uint32_t compat_runtime32_allocate(size_t n,int clear){uint32_t p=cursor;cursor+=(n+15)&~15u;assert(cursor<0x30100000);if(clear)memset((void *)(uintptr_t)p,0,n);return p;}
void compat_runtime32_deallocate(uint32_t p){assert(p>=0x30001000&&p<cursor);}
uint32_t compat_runtime32_copy_cstring(const char *s){uint32_t p=compat_runtime32_allocate(strlen(s)+1,0);strcpy((void *)(uintptr_t)p,s);return p;}
uint32_t compat_runtime32_call(uint32_t fn,const uint32_t *a,size_t n){assert(fn==1&&n==4&&a[3]==42);unsigned bytes=a[1]*a[2];assert(bytes+received_size<=sizeof(received));memcpy(received+received_size,(void *)(uintptr_t)a[0],bytes);received_size+=bytes;return bytes;}
static uint32_t call(const char *name,uint32_t *a){uint64_t r;assert(curl_bridge32_dispatch(name,a,&r));return r;}
int main(void){
 void *low=mmap((void *)0x30000000,0x100000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0);assert(low!=MAP_FAILED);
 char path[]="/tmp/lp32-curl-XXXXXX";int fd=mkstemp(path);assert(fd>=0);const char data[]="curl\0callback";assert(write(fd,data,sizeof(data))==sizeof(data));close(fd);
 char url[256];snprintf(url,sizeof(url),"file://%s",path);uint32_t u=compat_runtime32_copy_cstring(url);
 uint32_t a[8]={CURL_GLOBAL_DEFAULT};assert(call("_curl_global_init",a)==0);
 uint32_t easy=call("_curl_easy_init",a),multi=call("_curl_multi_init",a);assert(easy&&multi);
 a[0]=easy;a[1]=CURLOPT_URL;a[2]=u;assert(call("_curl_easy_setopt",a)==0);
 a[1]=CURLOPT_WRITEFUNCTION;a[2]=1;assert(call("_curl_easy_setopt",a)==0);
 a[1]=CURLOPT_WRITEDATA;a[2]=42;assert(call("_curl_easy_setopt",a)==0);
 a[0]=multi;a[1]=easy;assert(call("_curl_multi_add_handle",a)==0);
 uint32_t *running=(void *)(uintptr_t)0x30000000;running[1]=0x12345678;a[1]=(uint32_t)(uintptr_t)running;
 for(unsigned i=0;i<20;++i){assert(call("_curl_multi_perform",a)==0);if(!*running)break;}
 assert(!*running&&running[1]==0x12345678);assert(received_size==sizeof(data)&&!memcmp(received,data,sizeof(data)));
 uint32_t msg=call("_curl_multi_info_read",a);assert(msg);uint32_t *m=(void *)(uintptr_t)msg;assert(m[0]==CURLMSG_DONE&&m[1]==easy&&m[2]==CURLE_OK);
 a[0]=easy;a[1]=CURLINFO_RESPONSE_CODE;a[2]=(uint32_t)(uintptr_t)running;assert(call("_curl_easy_getinfo",a)==0);assert(running[1]==0x12345678);
 a[0]=multi;a[1]=easy;assert(call("_curl_multi_remove_handle",a)==0);assert(call("_curl_multi_cleanup",a)==0);
 a[0]=easy;call("_curl_easy_cleanup",a);call("_curl_global_cleanup",a);unlink(path);
 puts("curl PASS (local-file multi transfer, guest callback data, handles, 32-bit long output, cleanup)");
 return 0;
}
