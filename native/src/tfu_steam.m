#import <Cocoa/Cocoa.h>
#include "tfu_steam.h"
#include "carbon_bridge.h"
#include "game_profile.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* TFU's original Mac Steam build has no Steamworks SDK. Steam tracks its
   launch when started through the client, but our relocated Finder app has
   neither that process ancestry nor a Steam connection. Use the installed
   client's public, versioned ISteamClient interface for this process only.
   No Valve binaries are redistributed or borrowed from another game.

   ABI: ValveSoftware/source-sdk-2013/src/public/steam/{isteamclient,isteamutils}.h
   SteamClient020 slots 0/1/2/4/9; SteamUtils010 slot 9. These are versioned
   interface positions, not addresses in a game or client executable. */
static void *client_library, *client;
static int pipe_handle, user_handle;

static void disconnect(void) {
    if (!client) return;
    void **vtable = *(void ***)client;
    if (user_handle) ((void (*)(void *, int, int))vtable[4])(client, pipe_handle, user_handle);
    if (pipe_handle) ((bool (*)(void *, int))vtable[1])(client, pipe_handle);
    user_handle = pipe_handle = 0;
    /* Keep the library mapped: its own worker threads may still be exiting.
       _Exit/crash paths also close IPC when the OS destroys the process. */
}

void tfu_steam32_start(void) {
    static bool attempted;
    if (attempted || lp32_profile()->title != LP32_TITLE_TFU ||
        getenv("LP32_HEADLESS") || getenv("LP32_BACKGROUND_TEST")) return;
    attempted = true;
    NSBundle *source = carbon_bridge32_game_bundle();
    if (![[source objectForInfoDictionaryKey:@"AspyrBuild"] isEqual:@"steam"]) return;
    for (const char **key = (const char *[]){"SteamAppId", "SteamGameId", NULL}; *key; ++key) {
        const char *value = getenv(*key);
        if (value && *value && strcmp(value, "32430")) {
            fprintf(stderr, "compat32: TFU Steam tracking skipped: conflicting %s\n", *key);
            return;
        }
    }
    NSArray *running = [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.valvesoftware.steam"];
    NSString *path = nil;
    for (NSRunningApplication *app in running) {
        NSString *candidate = [[app.executableURL.path stringByDeletingLastPathComponent]
            stringByAppendingPathComponent:@"steamclient.dylib"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:candidate]) { path = candidate; break; }
    }
    if (!path) {
        fprintf(stderr, "compat32: TFU Steam tracking unavailable: Steam is not running\n");
        return;
    }
    setenv("SteamAppId", "32430", 1);
    setenv("SteamGameId", "32430", 1);
    client_library = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
    if (!client_library) {
        fprintf(stderr, "compat32: TFU Steam client load failed: %s\n", dlerror());
        return;
    }
    void *(*factory)(const char *, int *) = dlsym(client_library, "CreateInterface");
    client = factory ? factory("SteamClient020", NULL) : NULL;
    if (!client) {
        fprintf(stderr, "compat32: TFU SteamClient020 unavailable\n");
        return;
    }
    void **vtable = *(void ***)client;
    pipe_handle = ((int (*)(void *))vtable[0])(client);
    if (pipe_handle) user_handle = ((int (*)(void *, int))vtable[2])(client, pipe_handle);
    void *utils = user_handle ? ((void *(*)(void *, int, const char *))vtable[9])
        (client, pipe_handle, "SteamUtils010") : NULL;
    unsigned app_id = utils ? ((unsigned (*)(void *))(*(void ***)utils)[9])(utils) : 0;
    if (!user_handle || app_id != 32430) {
        fprintf(stderr, "compat32: TFU Steam connection unavailable or wrong app identity (%u)\n", app_id);
        disconnect();
        return;
    }
    atexit(disconnect);
    fprintf(stderr, "compat32: TFU connected to Steam app=%u pid=%d\n", app_id, getpid());
}
