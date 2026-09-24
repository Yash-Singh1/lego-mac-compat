#include "hid_bridge.h"
#include <ForceFeedback/ForceFeedback.h>
#include "objc_bridge.h"
#include "compat_runtime.h"
#include "game_profile.h"
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDValue.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <mach/mach_time.h>
/* Callback contexts outlive manager closure: a queued native notification can
 * still hold one. They are bounded by registration count, not input events. */
struct callback {void *manager;unsigned kind;uint32_t function,context;struct callback *next;};
static struct callback *callbacks;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int trace_hid(void){return getenv("LP32_TRACE_HID")!=NULL;}
enum {kSagaPads=4,kSagaVirtualVendor=0x7f32,kSagaVirtualProduct=0x7f32};
struct saga_pad {IOHIDDeviceRef device;IOHIDElementRef element;void *manager;char name[128];int virtual_profile,semantic_active;};
static struct saga_pad saga_pads[kSagaPads];
static unsigned saga_pad_count;
static _Thread_local IOHIDDeviceRef matching_saga_device;
static _Thread_local IOHIDValueRef synthetic_value;
static _Thread_local IOHIDElementRef synthetic_element;
static _Thread_local unsigned synthetic_page,synthetic_usage;
static _Thread_local int synthetic_integer;
static int saga_pad_index(IOHIDDeviceRef device){
    for(unsigned i=0;i<saga_pad_count;++i)
        if(saga_pads[i].device && saga_pads[i].device==device)return (int)i;
    return -1;
}
static uint32_t guest_word(uint32_t address){uint32_t result=0;memcpy(&result,(void *)(uintptr_t)address,4);return result;}
static int saga_has_profile(uint32_t context,uint32_t id){
    uint32_t node=guest_word(context+0x4c);
    for(unsigned i=0;node&&i<128;++i){
        uint32_t key=guest_word(node+0x10);
        if(key==id)return 1;
        node=guest_word(node+(id<key?0:4));
    }
    return 0;
}
static int saga_gamepad(IOHIDDeviceRef device){
    CFNumberRef page=IOHIDDeviceGetProperty(device,CFSTR(kIOHIDPrimaryUsagePageKey));
    CFNumberRef usage=IOHIDDeviceGetProperty(device,CFSTR(kIOHIDPrimaryUsageKey));
    int p=0,u=0;
    if(page)CFNumberGetValue(page,kCFNumberIntType,&p);
    if(usage)CFNumberGetValue(usage,kCFNumberIntType,&u);
    return p==1&&(u==4||u==5);
}
static IOHIDElementRef saga_input_element(IOHIDDeviceRef device){
    CFArrayRef elements=IOHIDDeviceCopyMatchingElements(device,NULL,0);
    if(!elements)return NULL;
    IOHIDElementRef result=NULL;
    for(CFIndex i=0;i<CFArrayGetCount(elements);++i){
        IOHIDElementRef element=(IOHIDElementRef)CFArrayGetValueAtIndex(elements,i);
        if(IOHIDElementGetUsagePage(element)==9){result=(IOHIDElementRef)CFRetain(element);break;}
    }
    CFRelease(elements);
    return result;
}
static void saga_register_pad(IOHIDDeviceRef device,void *manager,int virtual_profile){
    if(saga_pad_index(device)>=0)return;
    IOHIDElementRef element=saga_input_element(device);
    if(!element)return;
    unsigned index=0;
    while(index<saga_pad_count && saga_pads[index].device)++index;
    if(index==kSagaPads){CFRelease(element);return;}
    if(index==saga_pad_count)++saga_pad_count;
    struct saga_pad *pad=&saga_pads[index];
    pad->device=(IOHIDDeviceRef)CFRetain(device);
    pad->element=element;
    pad->manager=manager;
    pad->virtual_profile=virtual_profile;
    CFStringRef name=IOHIDDeviceGetProperty(device,CFSTR(kIOHIDProductKey));
    if(!name||CFGetTypeID(name)!=CFStringGetTypeID()||
       !CFStringGetCString(name,pad->name,sizeof(pad->name),kCFStringEncodingUTF8))
        strcpy(pad->name,"Game Controller");
    fprintf(stderr,"compat32: Saga semantic gamepad registered: %s\n",pad->name);
}
static void saga_unregister_pad(IOHIDDeviceRef device){
    int index=saga_pad_index(device);
    if(index<0)return;
    struct saga_pad *pad=&saga_pads[index];
    fprintf(stderr,"compat32: Saga semantic gamepad removed: %s\n",pad->name);
    CFRelease(pad->element);
    CFRelease(pad->device);
    memset(pad,0,sizeof(*pad));
}
unsigned hid_bridge32_saga_pad_count(void){return saga_pad_count;}
const char *hid_bridge32_saga_pad_name(unsigned index){
    return index<saga_pad_count&&saga_pads[index].device?saga_pads[index].name:NULL;
}
void hid_bridge32_saga_set_semantic_active(unsigned index,int active){
    if(index<saga_pad_count && saga_pads[index].device)
        saga_pads[index].semantic_active=!!active;
}
/* The game recognizes the DS4 v2 profile but leaves its per-device element
 * tree empty on modern macOS. The keys and logical controls mirror the
 * PS4DualshockV2.plist installed in the Saga bundle. */
