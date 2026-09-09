#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>
#include "tfu_controller.h"
#include "compat_runtime.h"
#include "game_profile.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* TFU 1.2 includes Aspyr's own Xbox 360/XInput adapter. Bridge its three
   cdecl entry points to GameController, preserving the game's Xbox controls,
   analog triggers, menus, and button prompts. No legacy IOKit driver or
   synthetic desktop keyboard/mouse events are involved. */
struct pad_sample { float lx, ly, rx, ry, lt, rt; uint32_t buttons; int pov; };
struct xinput_gamepad { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };
struct xinput_state { uint32_t packet; struct xinput_gamepad pad; };
struct xinput_caps { uint8_t type, subtype; uint16_t flags; struct xinput_gamepad pad; uint16_t motors[2]; };
_Static_assert(sizeof(struct xinput_state)==16, "i386 XINPUT_STATE ABI");
_Static_assert(sizeof(struct xinput_caps)==20, "i386 XINPUT_CAPABILITIES ABI");
enum { kDisconnected=1167, kInvalidArgument=87 };
static bool installed, self_testing, test_connected;
static struct pad_sample test_sample;
static pid_t test_foreground_pid;
static GCController *controller;
static double last_refresh;
static uint64_t state_reads;
static struct xinput_state previous_state;

static void *pointer(uint32_t value) { return (void *)(uintptr_t)value; }
static bool virtual_input(void) {
    const char *path = getenv("LP32_TFU_TEST_PAD_FILE");
    return path && (getenv("LP32_BACKGROUND_TEST") ||
        (getenv("LP32_TFU_TEST_FOREGROUND_INPUT") && access(path, F_OK) == 0));
}
static bool sample(struct pad_sample *out) {
    struct pad_sample s={.pov=-1}; *out=s;
    if(self_testing && !test_connected) return false;
    if(!self_testing && virtual_input()) {
        FILE *file=fopen(getenv("LP32_TFU_TEST_PAD_FILE"),"r");
        if(file) {
            int read=fscanf(file,"%f %f %f %f %f %f %u %d",&s.lx,&s.ly,&s.rx,&s.ry,&s.lt,&s.rt,&s.buttons,&s.pov);
            fclose(file);
            if(read==8) *out=s;
            else if(read==EOF) return false; /* Empty file simulates unplug. */
        }
        return true;
    }
    double now=[[NSProcessInfo processInfo] systemUptime];
    if(!self_testing && now-last_refresh>.25) {
        last_refresh=now;
        NSArray *connected=[GCController controllers];
        if(controller && ![connected containsObject:controller]) {
            fprintf(stderr,"compat32: TFU controller disconnected\n");[controller release];controller=nil;
        }
        if(!controller) for(GCController *candidate in connected) if(candidate.extendedGamepad) {
            controller=[candidate retain];
            fprintf(stderr,"compat32: TFU controller connected: %s\n",[[controller vendorName] UTF8String]);break;
        }
    }
    if(!self_testing && !controller) return false;
    /* The game pumps Carbon events, not NSApplication's event loop. Its
       window can own foreground input while NSApp.isActive is still NO.
       Ask the workspace which process is actually in front; an inactive
       Cocoa state must not suppress a foreground Carbon game's controls. */
    pid_t foreground=self_testing?test_foreground_pid:
        [NSWorkspace sharedWorkspace].frontmostApplication.processIdentifier;
    bool active=foreground==getpid();
    if(getenv("LP32_TRACE_TFU_CONTROLLER")) {
        static int last_focus=-1;
        int focus=(active?1:0)|([NSApp isActive]?2:0);
        if(focus!=last_focus) {
            fprintf(stderr,"compat32: TFU controller focus foreground=%d cocoa-active=%d foreground-pid=%d self=%d\n",
                active,!!(focus&2),foreground,getpid());
            last_focus=focus;
        }
    }
    /* Keep the connection while inactive, but release all controls. */
    if(!active || (!self_testing && getenv("LP32_BACKGROUND_TEST"))) return true;
    if(self_testing) {*out=test_sample;return true;}
    GCExtendedGamepad *g=[controller.extendedGamepad capture];
    s.lx=g.leftThumbstick.xAxis.value;s.ly=g.leftThumbstick.yAxis.value;
    s.rx=g.rightThumbstick.xAxis.value;s.ry=g.rightThumbstick.yAxis.value;
    s.lt=g.leftTrigger.value;s.rt=g.rightTrigger.value;
    GCControllerButtonInput *buttons[]={g.buttonA,g.buttonB,g.buttonX,g.buttonY,
        g.leftShoulder,g.rightShoulder,g.buttonOptions,g.buttonMenu,g.leftThumbstickButton,g.rightThumbstickButton,
        g.dpad.up,g.dpad.down,g.dpad.left,g.dpad.right};
    const uint16_t masks[]={0x1000,0x2000,0x4000,0x8000,0x100,0x200,0x20,0x10,0x40,0x80,1,2,4,8};
    for(unsigned i=0;i<sizeof(masks)/sizeof(masks[0]);++i) if(buttons[i].pressed)s.buttons|=masks[i];
    *out=s;return true;
}
static float finite_clamp(float value,float minimum,float maximum) {
    return isfinite(value)?fmaxf(minimum,fminf(maximum,value)):0;
}
static int16_t stick(float value) {
    value=finite_clamp(value,-1,1);return (int16_t)lroundf(value*(value<0?32768:32767));
}
static struct xinput_gamepad translate(struct pad_sample s) {
    struct xinput_gamepad p={.buttons=s.buttons&0xf3ff,
        .lt=lroundf(finite_clamp(s.lt,0,1)*255),.rt=lroundf(finite_clamp(s.rt,0,1)*255),
        .lx=stick(s.lx),.ly=stick(s.ly),.rx=stick(s.rx),.ry=stick(s.ry)};
    if(s.pov>=0 && s.pov<36000) {const uint16_t pov[]={1,9,8,10,2,6,4,5};p.buttons|=pov[((s.pov+2250)/4500)%8];}
    return p;
}
int tfu_controller32_dispatch(const char *name,const uint32_t *a,uint64_t *result) {
    if(!installed || strncmp(name,"_lp32_tfu_",10))return 0;
    bool state=!strcmp(name,"_lp32_tfu_xinput_state");
    bool caps=!strcmp(name,"_lp32_tfu_xinput_caps");
    bool vibration=!strcmp(name,"_lp32_tfu_xinput_vibration");
    if(!state && !caps && !vibration)return 0;
    @autoreleasepool {
        void *output=pointer(caps?a[2]:a[1]);
        *result=kInvalidArgument;if(!output)return 1;
        if(state)memset(output,0,sizeof(struct xinput_state));
        if(caps)memset(output,0,sizeof(struct xinput_caps));
        struct pad_sample s;
        *result=kDisconnected;if(a[0]!=0 || !sample(&s))return 1;
        *result=0;
        if(state) {
            struct xinput_gamepad pad=translate(s);
            if(memcmp(&pad,&previous_state.pad,sizeof(pad)))++previous_state.packet;
            previous_state.pad=pad;memcpy(output,&previous_state,sizeof(previous_state));
            if(++state_reads==1)fprintf(stderr,"compat32: TFU Xbox gamepad state polling active\n");
            if(getenv("LP32_TRACE_TFU_CONTROLLER")) {
                static struct xinput_gamepad logged;
                if(memcmp(&logged,&pad,sizeof(pad))) {
                    fprintf(stderr,"compat32: TFU pad buttons=%04x sticks=%d,%d,%d,%d triggers=%u,%u\n",
                        pad.buttons,pad.lx,pad.ly,pad.rx,pad.ry,pad.lt,pad.rt);logged=pad;
                }
            }
        } else if(caps) {
            struct xinput_caps *c=output;c->type=1;c->subtype=1;
            c->pad=(struct xinput_gamepad){.buttons=0xf3ff,.lt=255,.rt=255,.lx=32767,.ly=32767,.rx=32767,.ry=32767};
            /* No force-feedback capability: haptics are not implemented. */
        }
        return 1;
    }
}
int tfu_controller32_install(void) {
    if(lp32_profile()->title!=LP32_TITLE_TFU || installed)return 0;
    struct hook {uint32_t address;uint8_t expected[6];unsigned length;const char *name;uint32_t thunk;} hooks[]={
        {lp32_profile()->tfu->xinput_state,{0x55,0x89,0xe5,0x83,0xec,0x28},6,"_lp32_tfu_xinput_state",0},
        {lp32_profile()->tfu->xinput_caps,{0x55,0x89,0xe5,0x56,0x53},5,"_lp32_tfu_xinput_caps",0},
        {lp32_profile()->tfu->xinput_vibration,{0x55,0x89,0xe5,0x56,0x53},5,"_lp32_tfu_xinput_vibration",0},
    };
    for(unsigned i=0;i<3;++i) {
        if(memcmp(pointer(hooks[i].address),hooks[i].expected,hooks[i].length)) {
            fprintf(stderr,"compat32: TFU Xbox adapter signature mismatch at %08x\n",hooks[i].address);return -1;
        }
        hooks[i].thunk=compat_runtime32_guest_callback(hooks[i].name);if(!hooks[i].thunk)return -1;
    }
    uintptr_t page_size=(uintptr_t)getpagesize();
    for(unsigned i=0;i<3;++i) {
        uintptr_t page=hooks[i].address&~(page_size-1);
        size_t span=((hooks[i].address+hooks[i].length+page_size-1)&~(page_size-1))-page;
        if(mprotect((void *)page,span,PROT_READ|PROT_WRITE|PROT_EXEC))return -1;
        uint8_t patch[6]={0xe9,0,0,0,0,0x90};
        int32_t offset=(int32_t)(hooks[i].thunk-hooks[i].address-5);memcpy(patch+1,&offset,4);
        memcpy(pointer(hooks[i].address),patch,hooks[i].length);
        __builtin___clear_cache(pointer(hooks[i].address),(char *)pointer(hooks[i].address)+hooks[i].length);
        if(mprotect((void *)page,span,PROT_READ|PROT_EXEC))return -1;
    }
    installed=true;
    /* Carbon can own foreground input without updating Cocoa's active state.
       GameController's own foreground filter can then suppress physical
       events before sample() sees them. Receive the hardware state and let
       sample() enforce the actual workspace foreground PID, including neutral
       packets on focus loss. Virtual-input tests bypass this framework path. */
    if(@available(macOS 11.3,*))[GCController setShouldMonitorBackgroundEvents:YES];
    fprintf(stderr,"compat32: TFU GameController bridge installed (Xbox 360 adapter)\n");return 0;
}
int tfu_controller32_self_test(void) {
    if(!installed)return -1;
    uint32_t memory=compat_runtime32_allocate(64,1);if(!memory)return -1;
    int status=-1;self_testing=true;test_connected=true;test_foreground_pid=getpid();
    test_sample=(struct pad_sample){.lx=1,.ly=-1,.rx=-.5,.ry=.5,.lt=.25,.rt=1,.buttons=0x1010,.pov=4500};
    uint32_t result=compat_runtime32_call(lp32_profile()->tfu->xinput_state,(uint32_t[]){0,memory},2);
    struct xinput_state first=*(struct xinput_state *)pointer(memory);
    if(result || compat_runtime32_last_call_trapped() || first.pad.lx!=32767 || first.pad.ly!=-32768 ||
       first.pad.rx!=-16384 || first.pad.ry!=16384 || first.pad.lt!=64 || first.pad.rt!=255 || first.pad.buttons!=0x1019)goto done;
    if(compat_runtime32_call(lp32_profile()->tfu->xinput_state,(uint32_t[]){0,memory},2) || ((struct xinput_state *)pointer(memory))->packet!=first.packet)goto done;
    /* Leave every physical control held across a focus loss. The guest must
       receive a neutral packet while another app owns input, then the held
       state again on return. This also runs with Cocoa inactive headlessly. */
    test_foreground_pid=0;
    if(compat_runtime32_call(lp32_profile()->tfu->xinput_state,(uint32_t[]){0,memory},2))goto done;
    struct xinput_state inactive=*(struct xinput_state *)pointer(memory);
    struct xinput_gamepad zero={0};
    if(memcmp(&inactive.pad,&zero,sizeof(zero)) || inactive.packet==first.packet)goto done;
    if(compat_runtime32_call(lp32_profile()->tfu->xinput_caps,(uint32_t[]){0,0,memory},3))goto done;
    test_foreground_pid=getpid();
    if(compat_runtime32_call(lp32_profile()->tfu->xinput_state,(uint32_t[]){0,memory},2))goto done;
    struct xinput_state reactivated=*(struct xinput_state *)pointer(memory);
    if(memcmp(&reactivated.pad,&first.pad,sizeof(first.pad)) || reactivated.packet==inactive.packet)goto done;
    test_sample=(struct pad_sample){.pov=-1};
    if(compat_runtime32_call(lp32_profile()->tfu->xinput_state,(uint32_t[]){0,memory},2))goto done;
    struct xinput_state neutral=*(struct xinput_state *)pointer(memory);
    if(memcmp(&neutral.pad,&zero,sizeof(zero)) || neutral.packet==first.packet)goto done;
    if(compat_runtime32_call(lp32_profile()->tfu->xinput_caps,(uint32_t[]){0,0,memory},3) || ((struct xinput_caps *)pointer(memory))->subtype!=1 ||
       ((struct xinput_caps *)pointer(memory))->flags!=0)goto done;
    if(compat_runtime32_call(lp32_profile()->tfu->xinput_vibration,(uint32_t[]){0,memory},2))goto done;
    test_connected=false;
    if(compat_runtime32_call(lp32_profile()->tfu->xinput_state,(uint32_t[]){0,memory},2)!=kDisconnected ||
       memcmp(&((struct xinput_state *)pointer(memory))->pad,&zero,sizeof(zero)))goto done;
    test_connected=true;
    if(compat_runtime32_call(lp32_profile()->tfu->xinput_state,(uint32_t[]){1,memory},2)!=kDisconnected ||
       compat_runtime32_call(lp32_profile()->tfu->xinput_state,(uint32_t[]){0,0},2)!=kInvalidArgument)goto done;
    status=0;
 done:
    self_testing=false;compat_runtime32_deallocate(memory);
    fprintf(stderr,"TFU controller self-test: %s (guest ABI, axes, buttons, triggers, D-pad, packets, focus loss/return, disconnect)\n",status?"FAIL":"PASS");return status;
}
