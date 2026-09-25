#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Deliberately absent from the export list, like the retail engine. */
uint32_t sv_sendtables[13];
static unsigned initialized, setter_calls;
__attribute__((constructor)) static void initialize(void)
{
    sv_sendtables[3] = (uint32_t)(uintptr_t)"sv_sendtables";
    sv_sendtables[11] = 0x40e00000; /* 7.0f */
    sv_sendtables[12] = 7;
    if (getenv("COOP_BAD_CONVAR")) sv_sendtables[3] = UINT32_MAX - 1;
    initialized = 1;
}

void setter(uint32_t *, const char *) __asm__("__ZN6ConVar8SetValueEPKc");
void setter(uint32_t *object, const char *value)
{
    if (!initialized || object != sv_sendtables) __builtin_trap();
    if (getenv("COOP_TRAP_SETTER")) __builtin_trap();
    ++setter_calls;
    object[12] = !strcmp(value, "1");
    object[11] = object[12] ? 0x3f800000 : 0;
}

int fixture_value(void)
{
    if (!initialized || setter_calls > 2) return -1;
    if (sv_sendtables[12] == 1 && sv_sendtables[11] != 0x3f800000) return -2;
    return (int)sv_sendtables[12];
}
