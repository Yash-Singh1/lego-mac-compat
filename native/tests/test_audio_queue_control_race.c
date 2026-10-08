/* CPU-only: keep a native control call in progress while another thread
   disposes its queue. Disposal must wait without holding the bridge lock. */
#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
static OSStatus mock_start(AudioQueueRef, const AudioTimeStamp *);
static OSStatus mock_pause(AudioQueueRef);
static OSStatus mock_stop(AudioQueueRef, Boolean);
static OSStatus mock_get(AudioQueueRef, AudioQueuePropertyID, void *, UInt32 *);
static OSStatus mock_set(AudioQueueRef, AudioQueuePropertyID, const void *, UInt32);
static OSStatus mock_parameter(AudioQueueRef, AudioQueueParameterID, AudioQueueParameterValue);
static OSStatus mock_dispose(AudioQueueRef, Boolean);
#define AudioQueueStart mock_start
#define AudioQueueDispose mock_dispose
#define AudioQueuePause mock_pause
#define AudioQueueStop mock_stop
#define AudioQueueGetProperty mock_get
#define AudioQueueSetProperty mock_set
#define AudioQueueSetParameter mock_parameter
#include "../src/audio_queue_bridge.c"
#undef AudioQueueStart
#undef AudioQueueDispose
static atomic_bool requested, native_disposed;
static bool disposed_inside;
static pthread_t disposer;
static struct queue *tested_queue;
static bool self_dispose, nested_control;
static struct queue *nested_queue;
static OSStatus dispose_status;
const char *guest_dyld32_describe(uint32_t a,uint32_t *o) { (void)a;(void)o;return NULL; }
uint32_t compat_runtime32_allocate(size_t n,int c) { (void)n;(void)c;abort(); }
void compat_runtime32_deallocate(uint32_t p) { (void)p;abort(); }
uint32_t compat_runtime32_call(uint32_t f,const uint32_t *a,size_t n) { (void)f;(void)a;(void)n;abort(); }
static void *dispose_worker(void *p) { atomic_store(&requested,true); assert(dispose(p,true)==dispose_status);return NULL; }
static OSStatus native_control(AudioQueueRef a) {
 if (nested_control) {
   if (a == (AudioQueueRef)0x1234) {
     uint32_t args[] = {nested_queue->handle, 0}; uint64_t result;
     assert(audio_queue_bridge32_dispatch("_AudioQueuePause", args, &result) && !result);
     assert(!atomic_load(&native_disposed));
   } else {
     assert(a == (AudioQueueRef)0x1235);
     callback_queue = nested_queue;
     assert(!dispose(tested_queue, true));
     callback_queue = NULL;
   }
   return noErr;
 }
 assert(a==(AudioQueueRef)0x1234);
 if (self_dispose) {
   callback_queue = tested_queue;
   assert(!dispose(tested_queue, true));
   callback_queue = NULL;
   assert(!atomic_load(&native_disposed));
   return noErr;
 }
 assert(!pthread_create(&disposer,NULL,dispose_worker,tested_queue));
 while(!atomic_load(&requested)) usleep(100);
 for(unsigned i=0;i<1000 && !atomic_load(&native_disposed);++i) usleep(100);
 disposed_inside=atomic_load(&native_disposed);
 return noErr;
}
static OSStatus mock_start(AudioQueueRef a,const AudioTimeStamp *t) { assert(!t);return native_control(a); }
static OSStatus mock_pause(AudioQueueRef a) { return native_control(a); }
static OSStatus mock_stop(AudioQueueRef a,Boolean i) { (void)i;return native_control(a); }
static OSStatus mock_get(AudioQueueRef a,AudioQueuePropertyID p,void *v,UInt32 *n) { (void)p;(void)v;(void)n;return native_control(a); }
static OSStatus mock_set(AudioQueueRef a,AudioQueuePropertyID p,const void *v,UInt32 n) { (void)p;(void)v;(void)n;return native_control(a); }
static OSStatus mock_parameter(AudioQueueRef a,AudioQueueParameterID p,AudioQueueParameterValue v) { (void)p;(void)v;return native_control(a); }
static OSStatus mock_dispose(AudioQueueRef a,Boolean immediate) {
 assert(a==(AudioQueueRef)0x1234 && immediate);atomic_store(&native_disposed,true);return dispose_status;
}
int main(void) {
 const char *names[]={"_AudioQueueStart","_AudioQueuePause","_AudioQueueStop",
   "_AudioQueueGetProperty","_AudioQueueSetProperty","_AudioQueueSetParameter"};
 for (unsigned i=0;i<7;++i) {
   queue_count=i+1;tested_queue=&queues[i];
   tested_queue->audio=(AudioQueueRef)0x1234;tested_queue->handle=kQueueHandleBase+i+1;
   atomic_store(&requested,false);atomic_store(&native_disposed,false);
   disposed_inside=false;self_dispose=i==6;
   uint32_t a[]={tested_queue->handle,0,0,0};uint64_t result;
   assert(audio_queue_bridge32_dispatch(names[i%6],a,&result) && !result);
   if(!self_dispose) pthread_join(disposer,NULL);
   if(disposed_inside) { fputs("FAIL: native disposal freed a queue during its control call\n",stderr);return 1; }
   assert(atomic_load(&native_disposed) && !tested_queue->audio);
 }
 /* A nested q2 native control retires the outer q1 on the same thread.
    Its callback marker names q2, so only the control-scope stack finds q1. */
 self_dispose=false;nested_control=true;queue_count=9;
 tested_queue=&queues[7];nested_queue=&queues[8];
 tested_queue->audio=(AudioQueueRef)0x1234;tested_queue->handle=kQueueHandleBase+8;
 nested_queue->audio=(AudioQueueRef)0x1235;nested_queue->handle=kQueueHandleBase+9;
 atomic_store(&native_disposed,false);
 uint32_t nested_args[]={tested_queue->handle,0};uint64_t nested_result;
 assert(audio_queue_bridge32_dispatch("_AudioQueueStart",nested_args,&nested_result) && !nested_result);
 assert(atomic_load(&native_disposed) && !tested_queue->audio);
 nested_control=false;
 /* Failed retirement restores the successful in-flight control's result,
    rather than the playback state at the beginning of retirement. */
 dispose_status=kAudio_ParamError;
 for(unsigned i=0;i<3;++i) {
   queue_count=10+i;tested_queue=&queues[9+i];
   tested_queue->audio=(AudioQueueRef)0x1234;tested_queue->handle=kQueueHandleBase+10+i;
   tested_queue->movie=true;
   set_playing(tested_queue,i!=0);
   atomic_store(&requested,false);atomic_store(&native_disposed,false);
   disposed_inside=false;
   uint32_t args[]={tested_queue->handle,0};uint64_t result;
   assert(audio_queue_bridge32_dispatch(names[i],args,&result) && !result);
   pthread_join(disposer,NULL);
   assert(!disposed_inside && tested_queue->audio && !tested_queue->disposing);
   assert(tested_queue->playing==(i==0));
   set_playing(tested_queue,false);
 }
 puts("AudioQueue control/disposal race PASS (six controls, callback self-disposal, nested queues, failed disposal playback)");return 0;
}