static const struct {uint32_t key,control;} ds4_v2_elements[]={
    {0x00010030,0},  {0x00010031,1},  {0x00010032,3},
    {0x00010033,2},  {0x00010034,5},  {0x00010035,4},
    {0x00010039,21}, {0x00090001,8},  {0x00090002,6},
    {0x00090003,7},  {0x00090004,9},  {0x00090005,10},
    {0x00090006,11}, {0x00090009,15}, {0x0009000a,14},
    {0x0009000b,12}, {0x0009000c,13},
};
static uint32_t ds4_v2_tree(uint32_t *nodes,unsigned low,unsigned high,
                            uint32_t parent,unsigned depth){
    if(low==high)return 0;
    unsigned middle=low+(high-low)/2;
    uint32_t node=nodes[middle],*fields=(void *)(uintptr_t)node;
    fields[2]=parent;
    fields[3]=depth<4;
    fields[4]=ds4_v2_elements[middle].key;
    fields[5]=ds4_v2_elements[middle].control;
    /* The semantic controller bridge and the DS4 v2 profile use a 0..255
     * source range for these axes. */
    uint32_t page=ds4_v2_elements[middle].key>>16;
    uint32_t usage=ds4_v2_elements[middle].key&0xffff;
    if(page==1 && usage>=0x30 && usage<=0x35)fields[7]=255;
    fields[0]=ds4_v2_tree(nodes,low,middle,node,depth+1);
    fields[1]=ds4_v2_tree(nodes,middle+1,high,node,depth+1);
    return node;
}
static void install_ds4_v2_map(uint32_t record){
    enum {count=sizeof(ds4_v2_elements)/sizeof(ds4_v2_elements[0])};
    uint32_t nodes[count];
    unsigned i=0;
    for(;i<count;++i){
        nodes[i]=compat_runtime32_allocate(32,1);
        if(!nodes[i]){while(i)compat_runtime32_deallocate(nodes[--i]);return;}
    }
    uint32_t root=ds4_v2_tree(nodes,0,count,record+4,0);
    *(uint32_t *)(uintptr_t)(record+4)=root;
    /* Saga stores its FFDeviceObjectReference at +8. The number of mapped
     * controls belongs only to this bridge, not in the guest device record. */
    if(trace_hid())fprintf(stderr,"compat32: HID control map installed record=%08x root=%08x count=%u\n",record,root,count);
}
static unsigned free_ds4_v2_tree(uint32_t node){
    if(!node)return 0;
    uint32_t *fields=(void *)(uintptr_t)node;
    uint32_t left=fields[0],right=fields[1];
    unsigned count=1+free_ds4_v2_tree(left)+free_ds4_v2_tree(right);
    compat_runtime32_deallocate(node);
    return count;
}
/* Feral's HID callback can choose a cookie-based configuration for an
 * already-connected pad. Semantic reports do not have distinct native HID
 * cookies, so apply their logical controls to that pad's guest state after
 * the callback. The game's control ranges are signed 16-bit for sticks,
 * 0..255 for triggers, and 0/1 for buttons. */
