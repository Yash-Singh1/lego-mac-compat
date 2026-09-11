#include "game_profile.h"
#include "macho_loader.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    unsetenv("LP32_GAME");
    const char *names[] = {"pirates", "clonewars", "marvel", "saga-retail", "LEGOCompleteSaga10", "saga"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const struct lp32_game_profile *profile = lp32_profile_named(names[i]);
        assert(profile);
        struct macho_image32 image = {
            .entry_eip = profile->entry_eip,
            .max_address = profile->image_end,
        };
        assert(lp32_profile_select(&image) == 0);
        assert(lp32_profile() == profile);
        /* Neither half of the fingerprint alone may select a patch layout. */
        ++image.max_address;
        assert(lp32_profile_select(&image) == -1);
        --image.max_address;
        ++image.entry_eip;
        assert(lp32_profile_select(&image) == -1);
        assert(profile->thread_argument_is_direct == (i >= 2));
        assert(profile->callee_pops_struct_return == (i == 2 || i == 5));
        assert((profile->steam_achievement_guard != NULL) == (i == 2));
        assert(!profile->loading_screen);
        assert(profile->steam_app_id == (i == 2 ? 249130u : 0u));
        if (profile->title != LP32_TITLE_COMPLETE_SAGA)
            assert(profile->controller && profile->display);
    }
    const char *cod_names[] = {"cod4", "cod4mp"};
    for (unsigned i = 0; i < 2; ++i) {
        const struct lp32_game_profile *p = lp32_profile_named(cod_names[i]);
        assert(p && p->steam_app_id == 7940 && p->callee_pops_struct_return);
        assert(!p->controller && !p->startup_latch && !p->steam_achievement_guard);
        assert((p->loading_screen != NULL) == (i == 0));
        struct macho_image32 image = {.entry_eip = p->entry_eip, .max_address = p->image_end};
        assert(lp32_profile_select(&image) == 0 && lp32_profile() == p);
        ++image.max_address;
        assert(lp32_profile_select(&image) == -1);
        --image.max_address;
        ++image.entry_eip;
        assert(lp32_profile_select(&image) == -1);
        assert(p->depth_capability_check.length == 7);
    }
    assert(lp32_profile_named("cod") == lp32_profile_named("cod4"));
    assert(lp32_profile_named("cod4-mp") == lp32_profile_named("cod4mp"));
    assert(lp32_profile_named("LEGOMARVEL") == lp32_profile_named("marvel"));
    assert(lp32_profile_named("lsw3") == lp32_profile_named("clonewars"));
    assert(lp32_profile_named("lswc") == lp32_profile_named("saga"));
    assert(lp32_profile_named("completesaga") == lp32_profile_named("saga"));
    assert(lp32_profile_named("saga-steam") == lp32_profile_named("saga"));
    assert(lp32_profile_named("saga-retail") != lp32_profile_named("saga"));
    struct macho_image32 modern = {.entry_eip = 0x1234, .main_address = 0x1234};
    assert(lp32_profile_main_address(&modern) == 0x1234);
    assert(!lp32_profile_named(NULL));
    assert(!lp32_profile_named("unsupported"));
    struct macho_image32 unknown = {0};
    setenv("LP32_GAME", "unsupported", 1);
    assert(lp32_profile_select(&unknown) == -1);
    setenv("LP32_GAME", "marvel", 1);
    assert(lp32_profile_select(&unknown) == 0);
    assert(lp32_profile()->title == LP32_TITLE_MARVEL);
    unsetenv("LP32_GAME");
    puts("game-profile PASS (fingerprints, aliases, overrides, ABI isolation)");
    return 0;
}
