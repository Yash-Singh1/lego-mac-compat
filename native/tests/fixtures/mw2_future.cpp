#include <pthread.h>
#include <stdint.h>
#include <time.h>

// A nontrivial one-word result exercises libc++ future<void>'s hidden-return
// ABI without depending on an obsolete C++ SDK.
struct Future {
    uint32_t state;
    ~Future() __asm__("__ZNSt3__16futureIvED1Ev");
};
extern Future get_future(uint32_t *) __asm__("__ZNSt3__17promiseIvE10get_futureEv");
extern void promise_init(uint32_t *) __asm__("__ZNSt3__17promiseIvEC1Ev");
extern void promise_destroy(uint32_t *) __asm__("__ZNSt3__17promiseIvED1Ev");
extern void set_value(uint32_t *) __asm__("__ZNSt3__17promiseIvE9set_valueEv");
extern void get(Future *) __asm__("__ZNSt3__16futureIvE3getEv");
extern int64_t now() __asm__("__ZNSt3__16chrono12steady_clock3nowEv");
static void *complete(void *promise) {
    timespec delay={0,5000000};nanosleep(&delay,0);
    set_value(static_cast<uint32_t *>(promise));return promise;
}
extern "C" int MW2FutureTest() {
    struct { uint32_t state,guard; } promise={0,0x12345678};
    promise_init(&promise.state);
    if(!promise.state || promise.guard!=0x12345678)return -51;
    Future future=get_future(&promise.state);
    if(future.state!=promise.state || promise.guard!=0x12345678)return -52;
    pthread_t worker;
    int64_t start=now();
    if(pthread_create(&worker,0,complete,&promise.state))return -53;
    get(&future);
    if(future.state || now()-start<1000000)return -54;
    void *result=0;
    if(pthread_join(worker,&result) || result!=&promise.state)return -55;
    promise_destroy(&promise.state);
    if(promise.state || promise.guard!=0x12345678)return -56;
    return 0;
}
