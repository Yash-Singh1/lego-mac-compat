#ifndef LP32_STEAM_STARTUP_H
#define LP32_STEAM_STARTUP_H
#include <stdbool.h>

struct steam_startup_ops {
    void *context;
    bool (*running)(void *);
    bool (*initialize)(void *);
    void (*shutdown)(void *);
    bool (*open)(void *);
    bool (*wait)(void *);
};
enum steam_startup_result { STEAM_STARTUP_READY, STEAM_STARTUP_OPEN_FAILED, STEAM_STARTUP_TIMEOUT };
enum steam_startup_result steam_startup_prepare(const struct steam_startup_ops *ops);
#endif
