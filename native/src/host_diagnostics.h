#ifndef LP32_HOST_DIAGNOSTICS_H
#define LP32_HOST_DIAGNOSTICS_H
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/sysctl.h>

/* Public hardware identifiers only; no host name, serial number or account. */
static inline void lp32_host_description(char *text, size_t capacity)
{
    char model[128] = "unknown", chip[128] = "unknown";
    char os[64] = "unknown", build[64] = "unknown";
    uint64_t memory = 0;
    int translated = 0, cpus = 0;
#define HOST_QUERY(key, value) do { \
    size_t size = sizeof(value); sysctlbyname(key, &(value), &size, NULL, 0); \
} while (0)
    HOST_QUERY("hw.model", model);
    HOST_QUERY("machdep.cpu.brand_string", chip);
    HOST_QUERY("kern.osproductversion", os);
    HOST_QUERY("kern.osversion", build);
    HOST_QUERY("hw.memsize", memory);
    HOST_QUERY("hw.logicalcpu", cpus);
    HOST_QUERY("sysctl.proc_translated", translated);
#undef HOST_QUERY
    model[sizeof(model)-1] = chip[sizeof(chip)-1] = 0;
    os[sizeof(os)-1] = build[sizeof(build)-1] = 0;
    snprintf(text, capacity,
        "host model=%s chip=%s memory_bytes=%llu logical_cpus=%d macOS=%s (%s) "
        "rosetta=%d loader_built=%s %s fast_imports=%d guest_lock=%d",
        model, chip, (unsigned long long)memory, cpus, os, build, translated,
        __DATE__, __TIME__, getenv("LP32_NO_FAST_IMPORTS") == NULL,
        getenv("LP32_GUEST_LOCK") != NULL);
}
#endif
