#include "steam_startup.h"

static bool ready(const struct steam_startup_ops *ops)
{
    if (!ops->running(ops->context) || !ops->initialize(ops->context)) return false;
    ops->shutdown(ops->context);
    return true;
}

enum steam_startup_result steam_startup_prepare(const struct steam_startup_ops *ops)
{
    if (ready(ops)) return STEAM_STARTUP_READY;
    /* Open Steam itself: Steam's AppID relaunch would select the original
       library executable instead of this user's converted application. */
    if (!ops->open(ops->context)) return STEAM_STARTUP_OPEN_FAILED;
    do {
        if (ready(ops)) return STEAM_STARTUP_READY;
    } while (ops->wait(ops->context));
    return STEAM_STARTUP_TIMEOUT;
}
