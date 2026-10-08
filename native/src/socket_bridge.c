#include "socket_bridge.h"
#include "compat_runtime.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

struct guest_timeval { int32_t seconds, microseconds; };
struct guest_hostent { uint32_t name, aliases; int32_t type, length; uint32_t addresses; };
static pthread_mutex_t resolver_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local uint32_t resolver_storage;

/* The legacy resolver returns borrowed pointer arrays. Copy the entire result
   while holding the resolver lock, retaining it until this thread resolves again. */
static uint32_t guest_gethostbyname(const char *name) {
    pthread_mutex_lock(&resolver_lock);
    struct hostent *host = gethostbyname(name);
    if (!host || host->h_length <= 0) { pthread_mutex_unlock(&resolver_lock); return 0; }
    size_t aliases=0, addresses=0, bytes=sizeof(struct guest_hostent);
    while (host->h_aliases[aliases]) ++aliases;
    while (host->h_addr_list[addresses]) ++addresses;
    bytes += 4 * (aliases + addresses + 2) + strlen(host->h_name) + 1;
    for (size_t i=0;i<aliases;++i) bytes += strlen(host->h_aliases[i])+1;
    bytes += addresses * (size_t)host->h_length;
    uint32_t storage=compat_runtime32_allocate(bytes,0);
    if (!storage) { pthread_mutex_unlock(&resolver_lock); errno=ENOMEM; return 0; }
    memset((void *)(uintptr_t)storage,0,bytes);
    struct guest_hostent *out=(void *)(uintptr_t)storage;
    out->type=host->h_addrtype; out->length=host->h_length;
    out->aliases=storage+sizeof(*out);out->addresses=out->aliases+4*(aliases+1);
    uint32_t cursor=out->addresses+4*(addresses+1);
    out->name=cursor;size_t length=strlen(host->h_name)+1;
    memcpy((void *)(uintptr_t)cursor,host->h_name,length);cursor+=length;
    uint32_t *alias=(void *)(uintptr_t)out->aliases,*address=(void *)(uintptr_t)out->addresses;
    for(size_t i=0;i<aliases;++i){length=strlen(host->h_aliases[i])+1;alias[i]=cursor;memcpy((void *)(uintptr_t)cursor,host->h_aliases[i],length);cursor+=length;}
    for(size_t i=0;i<addresses;++i){address[i]=cursor;memcpy((void *)(uintptr_t)cursor,host->h_addr_list[i],host->h_length);cursor+=host->h_length;}
    pthread_mutex_unlock(&resolver_lock);
    if(resolver_storage)compat_runtime32_deallocate(resolver_storage);
    resolver_storage=storage;return storage;
}

/* Darwin's i386 addrinfo keeps the host field order with 32-bit pointers. */
struct guest_addrinfo { int32_t flags, family, socktype, protocol; uint32_t addrlen, canonname, addr, next; };
_Static_assert(sizeof(struct guest_addrinfo)==32,"i386 addrinfo");

static void guest_freeaddrinfo(uint32_t list) {
    while(list) { uint32_t next=((struct guest_addrinfo *)(uintptr_t)list)->next;compat_runtime32_deallocate(list);list=next; }
}