static int saga_write_semantic_state(uint32_t context,uint32_t token,
                                     unsigned page,unsigned usage,int value,
                                     uint64_t timestamp,int physical_profile){
    uint32_t key=(page<<16)|usage;
    for(unsigned slot=0;slot<32;++slot){
        uint32_t record=guest_word(context+0x6c+slot*4);
        if(!record || guest_word(record)!=token)continue;
        if(page==1 && usage==0x39){
            uint32_t up=0,down=0,left=0,right=0;
            switch(value){
                case 0: up=1; break;
                case 1: up=right=1; break;
                case 2: right=1; break;
                case 3: right=down=1; break;
                case 4: down=1; break;
                case 5: down=left=1; break;
                case 6: left=1; break;
                case 7: left=up=1; break;
            }
            memcpy((void *)(uintptr_t)(record+0x54),&up,4);
            memcpy((void *)(uintptr_t)(record+0x58),&down,4);
            memcpy((void *)(uintptr_t)(record+0x5c),&left,4);
            memcpy((void *)(uintptr_t)(record+0x60),&right,4);
        }else{
            unsigned control=22;
            for(unsigned i=0;i<sizeof(ds4_v2_elements)/sizeof(ds4_v2_elements[0]);++i)
                if(ds4_v2_elements[i].key==key){control=ds4_v2_elements[i].control;break;}
            if(control>=22)return 0;
            int32_t stored=value;
            if(!physical_profile && page==1 && usage>=0x30 && usage<=0x35 &&
               control!=2 && control!=5)
                stored=value*257-32768;
            memcpy((void *)(uintptr_t)(record+0x10+control*4),&stored,4);
            if(trace_hid() && (usage==0x30 || usage==0x31 || usage==10))
                fprintf(stderr,"compat32: Saga semantic state record=%08x key=%08x control=%u input=%d stored=%d\n",
                    record,key,control,value,stored);
        }
        memcpy((void *)(uintptr_t)(record+0x68),&timestamp,8);
        return 1;
    }
    return 0;
}
int hid_bridge32_run_ds4_map_self_test(void){
    uint32_t record=compat_runtime32_allocate(32,1);
    if(!record)return -1;
    uint32_t *fields=(void *)(uintptr_t)record;
    fields[2]=0x12345678; /* Force Feedback handle, not a map count. */
    install_ds4_v2_map(record);
    uint32_t root=fields[1];
    int ok=root && fields[2]==0x12345678;
    for(unsigned i=0;i<sizeof(ds4_v2_elements)/sizeof(ds4_v2_elements[0]);++i){
        uint32_t key=ds4_v2_elements[i].key,node=root;
        for(unsigned steps=0;node&&steps<32;++steps){
            uint32_t *entry=(void *)(uintptr_t)node;
            if(entry[4]==key){
                if(entry[5]!=ds4_v2_elements[i].control)ok=0;
                if((key>>16)==1 && (key&0xffff)>=0x30 &&
                   (key&0xffff)<=0x35 && entry[7]!=255)ok=0;
                break;
            }
            node=entry[key<entry[4]?0:1];
        }
        if(!node)ok=0;
    }
    unsigned count=free_ds4_v2_tree(root);
    compat_runtime32_deallocate(record);
    ok=ok && count==sizeof(ds4_v2_elements)/sizeof(ds4_v2_elements[0]);
    uint32_t context=compat_runtime32_allocate(0x100,1);
    uint32_t state=compat_runtime32_allocate(0xb0,1);
    if(!context || !state)ok=0;
    else{
        *(uint32_t *)(uintptr_t)(context+0x6c)=state;
        *(uint32_t *)(uintptr_t)state=0x12345678;
        ok=ok && saga_write_semantic_state(context,0x12345678,1,0x30,0,11,0);
        ok=ok && (int32_t)guest_word(state+0x10)==-32768;
        ok=ok && saga_write_semantic_state(context,0x12345678,1,0x30,255,12,0);
        ok=ok && (int32_t)guest_word(state+0x10)==32767;
        ok=ok && saga_write_semantic_state(context,0x12345678,1,0x30,0,12,1);
        ok=ok && guest_word(state+0x10)==0;
        ok=ok && saga_write_semantic_state(context,0x12345678,1,0x30,255,12,1);
        ok=ok && guest_word(state+0x10)==255;
        ok=ok && saga_write_semantic_state(context,0x12345678,9,10,1,13,0);
        ok=ok && guest_word(state+0x48)==1;
        ok=ok && saga_write_semantic_state(context,0x12345678,1,0x39,3,14,0);
        ok=ok && guest_word(state+0x58)==1 && guest_word(state+0x60)==1;
        ok=ok && guest_word(state+0x54)==0 && guest_word(state+0x5c)==0;
        uint64_t stored_time=0;
        memcpy(&stored_time,(const void *)(uintptr_t)(state+0x68),8);
        ok=ok && stored_time==14;
    }
    if(context)compat_runtime32_deallocate(context);
    if(state)compat_runtime32_deallocate(state);
    fprintf(stderr,"compat32: HID DS4 map layout %s\n",ok?"PASS":"FAIL");
    return ok?0:-1;
}
static struct callback *registration(void *manager,unsigned kind,uint32_t fn,uint32_t ctx){
    pthread_mutex_lock(&lock);struct callback *c=callbacks;
    while(c&&(c->manager!=manager||c->kind!=kind))c=c->next;
    if(!c){c=calloc(1,sizeof(*c));if(c){c->manager=manager;c->kind=kind;c->next=callbacks;callbacks=c;}}
    if(c){c->function=fn;c->context=ctx;}pthread_mutex_unlock(&lock);return c;
}
static int saga_needs_semantic_profile(IOHIDDeviceRef device){
    if(lp32_profile()->title!=LP32_TITLE_COMPLETE_SAGA || !saga_gamepad(device))return 0;
    CFNumberRef vendor=IOHIDDeviceGetProperty(device,CFSTR(kIOHIDVendorIDKey));
    CFNumberRef product=IOHIDDeviceGetProperty(device,CFSTR(kIOHIDProductIDKey));
    int vid=0,pid=0;
    if(vendor)CFNumberGetValue(vendor,kCFNumberIntType,&vid);
    if(product)CFNumberGetValue(product,kCFNumberIntType,&pid);
    uint32_t id=((uint32_t)(uint16_t)pid<<16)|(uint16_t)vid;
    pthread_mutex_lock(&lock);
    uint32_t context=0;
    for(struct callback *c=callbacks;c;c=c->next)
        if(c->kind==0 && c->context){context=c->context;break;}
    pthread_mutex_unlock(&lock);
    return context && !saga_has_profile(context,id);
}
static void invoke(void *raw,IOReturn status,void *sender,CFTypeRef value){
    struct callback *c=raw;pthread_mutex_lock(&lock);uint32_t fn=c->function,ctx=c->context;pthread_mutex_unlock(&lock);
    if(c->kind==2 && !synthetic_value){
        IOHIDElementRef element=IOHIDValueGetElement((IOHIDValueRef)value);
        int index=saga_pad_index(IOHIDElementGetDevice(element));
        if(index>=0 && saga_pads[index].semantic_active)return;
    }
    if(trace_hid()){
        if(c->kind==2){
            IOHIDElementRef element=IOHIDValueGetElement((IOHIDValueRef)value);
            if(IOHIDElementGetUsagePage(element)==9)
                fprintf(stderr,"compat32: HID button usage=%u value=%ld guest_callback=%08x status=%x\n",
                    IOHIDElementGetUsage(element),IOHIDValueGetIntegerValue((IOHIDValueRef)value),fn,status);
        }else fprintf(stderr,"compat32: HID device callback kind=%u device=%p guest_callback=%08x status=%x\n",c->kind,value,fn,status);
    }
    if(fn){
        IOHIDDeviceRef pending=NULL;
        if(c->kind==0 && lp32_profile()->title==LP32_TITLE_COMPLETE_SAGA &&
           saga_gamepad((IOHIDDeviceRef)value) && saga_pad_index((IOHIDDeviceRef)value)<0){
            CFNumberRef vendor=IOHIDDeviceGetProperty((IOHIDDeviceRef)value,CFSTR(kIOHIDVendorIDKey));
            CFNumberRef product=IOHIDDeviceGetProperty((IOHIDDeviceRef)value,CFSTR(kIOHIDProductIDKey));
            int vid=0,pid=0;
            if(vendor)CFNumberGetValue(vendor,kCFNumberIntType,&vid);
            if(product)CFNumberGetValue(product,kCFNumberIntType,&pid);
            uint32_t id=((uint32_t)(uint16_t)pid<<16)|(uint16_t)vid;
            if(!saga_has_profile(ctx,id))pending=(IOHIDDeviceRef)value;
        }
        /* Input values are per-event objects, unlike devices/elements.
           Retaining every value forever grows the global proxy table on
           every axis/button report and eventually exhausts it. */
        uint32_t token=c->kind==2?objc_bridge32_guest_owned_object((void *)value):objc_bridge32_guest_object((void *)value);
        uint32_t a[]={ctx,(uint32_t)status,objc_bridge32_guest_object(sender),token};
        matching_saga_device=pending;
        compat_runtime32_call(fn,a,4);
        matching_saga_device=NULL;
        if(trace_hid() && c->kind==2 && synthetic_value &&
           synthetic_page==1 && (synthetic_usage==48 || synthetic_usage==49)){
            uint32_t device=objc_bridge32_guest_object(IOHIDElementGetDevice(synthetic_element));
            for(unsigned index=0;index<32;++index){
                uint32_t record=guest_word(ctx+0x6c+index*4);
                if(!record || guest_word(record)!=device)continue;
                uint32_t key=(synthetic_page<<16)|synthetic_usage;
                uint32_t node=guest_word(record+4);
                for(unsigned steps=0;node&&steps<32;++steps){
                    uint32_t found=guest_word(node+0x10);
                    if(found==key)break;
                    node=guest_word(node+(key<found?0:4));
                }
                uint32_t control=node?guest_word(node+0x14):0xffffffffu;
                uint32_t stored=control<22?guest_word(record+0x10+4*control):0;
                fprintf(stderr,"compat32: Saga axis result record=%08x key=%08x map=%08x control=%u input=%d stored=%d\n",
                    record,key,node,control,synthetic_integer,(int32_t)stored);
                break;
            }
        }
        if(c->kind==1)saga_unregister_pad((IOHIDDeviceRef)value);
        if(c->kind==0){
            const uint8_t *guest=(const void *)(uintptr_t)ctx;
            for(unsigned index=0;index<32;++index){
                uint32_t record=0,id=0,root=0;
                memcpy(&record,guest+0x6c+index*4,4);
                if(!record)continue;
                if(lp32_profile()->title==LP32_TITLE_COMPLETE_SAGA &&
                   saga_gamepad((IOHIDDeviceRef)value) &&
                   guest_word(record)==token)
                    saga_register_pad((IOHIDDeviceRef)value,c->manager,pending!=NULL);
                memcpy(&id,(const void *)(uintptr_t)(record+0xc),4);
                memcpy(&root,(const void *)(uintptr_t)(record+4),4);
                if(id!=0x09cc054c && id!=((uint32_t)kSagaVirtualProduct<<16|kSagaVirtualVendor))continue;
                if(root)continue;
                install_ds4_v2_map(record);
            }
        }
        if(trace_hid() && (c->kind!=2 || (IOHIDElementGetUsagePage(IOHIDValueGetElement((IOHIDValueRef)value))==9))){
            const uint8_t *guest=(const void *)(uintptr_t)ctx;
            unsigned linked=0,active=0;
            uint32_t first_record=0;
            for(unsigned index=0;index<32;++index){
                uint32_t record;
                memcpy(&record,guest+0x6c+index*4,sizeof(record));
                if(!record)continue;
                if(!first_record)first_record=record;
                ++linked;
                if(*(const uint8_t *)(uintptr_t)(record+0x70))++active;
            }
            fprintf(stderr,"compat32: HID guest callback kind=%u context=%08x device=%08x linked=%u active=%u disabled=%u\n",
                c->kind,ctx,a[3],linked,active,*(const volatile uint8_t *)(uintptr_t)0x02bb4bf0);
            if(c->kind==0){
                uint32_t map_root=0,map_count=0;
                memcpy(&map_root,guest+0x4c,4);
                memcpy(&map_count,guest+0x50,4);
                uint32_t wanted=0;
                if(first_record)memcpy(&wanted,(const void *)(uintptr_t)(first_record+0xc),4);
                uint32_t node=map_root;unsigned steps=0;
                while(node && steps++<64){
                    uint32_t key=0,next=0;
                    memcpy(&key,(const void *)(uintptr_t)(node+0x10),4);
                    if(key==wanted)break;
                    memcpy(&next,(const void *)(uintptr_t)(node+(wanted<key?0:4)),4);
                    node=next;
                }
                uint32_t mapped_value=0,mapped_end=0,record_map=0,record_begin=0,record_end=0;
                if(node){
                    memcpy(&mapped_value,(const void *)(uintptr_t)(node+0x14),4);
                    memcpy(&mapped_end,(const void *)(uintptr_t)(node+0x18),4);
                }
                if(first_record){
                    memcpy(&record_map,(const void *)(uintptr_t)(first_record+4),4);
                    memcpy(&record_begin,(const void *)(uintptr_t)(first_record+0x14),4);
                    memcpy(&record_end,(const void *)(uintptr_t)(first_record+0x18),4);
                }
                fprintf(stderr,"compat32: HID profile map root=%08x count=%u wanted=%08x match=%08x value=%08x end=%08x steps=%u record_map=%08x begin=%08x end=%08x\n",map_root,map_count,wanted,node,mapped_value,mapped_end,steps,record_map,record_begin,record_end);
                uint32_t swapped=(wanted<<16)|(wanted>>16),other=map_root;
                for(unsigned i=0;other&&i<64;++i){
                    uint32_t key=0,next=0;
                    memcpy(&key,(const void *)(uintptr_t)(other+0x10),4);
                    if(key==swapped)break;
                    memcpy(&next,(const void *)(uintptr_t)(other+(swapped<key?0:4)),4);
                    other=next;
                }
                uint32_t other_begin=0,other_end=0;
                if(other){memcpy(&other_begin,(const void *)(uintptr_t)(other+0x14),4);memcpy(&other_end,(const void *)(uintptr_t)(other+0x18),4);}
                fprintf(stderr,"compat32: HID swapped key=%08x match=%08x begin=%08x end=%08x\n",swapped,other,other_begin,other_end);
                uint32_t original=0x05c4054c,old=map_root;
                for(unsigned i=0;old&&i<64;++i){uint32_t key=0,next=0;memcpy(&key,(const void *)(uintptr_t)(old+0x10),4);if(key==original)break;memcpy(&next,(const void *)(uintptr_t)(old+(original<key?0:4)),4);old=next;}
                uint32_t old_begin=0,old_end=0;if(old){memcpy(&old_begin,(const void *)(uintptr_t)(old+0x14),4);memcpy(&old_end,(const void *)(uintptr_t)(old+0x18),4);}
                fprintf(stderr,"compat32: HID original key=%08x match=%08x begin=%08x end=%08x\n",original,old,old_begin,old_end);
                if(first_record){uint32_t counts[4]={0};memcpy(&counts[0],(const void *)(uintptr_t)(first_record+8),4);memcpy(&counts[1],(const void *)(uintptr_t)(first_record+0x80),4);memcpy(&counts[2],(const void *)(uintptr_t)(first_record+0x94),4);memcpy(&counts[3],(const void *)(uintptr_t)(first_record+0xa4),4);fprintf(stderr,"compat32: HID record map counts=%u,%u,%u,%u\n",counts[0],counts[1],counts[2],counts[3]);}
            }
            if(c->kind==2 && first_record){
                IOHIDElementRef element=IOHIDValueGetElement((IOHIDValueRef)value);
                uint32_t record_device,record_id;
                memcpy(&record_device,(const void *)(uintptr_t)first_record,sizeof(record_device));
                memcpy(&record_id,(const void *)(uintptr_t)(first_record+0xc),sizeof(record_id));
                uint32_t element_device=objc_bridge32_guest_object(IOHIDElementGetDevice(element));
                uint32_t key=(IOHIDElementGetUsage(element)<<16)|IOHIDElementGetUsagePage(element);
                uint32_t node=0;
                memcpy(&node,(const void *)(uintptr_t)(first_record+4),sizeof(node));
                unsigned steps=0;
                while(node && steps++<64){
                    uint32_t found_key,next;
                    memcpy(&found_key,(const void *)(uintptr_t)(node+0x10),sizeof(found_key));
                    if(found_key==key)break;
                    memcpy(&next,(const void *)(uintptr_t)(node+(key<found_key?0:4)),sizeof(next));
                    node=next;
                }
                fprintf(stderr,"compat32: HID guest mapping record=%08x record_device=%08x element_device=%08x id=%08x key=%08x node=%08x steps=%u\n",
                    first_record,record_device,element_device,record_id,key,node,steps);
                uint32_t roots[6]={0};
                memcpy(&roots[0],(const void *)(uintptr_t)(first_record+0x74),4);
                memcpy(&roots[1],(const void *)(uintptr_t)(first_record+0x88),4);
                memcpy(&roots[2],(const void *)(uintptr_t)(first_record+0x9c),4);
                memcpy(&roots[3],(const void *)(uintptr_t)(ctx+0x128),4);
                memcpy(&roots[4],(const void *)(uintptr_t)(ctx+0x12c),4);
                memcpy(&roots[5],(const void *)(uintptr_t)(ctx+0x130),4);
                fprintf(stderr,"compat32: HID roots record74=%08x record88=%08x record9c=%08x context128=%08x context12c=%08x context130=%08x\n",
                    roots[0],roots[1],roots[2],roots[3],roots[4],roots[5]);
            }
        }
        if(c->kind==2){uint64_t ignored;objc_bridge32_dispatch("_CFRelease",&token,&ignored);}
    }
}
static void device_callback(void *raw,IOReturn status,void *sender,IOHIDDeviceRef device){invoke(raw,status,sender,device);}
static void value_callback(void *raw,IOReturn status,void *sender,IOHIDValueRef value){invoke(raw,status,sender,value);}
int hid_bridge32_saga_emit(unsigned index,unsigned page,unsigned usage,int value){
    if(index>=saga_pad_count || !saga_pads[index].device || synthetic_value)return 0;
    struct saga_pad *pad=&saga_pads[index];
    struct callback *input=NULL;
    pthread_mutex_lock(&lock);
    for(struct callback *c=callbacks;c;c=c->next){
        if(c->manager==pad->manager && c->kind==2 && c->function){input=c;break;}
    }
    pthread_mutex_unlock(&lock);
    if(!input)return 0;
    IOHIDValueRef report=IOHIDValueCreateWithIntegerValue(NULL,pad->element,
                                                           mach_absolute_time(),value);
    if(!report)return 0;
    synthetic_value=report;
    synthetic_element=pad->element;
    synthetic_page=page;
    synthetic_usage=usage;
    synthetic_integer=value;
    if(trace_hid() && page==9 && usage==10 && value){
        uint32_t token=objc_bridge32_guest_object(pad->device);
        for(unsigned i=0;i<32;++i){
            uint32_t record=guest_word(input->context+0x6c+i*4);
            if(!record)continue;
            fprintf(stderr,"compat32: Saga Menu record slot=%u record=%08x device=%08x target=%08x map=%08x active=%u\n",
                i,record,guest_word(record),token,guest_word(record+4),
                *(const uint8_t *)(uintptr_t)(record+0x70));
        }
    }
    if(trace_hid())fprintf(stderr,
        "compat32: Saga semantic event pad=%u page=%u usage=%u value=%d\n",
        index,page,usage,value);
    invoke(input,0,pad->manager,report);
    /* The game may select a cookie-based mapping for an existing device.
     * Write the logical state as well; publisher profiles use byte axes. */
    if(!saga_write_semantic_state(input->context,
                                  objc_bridge32_guest_object(pad->device),page,usage,value,
                                  IOHIDValueGetTimeStamp(report),!pad->virtual_profile) && trace_hid())
        fprintf(stderr,"compat32: Saga semantic state has no guest record for %s\n",pad->name);
    synthetic_value=NULL;
    synthetic_element=NULL;
    CFRelease(report);
    return 1;
}
/* Test harness only: use a real enumerated controller element, but deliver
 * its value to this process's registered callback without posting OS input. */
