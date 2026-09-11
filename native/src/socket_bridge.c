#include "socket_bridge.h"
#include "compat_runtime.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
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

int socket_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out) {
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
    else if(IS("_gethostbyname")) { *out=guest_gethostbyname(P(0));return 1; }
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
