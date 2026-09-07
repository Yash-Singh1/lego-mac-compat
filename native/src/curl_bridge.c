#include "curl_bridge.h"
#include "compat_runtime.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
struct handle { uint32_t token; void *p; unsigned kind; uint32_t write,read,header,wd,rd,hd; struct handle *next; };
static struct handle *handles;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct handle *lookup(uint32_t token) {
    pthread_mutex_lock(&lock);struct handle *p=handles;
    while(p && p->token!=token)p=p->next;
    pthread_mutex_unlock(&lock);return p;
}
static struct handle *wrap(void *p,unsigned kind) {
    if(!p)return NULL;struct handle *h=calloc(1,sizeof(*h));
    if(!h)return NULL;
    h->token=compat_runtime32_allocate(4,1);h->p=p;h->kind=kind;
    pthread_mutex_lock(&lock);h->next=handles;handles=h;pthread_mutex_unlock(&lock);return h;
}
static void forget(struct handle *h) {
    if(!h)return;pthread_mutex_lock(&lock);struct handle **p=&handles;
    while(*p && *p!=h)p=&(*p)->next;if(*p)*p=h->next;
    pthread_mutex_unlock(&lock);compat_runtime32_deallocate(h->token);free(h);
}
static size_t callback(char *p,size_t size,size_t count,struct handle *h,unsigned type) {
    uint32_t fn=type==0?h->write:type==1?h->read:h->header;
    uint32_t ud=type==0?h->wd:type==1?h->rd:h->hd;
    if(!fn)return type==1?0:size*count;
    if(size>UINT32_MAX || count>UINT32_MAX || (count && size>UINT32_MAX/count))return 0;
    uint32_t data=compat_runtime32_allocate(size*count,0);if(!data)return 0;
    if(type!=1)memcpy((void *)(uintptr_t)data,p,size*count);
    uint32_t a[]={data,(uint32_t)size,(uint32_t)count,ud};
    size_t n=compat_runtime32_call(fn,a,4);
    if(type==1 && n<=size*count)memcpy(p,(void *)(uintptr_t)data,n);
    compat_runtime32_deallocate(data);return n;
}
static size_t write_cb(char *p,size_t s,size_t n,void *h){return callback(p,s,n,h,0);}
static size_t read_cb(char *p,size_t s,size_t n,void *h){return callback(p,s,n,h,1);}
static size_t header_cb(char *p,size_t s,size_t n,void *h){return callback(p,s,n,h,2);}
int curl_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out) {
#define IS(n) (!strcmp(name,n))
#define P(i) ((void *)(uintptr_t)a[i])
    struct handle *h=lookup(a[0]);void *p=h?h->p:NULL;*out=0;
    if(IS("_curl_global_init"))*out=curl_global_init((int32_t)a[0]);
    else if(IS("_curl_global_cleanup"))curl_global_cleanup();
    else if(IS("_curl_multi_init") || IS("_curl_easy_init")) {
        bool multi=IS("_curl_multi_init"); h=wrap(multi?(void *)curl_multi_init():(void *)curl_easy_init(),multi?2:1);
        *out=h?h->token:0;
    } else if(IS("_curl_easy_cleanup")){curl_easy_cleanup(p);forget(h);}
    else if(IS("_curl_multi_cleanup")){*out=curl_multi_cleanup(p);forget(h);}
    else if(IS("_curl_easy_reset")){curl_easy_reset(p);if(h)h->write=h->read=h->header=h->wd=h->rd=h->hd=0;}
    else if(IS("_curl_multi_add_handle") || IS("_curl_multi_remove_handle")) {
        struct handle *e=lookup(a[1]);*out=IS("_curl_multi_add_handle")?curl_multi_add_handle(p,e?e->p:NULL):curl_multi_remove_handle(p,e?e->p:NULL);
    } else if(IS("_curl_multi_perform"))*out=curl_multi_perform(p,P(1));
    else if(IS("_curl_multi_fdset"))*out=curl_multi_fdset(p,P(1),P(2),P(3),P(4));
    else if(IS("_curl_multi_timeout")){long ms=0;*out=curl_multi_timeout(p,&ms);if(a[1])*(int32_t *)P(1)=(int32_t)ms;}
    else if(IS("_curl_multi_info_read")) {
        CURLMsg *msg=curl_multi_info_read(p,P(1));if(msg){
            uint32_t t=0;pthread_mutex_lock(&lock);for(struct handle *e=handles;e;e=e->next)if(e->p==msg->easy_handle)t=e->token;pthread_mutex_unlock(&lock);
            uint32_t words[]={msg->msg,t,(uint32_t)msg->data.result};uint32_t g=compat_runtime32_allocate(sizeof(words),0);memcpy((void *)(uintptr_t)g,words,sizeof(words));*out=g;
        }
    } else if(IS("_curl_slist_append")) {
        struct curl_slist *list=curl_slist_append(p,P(1));if(list){if(h)h->p=list;else h=wrap(list,3);*out=h?h->token:0;}
    } else if(IS("_curl_slist_free_all")){curl_slist_free_all(p);forget(h);}
    else if(IS("_curl_easy_setopt")) {
        CURLoption option=(CURLoption)a[1];uint32_t v=a[2];
        if(!h){*out=CURLE_BAD_FUNCTION_ARGUMENT;return 1;}
        if(option==CURLOPT_WRITEFUNCTION){h->write=v;*out=curl_easy_setopt(p,option,write_cb);curl_easy_setopt(p,CURLOPT_WRITEDATA,h);}
        else if(option==CURLOPT_READFUNCTION){h->read=v;*out=curl_easy_setopt(p,option,read_cb);curl_easy_setopt(p,CURLOPT_READDATA,h);}
        else if(option==CURLOPT_HEADERFUNCTION){h->header=v;*out=curl_easy_setopt(p,option,header_cb);curl_easy_setopt(p,CURLOPT_HEADERDATA,h);}
        else if(option==CURLOPT_WRITEDATA)h->wd=v;
        else if(option==CURLOPT_READDATA)h->rd=v;
        else if(option==CURLOPT_HEADERDATA)h->hd=v;
        else if(option==CURLOPT_HTTPHEADER || option==CURLOPT_QUOTE || option==CURLOPT_POSTQUOTE || option==CURLOPT_RESOLVE){struct handle *list=lookup(v);*out=curl_easy_setopt(p,option,list?list->p:NULL);}
        else if(option<CURLOPTTYPE_OBJECTPOINT)*out=curl_easy_setopt(p,option,(long)(int32_t)v);
        else if(option<CURLOPTTYPE_FUNCTIONPOINT)*out=curl_easy_setopt(p,option,(void *)(uintptr_t)v);
        else if(option<CURLOPTTYPE_OFF_T)*out=CURLE_UNKNOWN_OPTION;
        else {int64_t n;memcpy(&n,a+2,8);*out=curl_easy_setopt(p,option,(curl_off_t)n);}
    } else if(IS("_curl_easy_getinfo")) {
        CURLINFO info=(CURLINFO)a[1];
        if((info&CURLINFO_TYPEMASK)==CURLINFO_STRING){char *s=NULL;*out=curl_easy_getinfo(p,info,&s);*(uint32_t *)P(2)=s?compat_runtime32_copy_cstring(s):0;}
        else if((info&CURLINFO_TYPEMASK)==CURLINFO_LONG){long n=0;*out=curl_easy_getinfo(p,info,&n);*(int32_t *)P(2)=(int32_t)n;}
        else if((info&CURLINFO_TYPEMASK)==CURLINFO_DOUBLE)*out=curl_easy_getinfo(p,info,P(2));
        else *out=CURLE_UNKNOWN_OPTION;
    } else return 0;
    return 1;
}
