#include "focus_policy.h"
#import <AppKit/AppKit.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>

int lp32_focus_setting(const char *environment, int bundle_default)
{
    if (!environment) return !!bundle_default;
    return !strcmp(environment, "1") || !strcasecmp(environment, "true") ||
           !strcasecmp(environment, "yes") || !strcasecmp(environment, "on");
}

int lp32_continue_when_inactive(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        id value = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"LP32ContinueWhenInactive"];
        int bundled = [value isKindOfClass:[NSNumber class]] && [value boolValue];
        enabled = lp32_focus_setting(getenv("LP32_CONTINUE_WHEN_INACTIVE"), bundled);
        if (enabled) fputs("compat32: continue while inactive enabled\n", stderr);
    }
    return enabled;
}

int lp32_ignore_guest_focus_loss(void)
{
    return lp32_continue_when_inactive() ||
        (getenv("LP32_BACKGROUND_TEST") && !getenv("LP32_TEST_FOCUS_LOSS"));
}

int lp32_suppress_background_input(void)
{
    /* Scripted tests already isolate their input. Native AppKit retains the
       real activation state even when guest focus queries return true. */
    return lp32_continue_when_inactive() && !getenv("LP32_BACKGROUND_TEST") &&
        ![NSApp isActive];
}
