#ifndef LP32_STEAM_BRIDGE_H
#define LP32_STEAM_BRIDGE_H
#include <stdint.h>
/* Returns nonzero only for handled Steam imports. */
int steam_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
/* Set the relocated title's identity before guest initializers/threads run.
   Does not initialize Steam or override its ownership/relaunch decisions. */
int steam_bridge32_prepare_environment(uint32_t app_id);
int steam_bridge32_probe_storage(void);
int steam_bridge32_call_uses_sret(const char *,const uint32_t *);
#endif