static int test_element(unsigned page,unsigned usage,int value,const char *product_name){
    if(!getenv("LP32_TEST_HID_BUTTONS"))return 0;
    pthread_mutex_lock(&lock);struct callback *head=callbacks;pthread_mutex_unlock(&lock);
    for(struct callback *c=head;c;c=c->next){
        pthread_mutex_lock(&lock);uint32_t function=c->function;pthread_mutex_unlock(&lock);
        if(c->kind!=2 || !function)continue;
        CFSetRef devices=IOHIDManagerCopyDevices(c->manager);if(!devices)continue;
        CFIndex count=CFSetGetCount(devices);const void **list=calloc(count?count:1,sizeof(void*));
        if(!list){CFRelease(devices);return 0;}CFSetGetValues(devices,list);
        int delivered=0;
        for(CFIndex i=0;i<count&&!delivered;++i){
            IOHIDDeviceRef device=(IOHIDDeviceRef)list[i];
            CFNumberRef device_page=IOHIDDeviceGetProperty(device,CFSTR(kIOHIDPrimaryUsagePageKey));
            CFNumberRef use=IOHIDDeviceGetProperty(device,CFSTR(kIOHIDPrimaryUsageKey));
            int p=0,u=0;if(device_page)CFNumberGetValue(device_page,kCFNumberIntType,&p);if(use)CFNumberGetValue(use,kCFNumberIntType,&u);
            if(p!=1 || (u!=4&&u!=5))continue;
            if(product_name){
                CFStringRef product=IOHIDDeviceGetProperty(device,CFSTR(kIOHIDProductKey));
                char name[128]="";
                if(!product || CFGetTypeID(product)!=CFStringGetTypeID() ||
                   !CFStringGetCString(product,name,sizeof(name),kCFStringEncodingUTF8) ||
                   !strstr(name,product_name))continue;
            }
            CFArrayRef elements=IOHIDDeviceCopyMatchingElements(device,NULL,0);if(!elements)continue;
            for(CFIndex j=0;j<CFArrayGetCount(elements);++j){
                IOHIDElementRef element=(IOHIDElementRef)CFArrayGetValueAtIndex(elements,j);
                if(IOHIDElementGetUsagePage(element)!=page || IOHIDElementGetUsage(element)!=usage)continue;
                IOHIDValueRef hid_value=IOHIDValueCreateWithIntegerValue(NULL,element,mach_absolute_time(),value);
                if(hid_value){value_callback(c,0,c->manager,hid_value);CFRelease(hid_value);delivered=1;}break;
            }
            CFRelease(elements);
        }
        free(list);CFRelease(devices);if(delivered)return 1;
    }
    return 0;
}
int hid_bridge32_test_button(unsigned usage,int down){return test_element(9,usage,!!down,NULL);}
int hid_bridge32_test_named_button(const char *name,unsigned usage,int down){return test_element(9,usage,!!down,name);}
int hid_bridge32_test_hat(unsigned value){return test_element(1,57,value,NULL);}
int hid_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out){
    if (!strcmp(name, "_FFIsForceFeedback")) {
        *out = (uint32_t)FFIsForceFeedback(a[0]);
        return 1;
    }
    if(strncmp(name,"_IOHID",6))return 0;
#define IS(n) (!strcmp(name,n))
#define O(i) objc_bridge32_host_object(a[i])
    if(IS("_IOHIDManagerCreate")){IOHIDManagerRef manager=IOHIDManagerCreate(NULL,a[1]);*out=objc_bridge32_guest_object(manager);if(trace_hid())fprintf(stderr,"compat32: HID manager create native=%p guest=%08llx\n",manager,(unsigned long long)*out);if(manager)CFRelease(manager);}
    else if(IS("_IOHIDManagerOpen")){*out=(uint32_t)IOHIDManagerOpen(O(0),a[1]);if(trace_hid())fprintf(stderr,"compat32: HID manager open native=%p result=%08llx\n",O(0),(unsigned long long)*out);}
    else if(IS("_IOHIDManagerClose"))*out=(uint32_t)IOHIDManagerClose(O(0),a[1]);
    else if(IS("_IOHIDManagerCopyDevices")){CFSetRef devices=IOHIDManagerCopyDevices(O(0));*out=objc_bridge32_guest_object((void *)devices);if(trace_hid()){
        uint32_t profile_count=0;
        for(struct callback *c=callbacks;c;c=c->next)if(c->manager==O(0)&&c->kind==0&&c->context){memcpy(&profile_count,(const void *)(uintptr_t)(c->context+0x50),4);break;}
        fprintf(stderr,"compat32: HID manager copy devices native=%p count=%ld profiles=%u\n",O(0),devices?CFSetGetCount(devices):0L,profile_count);
    }if(devices)CFRelease(devices);}
    else if(IS("_IOHIDManagerSetDeviceMatching")){if(trace_hid())fprintf(stderr,"compat32: HID manager matching native=%p criteria=%s\n",O(0),O(1)?"dictionary":"all");IOHIDManagerSetDeviceMatching(O(0),O(1));*out=0;}
    else if(IS("_IOHIDManagerSetDeviceMatchingMultiple")){if(trace_hid())fprintf(stderr,"compat32: HID manager multiple matching native=%p count=%ld\n",O(0),O(1)?CFArrayGetCount(O(1)):0L);IOHIDManagerSetDeviceMatchingMultiple(O(0),O(1));*out=0;}
    else if(IS("_IOHIDManagerScheduleWithRunLoop")){if(trace_hid())fprintf(stderr,"compat32: HID manager schedule native=%p loop=%p mode=%p\n",O(0),O(1),O(2));IOHIDManagerScheduleWithRunLoop(O(0),O(1),O(2));*out=0;}
    else if(IS("_IOHIDManagerUnscheduleFromRunLoop")){IOHIDManagerUnscheduleFromRunLoop(O(0),O(1),O(2));*out=0;}
    else if(IS("_IOHIDDeviceUnscheduleFromRunLoop")){IOHIDDeviceUnscheduleFromRunLoop(O(0),O(1),O(2));*out=0;}
    else if(IS("_IOHIDManagerRegisterDeviceMatchingCallback")||IS("_IOHIDManagerRegisterDeviceRemovalCallback")||IS("_IOHIDManagerRegisterInputValueCallback")){
        unsigned kind=IS("_IOHIDManagerRegisterDeviceMatchingCallback")?0:IS("_IOHIDManagerRegisterDeviceRemovalCallback")?1:2;
        struct callback *c=registration(O(0),kind,a[1],a[2]);if(!c)return 0;
        if(trace_hid())fprintf(stderr,"compat32: HID register callback kind=%u native=%p guest=%08x\n",kind,O(0),a[1]);
        if(kind==0)IOHIDManagerRegisterDeviceMatchingCallback(O(0),a[1]?device_callback:NULL,c);
        else if(kind==1)IOHIDManagerRegisterDeviceRemovalCallback(O(0),a[1]?device_callback:NULL,c);
        else IOHIDManagerRegisterInputValueCallback(O(0),a[1]?value_callback:NULL,c);*out=0;
    }
    else if(IS("_IOHIDDeviceGetProperty")){
        CFTypeRef property=IOHIDDeviceGetProperty(O(0),O(1));
        int pad_index=saga_pad_index(O(0));
        if(matching_saga_device==O(0)||
           (pad_index>=0 && saga_pads[pad_index].virtual_profile)||
           saga_needs_semantic_profile(O(0))){
            static CFNumberRef canonical_vendor,canonical_product;
            if(!canonical_vendor){
                int vendor=kSagaVirtualVendor,product=kSagaVirtualProduct;
                canonical_vendor=CFNumberCreate(NULL,kCFNumberIntType,&vendor);
                canonical_product=CFNumberCreate(NULL,kCFNumberIntType,&product);
            }
            if(O(1) && CFEqual(O(1),CFSTR(kIOHIDVendorIDKey)))property=canonical_vendor;
            else if(O(1) && CFEqual(O(1),CFSTR(kIOHIDProductIDKey)))property=canonical_product;
        }
        *out=objc_bridge32_guest_object((void *)property);
        if(trace_hid()){
            char key[128]="?";
            if(O(1) && CFGetTypeID(O(1))==CFStringGetTypeID())CFStringGetCString(O(1),key,sizeof(key),kCFStringEncodingUTF8);
            fprintf(stderr,"compat32: HID device property %s -> %p type=%lu\n",key,property,property?CFGetTypeID(property):0L);
        }
    }
    else if(IS("_IOHIDDeviceSetReport"))*out=(uint32_t)IOHIDDeviceSetReport(O(0),a[1],a[2],(void *)(uintptr_t)a[3],(int32_t)a[4]);
    else if(IS("_IOHIDElementGetCookie"))*out=IOHIDElementGetCookie(O(0));
    else if(IS("_IOHIDElementGetUsage")){
        *out=synthetic_value&&O(0)==synthetic_element?synthetic_usage:IOHIDElementGetUsage(O(0));
        if(trace_hid()&&synthetic_value&&synthetic_usage==10)
            fprintf(stderr,"compat32: Saga guest element usage=%llu canonical=%u matched=%d\n",
                (unsigned long long)*out,synthetic_usage,O(0)==synthetic_element);
    }
    else if(IS("_IOHIDElementGetUsagePage"))*out=synthetic_value&&O(0)==synthetic_element?synthetic_page:IOHIDElementGetUsagePage(O(0));
    else if(IS("_IOHIDElementGetDevice"))*out=objc_bridge32_guest_object(IOHIDElementGetDevice(O(0)));
    else if(IS("_IOHIDValueGetElement"))*out=objc_bridge32_guest_object(IOHIDValueGetElement(O(0)));
    else if(IS("_IOHIDValueGetIntegerValue")){
        *out=(uint32_t)(synthetic_value&&O(0)==synthetic_value?synthetic_integer:IOHIDValueGetIntegerValue(O(0)));
        if(trace_hid()&&synthetic_value&&synthetic_usage==10)
            fprintf(stderr,"compat32: Saga guest Menu value=%u matched=%d\n",
                    (unsigned)*out,O(0)==synthetic_value);
    }
    else if(IS("_IOHIDValueGetTimeStamp"))*out=IOHIDValueGetTimeStamp(O(0));
    else return 0;
    return 1;
#undef IS
#undef O
}
