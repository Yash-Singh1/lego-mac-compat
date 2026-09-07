#include "../src/resource_bridge.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static uint32_t cursor = 0x10000000;
uint32_t compat_runtime32_allocate(size_t size, int clear) {
  uint32_t p = cursor + 16;
  cursor += (uint32_t)((size + 31) & ~(size_t)15);
  *(size_t *)(uintptr_t)(p - 16) = size;
  if (clear)
    memset((void *)(uintptr_t)p, 0, size);
  return p;
}
void compat_runtime32_deallocate(uint32_t p) { (void)p; }
uint32_t compat_runtime32_reallocate(uint32_t p, size_t size) {
  uint32_t result = compat_runtime32_allocate(size, 0);
  if (p) {
    size_t old = *(size_t *)(uintptr_t)(p - 16);
    memcpy((void *)(uintptr_t)result, (void *)(uintptr_t)p,
           old < size ? old : size);
  }
  return result;
}
static uint64_t call(const char *name, uint32_t a, uint32_t b, uint32_t c) {
  uint32_t args[] = {a, b, c};
  uint64_t result = 0;
  assert(resource_bridge32_dispatch(name, args, &result));
  return result;
}
static void put16(unsigned char *p, unsigned value) {
  p[0] = value >> 8;
  p[1] = value;
}
static void put32(unsigned char *p, uint32_t value) {
  put16(p, value >> 16);
  put16(p + 2, value);
}
static void write_file(const char *path, const void *bytes, size_t size) {
  FILE *f = fopen(path, "wb");
  assert(f);
  assert(fwrite(bytes, 1, size, f) == size);
  assert(!fclose(f));
}
int main(void) {
  assert(mmap((void *)0x10000000, 0x100000, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == (void *)0x10000000);
  unsigned char fork[348] = {0};
  put32(fork, 256);
  put32(fork + 4, 288);
  put32(fork + 8, 17);
  put32(fork + 12, 60);
  put32(fork + 256, 13);
  memcpy(fork + 260, "\0\2\5Alpha\4beta", 13);
  unsigned char *map = fork + 288;
  put16(map + 24, 28);
  put16(map + 26, 50);
  put16(map + 28, 0);
  memcpy(map + 30, "STR#", 4);
  put16(map + 34, 0);
  put16(map + 36, 10);
  put16(map + 38, 1008);
  put16(map + 40, 0);
  memcpy(map + 50, "\10greeting", 9);
  char dir[] = "/tmp/lp32-resource-XXXXXX";
  assert(mkdtemp(dir));
  char path[256], sidecar[280];
  snprintf(path, sizeof(path), "%s/test.rsrc", dir);
  snprintf(sidecar, sizeof(sidecar), "%s.lp32-rsrc", path);
  write_file(path, fork, sizeof(fork));
  int16_t file = resource_bridge32_open(path);
  assert(file > 0);
  uint32_t handle = (uint32_t)call("_GetResource", 0x53545223, 1008, 0);
  assert(handle && call("_GetHandleSize", handle, 0, 0) == 13);
  uint32_t string = compat_runtime32_allocate(256, 1);
  call("_GetIndString", string, 1008, 2);
  assert(!memcmp((void *)(uintptr_t)string, "\4beta", 5));
  memcpy((void *)(uintptr_t)string, "\10greeting", 9);
  assert(call("_GetNamedResource", 0x53545223, string, 0) == handle);
  call("_ReleaseResource", handle, 0, 0);
  assert(call("_ResError", 0, 0, 0) == 0);
  call("_CloseResFile", file, 0, 0);
  /* GetCTable must prefer the application's palette and convert every
   * big-endian ColorTable field without modifying the resource handle. */
  unsigned char palette_fork[348];
  memcpy(palette_fork, fork, sizeof(fork));
  put32(palette_fork + 8, 28);
  put32(palette_fork + 256, 24);
  unsigned char *palette = palette_fork + 260;
  put32(palette, 0x12345678);
  put16(palette + 4, 0x8000);
  put16(palette + 6, 1);
  for (unsigned i = 0; i < 8; ++i)
    put16(palette + 8 + i * 2, 0x1000 + i);
  memcpy(palette_fork + 318, "clut", 4);
  put16(palette_fork + 326, 256);
  write_file(path, palette_fork, sizeof(palette_fork));
  file = resource_bridge32_open(path);
  assert(file > 0);
  uint32_t table = (uint32_t)call("_GetCTable", 256, 0, 0);
  assert(table);
  uint16_t *colors = (void *)(uintptr_t)*(uint32_t *)(uintptr_t)table;
  assert(*(uint32_t *)colors == 0x12345678 && colors[2] == 0x8000 &&
         colors[3] == 1);
  for (unsigned i = 0; i < 8; ++i)
    assert(colors[4 + i] == 0x1000 + i);
  call("_DisposeCTable", table, 0, 0);
  call("_CloseResFile", file, 0, 0);
  write_file(path, "unrelated data fork", 19);
  write_file(sidecar, fork, sizeof(fork));
  file = resource_bridge32_open(path);
  assert(file > 0);
  call("_CloseResFile", file, 0, 0);
  unlink(sidecar);
  put32(fork + 4, 0xfffffff0);
  write_file(path, fork, sizeof(fork));
  assert(resource_bridge32_open(path) == -1);
  handle = (uint32_t)call("_NewHandleClear", 32, 0, 0);
  assert(handle);
  unsigned char *data = (void *)(uintptr_t)*(uint32_t *)(uintptr_t)handle;
  for (int i = 0; i < 32; ++i)
    assert(data[i] == 0);
  data[0] = 91;
  call("_ReallocateHandle", handle, 128, 0);
  assert(call("_GetHandleSize", handle, 0, 0) == 128 &&
         *(unsigned char *)(uintptr_t)*(uint32_t *)(uintptr_t)handle == 91);
  call("_DisposeHandle", handle, 0, 0);
  call("_GetHandleSize", handle, 0, 0);
  assert((int16_t)call("_MemError", 0, 0, 0) == -109);
  unlink(path);
  rmdir(dir);
  puts("Resource bridge PASS (big-endian map, STR# and named lookup, release, "
       "sidecar precedence, corrupt map rejection, handles)");
  return 0;
}