/* Each result node, its address and its canonical name share one guest block. */
static int guest_getaddrinfo(const char *node,const char *service,const struct guest_addrinfo *guest_hints,uint32_t *result) {
    struct addrinfo hints={0},*host=NULL;
    if(guest_hints) { hints.ai_flags=guest_hints->flags;hints.ai_family=guest_hints->family;hints.ai_socktype=guest_hints->socktype;hints.ai_protocol=guest_hints->protocol; }
    int status=getaddrinfo(node,service,guest_hints?&hints:NULL,&host);
    if(status)return status;
    uint32_t head=0,*link=&head;
    for(struct addrinfo *entry=host;entry;entry=entry->ai_next) {
        size_t name=entry->ai_canonname?strlen(entry->ai_canonname)+1:0;
        uint32_t block=compat_runtime32_allocate(sizeof(struct guest_addrinfo)+entry->ai_addrlen+name,1);
        if(!block) { guest_freeaddrinfo(head);freeaddrinfo(host);return EAI_MEMORY; }
        struct guest_addrinfo *copy=(void *)(uintptr_t)block;
        *copy=(struct guest_addrinfo){entry->ai_flags,entry->ai_family,entry->ai_socktype,entry->ai_protocol,entry->ai_addrlen,0,0,0};
        uint32_t cursor=block+sizeof(*copy);
        if(entry->ai_addr) { memcpy((void *)(uintptr_t)cursor,entry->ai_addr,entry->ai_addrlen);copy->addr=cursor;cursor+=entry->ai_addrlen; }
        if(name) { memcpy((void *)(uintptr_t)cursor,entry->ai_canonname,name);copy->canonname=cursor; }
        *link=block;link=&copy->next;
    }
    freeaddrinfo(host);*result=head;return 0;
}

static int socket_bridge32_dispatch_body(const char *name,const uint32_t *a,uint64_t *out);

/* LP32_TRACE_SOCKETS[=lines]: lookups, connects and packets with results. */
static void trace_socket_call(const char *name,const uint32_t *a,uint64_t result,int error) {
    static long limit=-1;static long lines;
    if(limit<0){const char *v=getenv("LP32_TRACE_SOCKETS");limit=v?(atol(v)>0?atol(v):300):0;}
    /* Nonblocking polls that found nothing are not logged. */
    if(lines>=limit || ((int32_t)result==-1 && error==EAGAIN) || !strcmp(name,"_select"))return;
    ++lines;
    char detail[128]="";
    if(!strcmp(name,"_gethostbyname") || !strcmp(name,"_getaddrinfo") || !strcmp(name,"_inet_addr"))
        snprintf(detail,sizeof(detail)," host=%.80s",a[0]?(const char *)(uintptr_t)a[0]:"(null)");
    else if((!strcmp(name,"_connect") || !strcmp(name,"_sendto") || !strcmp(name,"_bind")) && a[!strcmp(name,"_sendto")?4:1]) {
        const struct sockaddr_in *address=(const void *)(uintptr_t)a[!strcmp(name,"_sendto")?4:1];
        if(address->sin_family==AF_INET)
            snprintf(detail,sizeof(detail)," to=%s:%u",inet_ntoa(address->sin_addr),ntohs(address->sin_port));
    }
    if((!strcmp(name,"_send") || !strcmp(name,"_recv")) && (int32_t)result>0 && a[1]) {
        const unsigned char *bytes=(const void *)(uintptr_t)a[1];
        size_t used=strlen(detail);
        for(int32_t i=0;i<(int32_t)result && i<24 && used+3<sizeof(detail);++i,used+=2)
            snprintf(detail+used,sizeof(detail)-used,i?"%02x":" data=%02x",bytes[i]),used+=(i?0:6);
    }
    fprintf(stderr,"compat32: socket %s(%08x,%08x,%08x) -> %lld errno=%d%s\n",name,a[0],a[1],a[2],
            (long long)(int32_t)result,error,detail);
}

int socket_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out) {
    int handled=socket_bridge32_dispatch_body(name,a,out);
    if(handled)trace_socket_call(name,a,*out,errno);
    return handled;
}

