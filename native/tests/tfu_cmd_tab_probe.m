/* Opt-in desktop integration test. Sends real Cmd+Tab, never pre-activates
   the target: force-activating it first can hide the cold-launch failure. */
#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#include <unistd.h>

static const char *report_path;
static NSDictionary *snapshot(const char *phase, CGWindowID target)
{
    ProcessSerialNumber psn = {0};
    pid_t foreground = 0;
    GetFrontProcess(&psn);
    GetProcessPID(&psn, &foreground);
    NSArray *windows = CFBridgingRelease(CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID));
    CGRect bounds = CGRectZero;
    NSInteger target_level = 0;
    for (NSDictionary *window in windows)
        if ([window[(id)kCGWindowNumber] unsignedIntValue] == target) {
            CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)window[(id)kCGWindowBounds], &bounds);
            target_level = [window[(id)kCGWindowLayer] integerValue];
        }
    CGWindowID top = 0;
    pid_t top_owner = 0;
    for (NSDictionary *window in windows) {
        /* Include the foreground fullscreen surface at its presentation
           level, as well as normal application windows. System overlays
           such as the menu bar and Cmd+Tab panel are not game occlusion. */
        BOOL is_target = [window[(id)kCGWindowNumber] unsignedIntValue] == target;
        if ((!is_target && [window[(id)kCGWindowLayer] intValue]) ||
            [window[(id)kCGWindowAlpha] doubleValue] <= 0) continue;
        CGRect candidate;
        if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)window[(id)kCGWindowBounds], &candidate)) continue;
        if (CGRectIntersectsRect(bounds, candidate)) {
            top = [window[(id)kCGWindowNumber] unsignedIntValue];
            top_owner = [window[(id)kCGWindowOwnerPID] intValue];
            break;
        }
    }
    NSMutableDictionary *result = [@{@"phase": @(phase), @"foreground": @(foreground),
        @"target": @(target), @"targetLevel": @(target_level),
        @"topWindow": @(top), @"topOwner": @(top_owner)} mutableCopy];
    if (report_path) {
        NSData *data = [NSData dataWithContentsOfFile:@(report_path)];
        NSArray *report = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
        for (NSDictionary *state in report)
            if ([state[@"windowNumber"] unsignedIntValue] == target) result[@"cocoa"] = state;
    }
    NSData *json = [NSJSONSerialization dataWithJSONObject:result options:0 error:NULL];
    fwrite(json.bytes, 1, json.length, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return result;
}

static void key(CGKeyCode code, bool down, CGEventFlags flags)
{
    CGEventRef event = CGEventCreateKeyboardEvent(NULL, code, down);
    CGEventSetFlags(event, flags);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static void cmd_tab(void)
{
    BOOL rapid = getenv("LP32_FOCUS_RAPID") != NULL;
    key(55, true, kCGEventFlagMaskCommand);
    usleep(rapid ? 20000 : 150000);
    key(48, true, kCGEventFlagMaskCommand);
    usleep(rapid ? 20000 : 100000);
    key(48, false, kCGEventFlagMaskCommand);
    usleep(rapid ? 20000 : 200000);
    key(55, false, 0);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if ((argc != 3 && argc != 4) || !CGPreflightPostEventAccess()) {
            fprintf(stderr, "Requires target PID/window ID and existing event-posting access.\n");
            return 2;
        }
        pid_t pid = atoi(argv[1]);
        CGWindowID window = (CGWindowID)strtoul(argv[2], NULL, 10);
        report_path = argc == 4 ? argv[3] : NULL;
        NSDictionary *before = snapshot("before", window);
        if ([before[@"foreground"] intValue] != pid || [before[@"topWindow"] unsignedIntValue] != window) return 3;
        BOOL rapid = getenv("LP32_FOCUS_RAPID") != NULL;
        for (unsigned round = 0; round < (rapid ? 6u : 2u); ++round) {
            cmd_tab();
            if (rapid) usleep(round % 2 ? 200000 : 100000);
            else sleep(round ? 10 : 2);
            NSDictionary *away = snapshot("away", window);
            BOOL switched = [away[@"foreground"] intValue] != pid &&
                [away[@"topWindow"] unsignedIntValue] != window && [away[@"targetLevel"] integerValue] == 0;
            cmd_tab();
            sleep(2);
            NSDictionary *back = snapshot("return", window);
            if (!switched || [back[@"foreground"] intValue] != pid ||
                [back[@"topWindow"] unsignedIntValue] != window) return 1;
            if (report_path) {
                NSDictionary *state = back[@"cocoa"];
                for (NSString *flag in @[@"key", @"main", @"active", @"foreground", @"visible", @"onActiveSpace"])
                    if (![state[flag] boolValue]) return 1;
                if ([state[@"appKeyWindow"] unsignedIntValue] != window ||
                    [state[@"appMainWindow"] unsignedIntValue] != window) return 1;
            }
        }
    }
    return 0;
}
