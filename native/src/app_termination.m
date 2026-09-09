#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#include "app_termination.h"
#include "compat_runtime.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * Quit from the Dock menu (or any other quit Apple event) reaches AppKit's own
 * -terminate:, not the guest's, so the objc_msgSend hook that ends a guest
 * quit with _Exit never sees it.  AppKit then runs the normal termination
 * teardown, which can wait forever on the mixed-ABI audio graph (see the exit
 * handling in compat_runtime.c), leaving a process that only Force Quit ends.
 * Leave the same way the guest's quit does, from the notification AppKit
 * posts right before exit().
 */
@interface LP32QuitEventHandler : NSObject
- (void)handleQuit:(NSAppleEventDescriptor *)event
    withReplyEvent:(NSAppleEventDescriptor *)reply;
@end

@implementation LP32QuitEventHandler
- (void)handleQuit:(NSAppleEventDescriptor *)event
    withReplyEvent:(NSAppleEventDescriptor *)reply
{
    (void)event;
    (void)reply;
    fprintf(stderr, "compat32: quit Apple event received; exiting\n");
    compat_runtime32_heap_report("quit-apple-event");
    fflush(NULL);
    _Exit(EXIT_SUCCESS);
}
@end

void app_termination_install(void)
{
    static bool installed;
    if (installed) return;
    installed = true;
    /* The guest calls finishLaunching but never runs NSApplication's own
       loop, and AppKit's quit handler did not fire for a Dock quit under the
       bridge; register one directly with the Apple event manager. */
    static LP32QuitEventHandler *quit_handler;
    quit_handler = [[LP32QuitEventHandler alloc] init];
    [[NSAppleEventManager sharedAppleEventManager]
        setEventHandler:quit_handler
            andSelector:@selector(handleQuit:withReplyEvent:)
          forEventClass:kCoreEventClass
             andEventID:kAEQuitApplication];
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationWillTerminateNotification
                    object:NSApp
                     queue:nil
                usingBlock:^(NSNotification *note) {
        (void)note;
        fprintf(stderr, "compat32: AppKit termination requested (Dock quit "
                "or quit Apple event); exiting\n");
        compat_runtime32_heap_report("AppKit-will-terminate");
        fflush(NULL);
        _Exit(EXIT_SUCCESS);
    }];
}

void app_termination_pump(void) {
    app_termination_install();
    /* Cmd+Q in the app switcher and Dock Quit send Apple events, not key
       events. TFU never enters NSApplication.run, which normally drains
       this queue. Service it even while the game is in the background. */
    EventTypeSpec apple_kind = {kEventClassAppleEvent, kEventAppleEvent};
    EventRef apple_event = NULL;
    for (unsigned i = 0; i < 8 && ReceiveNextEvent(1, &apple_kind,
            kEventDurationNoWait, true, &apple_event) == noErr; ++i) {
        OSStatus status = AEProcessEvent(apple_event);
        if (getenv("LP32_TRACE_CARBON_EVENTS"))
            fprintf(stderr, "Carbon processed queued Apple event status=%d\n", (int)status);
        ReleaseEvent(apple_event);
    }
}