static int socket_bridge32_dispatch_body(const char *name,const uint32_t *a,uint64_t *out) {
#define IS(s) (!strcmp(name,s))
#define P(i) ((void *)(uintptr_t)a[i])
    int result;
    if(IS("_socket")) result=socket((int)a[0],(int)a[1],(int)a[2]);
    else if(IS("_bind")) result=bind((int)a[0],P(1),(socklen_t)a[2]);
    else if(IS("_connect")) result=connect((int)a[0],P(1),(socklen_t)a[2]);
    else if(IS("_send")) result=(int)send((int)a[0],P(1),a[2],(int)a[3]);
    else if(IS("_recv")) result=(int)recv((int)a[0],P(1),a[2],(int)a[3]);
    else if(IS("_sendto")) result=(int)sendto((int)a[0],P(1),a[2],(int)a[3],P(4),(socklen_t)a[5]);
    else if(IS("_recvfrom")) result=(int)recvfrom((int)a[0],P(1),a[2],(int)a[3],P(4),P(5));
    else if(IS("_getsockname")) result=getsockname((int)a[0],P(1),P(2));
    else if(IS("_gethostname")) result=gethostname(P(0),a[1]);
    else if(IS("_inet_addr")) { *out=inet_addr(P(0));return 1; }
    else if(IS("_inet_ntoa")) {
        /* struct in_addr is passed by value; the result is a per-thread static. */
        static _Thread_local uint32_t text;
        if(!text) text=compat_runtime32_allocate(INET_ADDRSTRLEN,1);
        struct in_addr address={.s_addr=a[0]};
        if(text) strlcpy((char *)(uintptr_t)text,inet_ntoa(address),INET_ADDRSTRLEN);
        *out=text;return 1;
    }
    /* struct pollfd is {int, short, short} on i386 and x86_64 alike. */
    else if(IS("_poll")) result=poll(P(0),(nfds_t)a[1],(int)a[2]);
    else if(IS("_gethostbyname")) { *out=guest_gethostbyname(P(0));return 1; }
    else if(IS("_getaddrinfo")) { *out=(uint32_t)guest_getaddrinfo(P(0),P(1),P(2),P(3));return 1; }
    else if(IS("_freeaddrinfo")) { guest_freeaddrinfo(a[0]);return 1; }
    else if(IS("_ioctl")) {
        if(a[1]==FIONBIO || a[1]==FIONREAD) result=ioctl((int)a[0],(unsigned long)a[1],P(2));
        else { errno=ENOTTY;result=-1; }
    } else if(IS("_select")) {
        struct timeval timeout,*ptr=NULL;
        if(a[4]) { struct guest_timeval *guest=P(4);timeout.tv_sec=guest->seconds;timeout.tv_usec=guest->microseconds;ptr=&timeout; }
        result=select((int)a[0],P(1),P(2),P(3),ptr);
        if(ptr) { struct guest_timeval *guest=P(4);guest->seconds=(int32_t)timeout.tv_sec;guest->microseconds=timeout.tv_usec; }
    } else if(IS("_setsockopt") || IS("_getsockopt")) {
        int get=IS("_getsockopt");
        if(a[1]==SOL_SOCKET && (a[2]==SO_RCVTIMEO || a[2]==SO_SNDTIMEO)) {
            struct timeval timeout;
            if(!a[3] || (get && !a[4])) { errno=EFAULT;result=-1; }
            else if((get ? *(socklen_t *)P(4) : a[4]) < sizeof(struct guest_timeval)) {errno=EINVAL;result=-1;}
            else if(get) {socklen_t size=sizeof(timeout);result=getsockopt((int)a[0],a[1],a[2],&timeout,&size);if(!result){struct guest_timeval *guest=P(3);guest->seconds=(int32_t)timeout.tv_sec;guest->microseconds=timeout.tv_usec;*(socklen_t *)P(4)=sizeof(*guest);}}
            else {struct guest_timeval *guest=P(3);timeout.tv_sec=guest->seconds;timeout.tv_usec=guest->microseconds;result=setsockopt((int)a[0],a[1],a[2],&timeout,sizeof(timeout));}
        } else if(get) result=getsockopt((int)a[0],a[1],a[2],P(3),P(4));
        else result=setsockopt((int)a[0],a[1],a[2],P(3),a[4]);
    } else return 0;
    *out=(uint32_t)result;return 1;
#undef P
#undef IS
}
