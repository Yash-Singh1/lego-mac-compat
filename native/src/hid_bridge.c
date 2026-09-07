#include "hid_bridge.h"
#include <ForceFeedback/ForceFeedback.h>
#include "objc_bridge.h"
#include "compat_runtime.h"
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDValue.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
/* Callback contexts outlive manager closure: a queued native notification can
 * still hold one. They are bounded by registration count, not input events. */
struct callback {void *manager;unsigned kind;uint32_t function,context;struct callback *next;};
static struct callback *callbacks;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct callback *registration(void *manager,unsigned kind,uint32_t fn,uint32_t ctx){
    pthread_mutex_lock(&lock);struct callback *c=callbacks;
    while(c&&(c->manager!=manager||c->kind!=kind))c=c->next;
    if(!c){c=calloc(1,sizeof(*c));if(c){c->manager=manager;c->kind=kind;c->next=callbacks;callbacks=c;}}
    if(c){c->function=fn;c->context=ctx;}pthread_mutex_unlock(&lock);return c;
}
static void invoke(void *raw,IOReturn status,void *sender,CFTypeRef value){
    struct callback *c=raw;pthread_mutex_lock(&lock);uint32_t fn=c->function,ctx=c->context;pthread_mutex_unlock(&lock);
    if(fn){uint32_t a[]={ctx,(uint32_t)status,objc_bridge32_guest_object(sender),objc_bridge32_guest_object((void *)value)};compat_runtime32_call(fn,a,4);}
}
static void device_callback(void *raw,IOReturn status,void *sender,IOHIDDeviceRef device){invoke(raw,status,sender,device);}
static void value_callback(void *raw,IOReturn status,void *sender,IOHIDValueRef value){invoke(raw,status,sender,value);}
int hid_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out){
    if (!strcmp(name, "_FFIsForceFeedback")) {
        *out = (uint32_t)FFIsForceFeedback(a[0]);
        return 1;
    }
    if(strncmp(name,"_IOHID",6))return 0;
#define IS(n) (!strcmp(name,n))
#define O(i) objc_bridge32_host_object(a[i])
    if(IS("_IOHIDManagerCreate")){IOHIDManagerRef manager=IOHIDManagerCreate(NULL,a[1]);*out=objc_bridge32_guest_object(manager);if(manager)CFRelease(manager);}
    else if(IS("_IOHIDManagerOpen"))*out=(uint32_t)IOHIDManagerOpen(O(0),a[1]);
    else if(IS("_IOHIDManagerClose"))*out=(uint32_t)IOHIDManagerClose(O(0),a[1]);
    else if(IS("_IOHIDManagerCopyDevices")){CFSetRef devices=IOHIDManagerCopyDevices(O(0));*out=objc_bridge32_guest_object((void *)devices);if(devices)CFRelease(devices);}
    else if(IS("_IOHIDManagerSetDeviceMatching")){IOHIDManagerSetDeviceMatching(O(0),O(1));*out=0;}
    else if(IS("_IOHIDManagerSetDeviceMatchingMultiple")){IOHIDManagerSetDeviceMatchingMultiple(O(0),O(1));*out=0;}
    else if(IS("_IOHIDManagerScheduleWithRunLoop")){IOHIDManagerScheduleWithRunLoop(O(0),O(1),O(2));*out=0;}
    else if(IS("_IOHIDManagerUnscheduleFromRunLoop")){IOHIDManagerUnscheduleFromRunLoop(O(0),O(1),O(2));*out=0;}
    else if(IS("_IOHIDDeviceUnscheduleFromRunLoop")){IOHIDDeviceUnscheduleFromRunLoop(O(0),O(1),O(2));*out=0;}
    else if(IS("_IOHIDManagerRegisterDeviceMatchingCallback")||IS("_IOHIDManagerRegisterDeviceRemovalCallback")||IS("_IOHIDManagerRegisterInputValueCallback")){
        unsigned kind=IS("_IOHIDManagerRegisterDeviceMatchingCallback")?0:IS("_IOHIDManagerRegisterDeviceRemovalCallback")?1:2;
        struct callback *c=registration(O(0),kind,a[1],a[2]);if(!c)return 0;
        if(kind==0)IOHIDManagerRegisterDeviceMatchingCallback(O(0),a[1]?device_callback:NULL,c);
        else if(kind==1)IOHIDManagerRegisterDeviceRemovalCallback(O(0),a[1]?device_callback:NULL,c);
        else IOHIDManagerRegisterInputValueCallback(O(0),a[1]?value_callback:NULL,c);*out=0;
    }
    else if(IS("_IOHIDDeviceGetProperty"))*out=objc_bridge32_guest_object((void *)IOHIDDeviceGetProperty(O(0),O(1)));
    else if(IS("_IOHIDDeviceSetReport"))*out=(uint32_t)IOHIDDeviceSetReport(O(0),a[1],a[2],(void *)(uintptr_t)a[3],(int32_t)a[4]);
    else if(IS("_IOHIDElementGetCookie"))*out=IOHIDElementGetCookie(O(0));
    else if(IS("_IOHIDElementGetUsage"))*out=IOHIDElementGetUsage(O(0));
    else if(IS("_IOHIDElementGetUsagePage"))*out=IOHIDElementGetUsagePage(O(0));
    else if(IS("_IOHIDElementGetDevice"))*out=objc_bridge32_guest_object(IOHIDElementGetDevice(O(0)));
    else if(IS("_IOHIDValueGetElement"))*out=objc_bridge32_guest_object(IOHIDValueGetElement(O(0)));
    else if(IS("_IOHIDValueGetIntegerValue"))*out=(uint32_t)IOHIDValueGetIntegerValue(O(0));
    else if(IS("_IOHIDValueGetTimeStamp"))*out=IOHIDValueGetTimeStamp(O(0));
    else return 0;
    return 1;
#undef IS
#undef O
}
