#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#include "app_termination.h"
#include <stdio.h>
#include <unistd.h>

void compat_runtime32_heap_report(const char *reason) {
    fprintf(stderr, "PASS: queued quit reached termination handler (%s)\n", reason);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        app_termination_install();
        /* Exercise the actual macOS event queue in this test process only.
           No keyboard injection, other application, or game save is used. */
        pid_t pid = getpid();
        AEAddressDesc target = {0};
        AppleEvent event = {0};
        OSStatus status = AECreateDesc(typeKernelProcessID, &pid, sizeof(pid), &target);
        if (!status) status = AECreateAppleEvent(kCoreEventClass, kAEQuitApplication,
            &target, kAutoGenerateReturnID, kAnyTransactionID, &event);
        if (!status) status = AESendMessage(&event, NULL, kAENoReply, kAEDefaultTimeout);
        AEDisposeDesc(&event); AEDisposeDesc(&target);
        if (status) { fprintf(stderr, "self quit send failed: %d\n", (int)status); return 2; }
        double deadline = CFAbsoluteTimeGetCurrent() + 3;
        while (CFAbsoluteTimeGetCurrent() < deadline) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, .01, false);
            app_termination_pump();
        }
        fprintf(stderr, "FAIL: queued quit was not handled\n");
        return 1;
    }
}
