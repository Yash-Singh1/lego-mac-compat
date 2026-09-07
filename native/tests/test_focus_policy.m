#include "focus_policy.h"
#import <AppKit/AppKit.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

/* No windows, activation, or real user input needed to test both states. */
@interface FocusTestApp : NSObject
@property BOOL active;
- (BOOL)isActive;
@end
@implementation FocusTestApp
- (BOOL)isActive { return _active; }
@end

int main(int argc, const char **argv)
{
    assert(argc == 2);
    int expected = atoi(argv[1]);
    assert(!lp32_focus_setting(NULL, 0));
    assert(lp32_focus_setting(NULL, 1));
    const char *off[] = {"0", "false", "no", "off", "", "invalid"};
    const char *on[] = {"1", "true", "YES", "On"};
    for (unsigned i = 0; i < sizeof(off)/sizeof(*off); ++i)
        assert(!lp32_focus_setting(off[i], 1));
    for (unsigned i = 0; i < sizeof(on)/sizeof(*on); ++i)
        assert(lp32_focus_setting(on[i], 0));
    assert(lp32_continue_when_inactive() == expected);
    assert(lp32_ignore_guest_focus_loss() == expected);
    NSApplication *saved = NSApp;
    FocusTestApp *mock = [FocusTestApp new];
    NSApp = (NSApplication *)mock;
    mock.active = NO;
    assert(lp32_suppress_background_input() == expected);
    mock.active = YES;
    assert(!lp32_suppress_background_input());
    mock.active = NO;
    setenv("LP32_BACKGROUND_TEST", "1", 1);
    assert(lp32_ignore_guest_focus_loss());
    assert(!lp32_suppress_background_input());
    setenv("LP32_TEST_FOCUS_LOSS", "1,2", 1);
    assert(lp32_ignore_guest_focus_loss() == expected);
    NSApp = saved;
    [mock release];
    puts("focus policy PASS (default, overrides, guest focus, inactive input, test isolation)");
    return 0;
}
