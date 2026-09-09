#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include "tfu_steam.h"
#include "game_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool steam_build;
static unsigned client_queries;
static const struct lp32_game_profile profile = {.title = LP32_TITLE_TFU};
const struct lp32_game_profile *lp32_profile(void) { return &profile; }

@interface SourceFixture : NSObject
@end
@implementation SourceFixture
- (id)objectForInfoDictionaryKey:(NSString *)key {
    assert([key isEqualToString:@"AspyrBuild"]);
    return steam_build ? @"steam" : @"retail";
}
@end
void *carbon_bridge32_game_bundle(void) { return [[[SourceFixture alloc] init] autorelease]; }

static NSArray *no_client(id receiver, SEL selector, NSString *identifier) {
    (void)receiver; (void)selector;
    assert([identifier isEqualToString:@"com.valvesoftware.steam"]);
    ++client_queries;
    return @[];
}

int main(int argc, char **argv) {
    assert(argc == 2);
    @autoreleasepool {
        /* Replace process discovery only inside this isolated test process.
           Do not stop, connect to, or change the user's live Steam client. */
        Method method = class_getClassMethod([NSRunningApplication class],
            @selector(runningApplicationsWithBundleIdentifier:));
        method_setImplementation(method, (IMP)no_client);
        unsetenv("SteamAppId"); unsetenv("SteamGameId");
        unsetenv("LP32_HEADLESS"); unsetenv("LP32_BACKGROUND_TEST");
        steam_build = strcmp(argv[1], "retail") != 0;
        if (!strcmp(argv[1], "headless")) setenv("LP32_HEADLESS", "1", 1);
        if (!strcmp(argv[1], "background")) setenv("LP32_BACKGROUND_TEST", "1", 1);
        tfu_steam32_start();
        tfu_steam32_start();
        assert(client_queries == (!strcmp(argv[1], "missing") ? 1u : 0u));
        assert(!getenv("SteamAppId") && !getenv("SteamGameId"));
        assert(!NSApp); /* No application or modal startup UI was created. */
        printf("PASS: %s continues without Steam, repeated checks, or UI\n", argv[1]);
    }
}
