#include "focus_policy.h"
#import <AppKit/AppKit.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>

static int managed_input_state = -1;
void lp32_set_managed_input_active(int active)
{
    __atomic_store_n(&managed_input_state, !!active, __ATOMIC_RELEASE);
}

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
    /* Scripted tests already isolate their input. */
    int managed = __atomic_load_n(&managed_input_state, __ATOMIC_ACQUIRE);
    if (getenv("LP32_BACKGROUND_TEST")) return 0;
    /* Carbon's event loop does not maintain NSApplication.isActive reliably.
       Its presenter supplies the process activation state instead. */
    if (managed >= 0) return !managed;
    return lp32_continue_when_inactive() && ![NSApp isActive];
}
