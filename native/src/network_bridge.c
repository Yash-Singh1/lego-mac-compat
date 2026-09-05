#include "network_bridge.h"
#include "compat_runtime.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static pthread_mutex_t resolver_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local uint32_t guest_hostent;

/* Deep-copy every pointer in hostent into one guest allocation. */
static uint32_t copy_hostent(const struct hostent *entry)
{
    if (!entry) return 0;
    size_t aliases = 0, addresses = 0, names_size = strlen(entry->h_name) + 1;
    while (entry->h_aliases && entry->h_aliases[aliases]) names_size += strlen(entry->h_aliases[aliases++]) + 1;
    while (entry->h_addr_list && entry->h_addr_list[addresses]) ++addresses;
    if (entry->h_length < 0) return 0;
    size_t size = 20 + (aliases + addresses + 2) * 4 + names_size + addresses * entry->h_length;
    if (guest_hostent) compat_runtime32_deallocate(guest_hostent);
    guest_hostent = compat_runtime32_allocate(size, 1);
    if (!guest_hostent) return 0;
    uint32_t *words = (void *)(uintptr_t)guest_hostent;
    uint32_t *alias_array = words + 5, *address_array = alias_array + aliases + 1;
    char *cursor = (void *)(address_array + addresses + 1);
    words[0] = (uint32_t)(uintptr_t)cursor;
    words[1] = (uint32_t)(uintptr_t)alias_array;
    words[2] = entry->h_addrtype; words[3] = entry->h_length;
    words[4] = (uint32_t)(uintptr_t)address_array;
    strcpy(cursor, entry->h_name); cursor += strlen(cursor) + 1;
    for (size_t i = 0; i < aliases; ++i) {
        alias_array[i] = (uint32_t)(uintptr_t)cursor;
        strcpy(cursor, entry->h_aliases[i]); cursor += strlen(cursor) + 1;
    }
    for (size_t i = 0; i < addresses; ++i) {
        address_array[i] = (uint32_t)(uintptr_t)cursor;
        memcpy(cursor, entry->h_addr_list[i], entry->h_length); cursor += entry->h_length;
    }
    return guest_hostent;
}

int network_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *r)
{
#define IS(s) (!strcmp(name, s))
#define PTR(i) ((void *)(uintptr_t)a[i])
    if (IS("_gethostname")) *r = (uint32_t)gethostname(PTR(0), a[1]);
    else if (IS("___darwin_check_fd_set_overflow")) *r = (uint32_t)__darwin_check_fd_set_overflow(a[0], PTR(1), a[2]);
    else if (IS("_gethostbyname") || IS("_gethostbyaddr")) {
        pthread_mutex_lock(&resolver_lock);
        struct hostent *entry = IS("_gethostbyname") ? gethostbyname(PTR(0)) : gethostbyaddr(PTR(0), a[1], a[2]);
        *r = copy_hostent(entry);
        pthread_mutex_unlock(&resolver_lock);
    } else if (IS("_inet_addr")) *r = inet_addr(PTR(0));
    else if (IS("_inet_ntoa")) { struct in_addr address = {a[0]}; *r = compat_runtime32_copy_cstring(inet_ntoa(address)); }
    else if (IS("_inet_pton")) *r = (uint32_t)inet_pton(a[0], PTR(1), PTR(2));
    else if (IS("_inet_ntop")) *r = (uint32_t)(uintptr_t)inet_ntop(a[0], PTR(1), PTR(2), a[3]);
    else if (IS("_socket")) *r = (uint32_t)socket(a[0], a[1], a[2]);
    else if (IS("_socketpair")) *r = (uint32_t)socketpair(a[0], a[1], a[2], PTR(3));
    else if (IS("_bind")) *r = (uint32_t)bind(a[0], PTR(1), a[2]);
    else if (IS("_connect")) *r = (uint32_t)connect(a[0], PTR(1), a[2]);
    else if (IS("_listen")) *r = (uint32_t)listen(a[0], a[1]);
    else if (IS("_accept")) *r = (uint32_t)accept(a[0], PTR(1), PTR(2));
    else if (IS("_shutdown")) *r = (uint32_t)shutdown(a[0], a[1]);
    else if (IS("_send")) *r = (uint32_t)send(a[0], PTR(1), a[2], a[3]);
    else if (IS("_recv")) *r = (uint32_t)recv(a[0], PTR(1), a[2], a[3]);
    else if (IS("_sendto")) *r = (uint32_t)sendto(a[0], PTR(1), a[2], a[3], PTR(4), a[5]);
    else if (IS("_recvfrom")) *r = (uint32_t)recvfrom(a[0], PTR(1), a[2], a[3], PTR(4), PTR(5));
    else if (IS("_getsockname")) *r = (uint32_t)getsockname(a[0], PTR(1), PTR(2));
    else if (IS("_getpeername")) *r = (uint32_t)getpeername(a[0], PTR(1), PTR(2));
    else if (IS("_setsockopt")) {
        if (a[1] == SOL_SOCKET && (a[2] == SO_RCVTIMEO || a[2] == SO_SNDTIMEO) && a[4] == 8) {
            int32_t *guest = PTR(3); struct timeval value = {guest[0], guest[1]};
            *r = (uint32_t)setsockopt(a[0], a[1], a[2], &value, sizeof(value));
        } else *r = (uint32_t)setsockopt(a[0], a[1], a[2], PTR(3), a[4]);
    } else if (IS("_getsockopt")) {
        if (a[1] == SOL_SOCKET && (a[2] == SO_RCVTIMEO || a[2] == SO_SNDTIMEO)) {
            int32_t *guest = PTR(3); socklen_t *size = PTR(4);
            struct timeval value = {0}; socklen_t length = sizeof(value);
            if (!size || *size < 8) { errno = EINVAL; *r = UINT32_MAX; }
            else {
                *r = (uint32_t)getsockopt(a[0], a[1], a[2], &value, &length);
                if (!*r) { guest[0] = (int32_t)value.tv_sec; guest[1] = value.tv_usec; *size = 8; }
            }
        } else *r = (uint32_t)getsockopt(a[0], a[1], a[2], PTR(3), PTR(4));
    } else if (IS("_ioctl")) {
        if (a[1] == FIONBIO || a[1] == FIONREAD) *r = (uint32_t)ioctl(a[0], (unsigned long)a[1], PTR(2));
        else { errno = ENOTSUP; *r = UINT32_MAX; }
    } else if (IS("_select") || IS("_select$DARWIN_EXTSN")) {
        int32_t *guest = PTR(4); struct timeval timeout = {0};
        if (guest) { timeout.tv_sec = guest[0]; timeout.tv_usec = guest[1]; }
        *r = (uint32_t)select(a[0], PTR(1), PTR(2), PTR(3), guest ? &timeout : NULL);
        if (guest) { guest[0] = (int32_t)timeout.tv_sec; guest[1] = timeout.tv_usec; }
    } else if (IS("_poll")) *r = (uint32_t)poll(PTR(0), a[1], a[2]);
    else return 0;
    return 1;
#undef IS
#undef PTR
}
