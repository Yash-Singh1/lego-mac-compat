#include "crypto_bridge.h"
#include "compat_runtime.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <Security/SecRandom.h>
#include <Security/SecBase.h>

/* OpenSSL 0.9.8 exposed these structures to i386 callers. Keep guest limbs
 * 32-bit and rebuild native opaque keys for verification with system LibreSSL.
 * ABI reference: openssl/OpenSSL_0_9_8zh/crypto/{bn/bn,rsa/rsa}.h. */
struct bn32 {
  uint32_t limbs;
  int32_t top, capacity, negative, flags;
};
struct rsa32 {
  uint32_t pad, version, method, engine, numbers[8], extra[2], references,
      flags, caches[6];
};
static uint32_t allocate_callback, reallocate_callback, free_callback;
static int allocation_started;
static void *library;
static void *crypto_symbol(const char *name) {
  if (!library)
    library = dlopen("/usr/lib/libcrypto.46.dylib", RTLD_NOW | RTLD_LOCAL);
  return library ? dlsym(library, name) : NULL;
}
static uint32_t allocate32(uint32_t size) {
  allocation_started = 1;
  uint32_t p = allocate_callback
                   ? compat_runtime32_call(allocate_callback, &size, 1)
                   : compat_runtime32_allocate(size, 0);
  if (p)
    memset((void *)(uintptr_t)p, 0, size);
  return p;
}
static void free32(uint32_t p) {
  if (!p)
    return;
  if (free_callback)
    compat_runtime32_call(free_callback, &p, 1);
  else
    compat_runtime32_deallocate(p);
}
static void bn_free32(uint32_t p) {
  if (!p)
    return;
  struct bn32 *bn = (void *)(uintptr_t)p;
  free32(bn->limbs);
  if (bn->flags & 1)
    free32(p);
}
static void *native_bn(uint32_t p) {
  if (!p)
    return NULL;
  struct bn32 *bn = (void *)(uintptr_t)p;
  if (bn->top < 0 || bn->top > 16384 || bn->negative || (bn->top && !bn->limbs))
    return NULL;
  size_t length = (size_t)bn->top * 4;
  unsigned char *bytes = malloc(length ? length : 1);
  if (!bytes)
    return NULL;
  const unsigned char *limbs = (void *)(uintptr_t)bn->limbs;
  for (size_t i = 0; i < length; ++i)
    bytes[i] = limbs[length - 1 - i];
  void *(*convert)(const unsigned char *, int, void *) =
      crypto_symbol("BN_bin2bn");
  void *result = convert ? convert(bytes, (int)length, NULL) : NULL;
  free(bytes);
  return result;
}
int crypto_bridge32_dispatch(const char *name, const uint32_t *a,
                             uint64_t *out) {
  if (strcmp(name, "_SecRandomCopyBytes") == 0) {
    /* kSecRandomDefault is NULL; Apple exposes no other generator handles. */
    *out = (uint32_t)(a[0] ? errSecParam :
        SecRandomCopyBytes(kSecRandomDefault, a[1], (void *)(uintptr_t)a[2]));
    return 1;
  }
  if (strcmp(name, "_CRYPTO_set_mem_functions") == 0) {
    *out = !allocation_started && a[0] && a[1] && a[2];
    if (*out) {
      allocate_callback = a[0];
      reallocate_callback = a[1];
      free_callback = a[2];
    }
    return 1;
  }
  if (strcmp(name, "_BN_bin2bn") == 0) {
    *out = 0;
    if ((int32_t)a[1] < 0 || a[1] > 65536 || (a[1] && !a[0]))
      return 1;
    uint32_t p = a[2] ? a[2] : allocate32(sizeof(struct bn32));
    if (!p)
      return 1;
    struct bn32 *bn = (void *)(uintptr_t)p;
    if (!a[2])
      bn->flags = 1;
    uint32_t count = (a[1] + 3) / 4;
    if (count > (uint32_t)bn->capacity) {
      uint32_t limbs = allocate32(count * 4);
      if (!limbs) {
        if (!a[2])
          free32(p);
        return 1;
      }
      free32(bn->limbs);
      bn->limbs = limbs;
      bn->capacity = (int32_t)count;
    }
    unsigned char *limbs = (void *)(uintptr_t)bn->limbs;
    const unsigned char *bytes = (void *)(uintptr_t)a[0];
    if (count)
      memset(limbs, 0, count * 4);
    for (uint32_t i = 0; i < a[1]; ++i)
      limbs[i] = bytes[a[1] - 1 - i];
    bn->top = (int32_t)count;
    bn->negative = 0;
    while (bn->top && !((uint32_t *)limbs)[bn->top - 1])
      --bn->top;
    *out = p;
    return 1;
  }
  if (strcmp(name, "_RSA_new") == 0) {
    *out = allocate32(sizeof(struct rsa32));
    if (*out)
      ((struct rsa32 *)(uintptr_t)*out)->references = 1;
    return 1;
  }
  if (strcmp(name, "_RSA_free") == 0) {
    struct rsa32 *rsa = (void *)(uintptr_t)a[0];
    if (rsa && --rsa->references == 0) {
      for (unsigned i = 0; i < 8; ++i)
        bn_free32(rsa->numbers[i]);
      free32(a[0]);
    }
    *out = 0;
    return 1;
  }
  if (strcmp(name, "_RSA_verify") == 0) {
    void *(*new_key)(void) = crypto_symbol("RSA_new");
    int (*set_key)(void *, void *, void *, void *) =
        crypto_symbol("RSA_set0_key");
    int (*verify)(int, const void *, unsigned, const void *, unsigned, void *) =
        crypto_symbol("RSA_verify");
    void (*free_key)(void *) = crypto_symbol("RSA_free");
    void (*free_bn)(void *) = crypto_symbol("BN_free");
    if (!new_key || !set_key || !verify || !free_key || !free_bn)
      return 0;
    *out = 0;
    struct rsa32 *guest = (void *)(uintptr_t)a[5];
    if (!guest)
      return 1;
    void *key = new_key(), *n = native_bn(guest->numbers[0]),
         *e = native_bn(guest->numbers[1]);
    if (key && n && e && set_key(key, n, e, NULL)) {
      *out = verify((int)a[0], (void *)(uintptr_t)a[1], a[2],
                    (void *)(uintptr_t)a[3], a[4], key);
    } else {
      free_bn(n);
      free_bn(e);
    }
    free_key(key);
    return 1;
  }
  return 0;
}
