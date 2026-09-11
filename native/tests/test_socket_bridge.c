#include "socket_bridge.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
static uint32_t next=0x10002000;
uint32_t compat_runtime32_allocate(size_t size,int zero){uint32_t p=next;next+=(size+15)&~15u;assert(next<0x10010000);if(zero)memset((void *)(uintptr_t)p,0,size);return p;}
void compat_runtime32_deallocate(uint32_t p){(void)p;}
static int32_t call(const char *name,uint32_t *a){uint64_t out;assert(socket_bridge32_dispatch(name,a,&out));return (int32_t)out;}
int main(void){
    alarm(5);
    assert(mmap((void *)0x10000000,0x10000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==(void *)0x10000000);
    uint32_t create[]={AF_INET,SOCK_DGRAM,IPPROTO_UDP};int receiver=call("_socket",create),sender=call("_socket",create);assert(receiver>=0 && sender>=0);
    struct sockaddr_in *address=(void *)0x10000000;*address=(struct sockaddr_in){.sin_len=sizeof(*address),.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    uint32_t bind_args[]={receiver,0x10000000,sizeof(*address)};assert(!call("_bind",bind_args));
    uint32_t *length=(void *)0x10000100;length[0]=sizeof(*address);length[1]=0xabcdef01;
    uint32_t name_args[]={receiver,0x10000000,0x10000100};assert(!call("_getsockname",name_args));assert(length[1]==0xabcdef01);
    memcpy((void *)0x10000200,"loopback",8);uint32_t send_args[]={sender,0x10000200,8,0,0x10000000,sizeof(*address)};assert(call("_sendto",send_args)==8);
    fd_set *readable=(void *)0x10000400;FD_ZERO(readable);FD_SET(receiver,readable);
    int32_t *timeout=(void *)0x10000500;timeout[0]=1;timeout[1]=0;timeout[2]=0x76543210;
    uint32_t select_args[]={receiver+1,0x10000400,0,0,0x10000500};assert(call("_select",select_args)==1 && FD_ISSET(receiver,readable));assert(timeout[2]==0x76543210);
    uint32_t recv_args[]={receiver,0x10000300,16,0,0x10000000,0x10000100};assert(call("_recvfrom",recv_args)==8);assert(!memcmp((void *)0x10000300,"loopback",8) && length[1]==0xabcdef01);
    /* COD4's setup message arrives as thirteen full 1,310-byte UDP packets
       and a shorter final fragment. Keep the whole burst intact across the
       i386 socket ABI, including packets larger than a VPN's 1,300-byte MTU. */
    *(int *)0x10000600=262144;
    uint32_t buffer_args[]={receiver,SOL_SOCKET,SO_RCVBUF,0x10000600,4};
    assert(!call("_setsockopt",buffer_args));
    length[0]=sizeof(*address);assert(!call("_getsockname",name_args));
    unsigned char *packet=(void *)0x10000800,*received=(void *)0x10001000;
    for(unsigned fragment=0;fragment<14;++fragment){
        uint32_t sequence=0x80000002,offset=fragment*1300;
        uint16_t payload=fragment==13?1199:1300;
        memcpy(packet,&sequence,4);memcpy(packet+4,&offset,4);memcpy(packet+8,&payload,2);
        memset(packet+10,(int)fragment,payload);
        send_args[1]=0x10000800;send_args[2]=payload+10;
        assert(call("_sendto",send_args)==payload+10);
    }
    for(unsigned fragment=0;fragment<14;++fragment){
        unsigned payload=fragment==13?1199:1300;
        memset(received,0xcc,1311);length[0]=sizeof(*address);
        recv_args[1]=0x10001000;recv_args[2]=1310;
        assert(call("_recvfrom",recv_args)==(int32_t)(payload+10));
        uint32_t sequence,offset;uint16_t size;
        memcpy(&sequence,received,4);memcpy(&offset,received+4,4);memcpy(&size,received+8,2);
        assert(sequence==0x80000002 && offset==fragment*1300 && size==payload);
        for(unsigned i=0;i<payload;++i)assert(received[10+i]==fragment);
        assert(received[payload+10]==0xcc && length[1]==0xabcdef01);
    }
    timeout[0]=0;timeout[1]=250000;uint32_t set_args[]={receiver,SOL_SOCKET,SO_RCVTIMEO,0x10000500,8};assert(!call("_setsockopt",set_args));length[0]=8;uint32_t get_args[]={receiver,SOL_SOCKET,SO_RCVTIMEO,0x10000500,0x10000100};assert(!call("_getsockopt",get_args));assert(length[0]==8 && timeout[0]==0 && timeout[1]==250000 && timeout[2]==0x76543210 && length[1]==0xabcdef01);
    *(int *)0x10000600=1;uint32_t ioctl_args[]={receiver,FIONBIO,0x10000600};assert(!call("_ioctl",ioctl_args));assert(call("_recvfrom",recv_args)==-1 && errno==EAGAIN);
    strcpy((void *)0x10000700,"127.0.0.1");uint32_t resolve[]={0x10000700};uint32_t host=(uint32_t)call("_gethostbyname",resolve);assert(host);uint32_t *fields=(void *)(uintptr_t)host;assert(fields[2]==AF_INET && fields[3]==4);uint32_t *addresses=(void *)(uintptr_t)fields[4];assert(addresses[0] && !addresses[1]);assert(*(uint32_t *)(uintptr_t)addresses[0]==htonl(INADDR_LOOPBACK));
    close(sender);close(receiver);alarm(0);puts("Socket bridge PASS (UDP loopback, COD4 setup-fragment burst, select, timeouts, nonblocking errno, resolver pointers, canaries)");
}
