#include "../src/quicktime_bridge.h"
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* COD4 streams music and dialogue through NewMovieFromDataRef. During slow
   motion CStreamSound::UpdateAllStreams calls SetMovieRate and IsMovieDone
   for every stream each frame, and loops a finished stream with
   GoToBeginningOfMovie. */

static uint32_t handle_size;
int resource_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *out) {
  (void)a;
  if (strcmp(name, "_GetHandleSize")) return 0;
  *out = handle_size;
  return 1;
}
int carbon_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *out) {
  (void)name; (void)a; (void)out;
  return 0;
}
int carbon_bridge32_path_from_spec(uint32_t spec, char *path, uint32_t capacity) {
  (void)spec; (void)path; (void)capacity;
  return 0;
}
int carbon_bridge32_region_bounds(uint32_t region, void *rect) {
  (void)region; (void)rect;
  return 0;
}
uint32_t compat_runtime32_call(uint32_t function, const uint32_t *arguments, size_t count) {
  (void)function; (void)arguments; (void)count;
  return 0;
}
uint32_t compat_runtime32_allocate(size_t size, int clear) {
  (void)size; (void)clear;
  return 0;
}
void compat_runtime32_deallocate(uint32_t pointer) { (void)pointer; }

static uint32_t call(const char *name, const uint32_t *arguments) {
  uint64_t out = 0;
  assert(quicktime_bridge32_dispatch(name, arguments, &out));
  return (uint32_t)out;
}

static void write_wav(const char *path, double seconds) {
  uint32_t rate = 44100, frames = (uint32_t)(rate * seconds), bytes = frames * 2;
  FILE *file = fopen(path, "wb");
  assert(file);
  uint32_t header[] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16,
                       0x00010001, rate, rate * 2, 0x00100002, 0x61746164, bytes};
  fwrite(header, sizeof(header), 1, file);
  for (uint32_t i = 0; i < frames; ++i) {
    int16_t sample = (int16_t)(8000 * ((i / 50) & 1 ? 1 : -1));
    fwrite(&sample, 2, 1, file);
  }
  fclose(file);
}

static uint32_t *guest;
static uint32_t open_movie(const char *path) {
  NSString *url = [[NSURL fileURLWithPath:@(path)] absoluteString];
  char *text = (char *)(guest + 64);
  strcpy(text, url.UTF8String);
  handle_size = (uint32_t)strlen(text) + 1;
  guest[0] = (uint32_t)(uintptr_t)text;
  uint32_t handle = (uint32_t)(uintptr_t)guest;
  uint32_t movie_out = (uint32_t)(uintptr_t)(guest + 16);
  uint32_t args[] = {movie_out, 0, 0, handle, 0x75726c20};
  assert(call("_NewMovieFromDataRef", args) == 0);
  assert(guest[16]);
  return guest[16];
}

static void pump(double seconds) {
  [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

static void set_rate(uint32_t movie, double rate) {
  uint32_t args[] = {movie, (uint32_t)(int32_t)(rate * 65536)};
  assert(call("_SetMovieRate", args) == 0);
}

static void test_rate_ramp_does_not_block(const char *path) {
  uint32_t movie = open_movie(path);
  uint32_t one[] = {movie, 0};
  set_rate(movie, 1.0);
  pump(0.3);
  uint64_t worst = 0;
  uint32_t previous = call("_GetMovieTime", one);
  for (int frame = 0; frame < 45; ++frame) {
    set_rate(movie, frame < 15 ? 1.0 - 0.7 * frame / 15.0 :
                    frame < 30 ? 0.3 : 0.3 + 0.7 * (frame - 30) / 15.0);
    uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint32_t done = call("_IsMovieDone", one);
    uint32_t time = call("_GetMovieTime", one);
    uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
    if (elapsed > worst) worst = elapsed;
    assert(!done);
    assert(time >= previous);
    previous = time;
    pump(0.016);
  }
  assert(previous > 0);
  printf("rate ramp: worst IsMovieDone+GetMovieTime %.2f ms\n", worst / 1e6);
  assert(worst < 8000000);
  call("_DisposeMovie", one);
}

static void test_loop_restart(const char *path) {
  uint32_t movie = open_movie(path);
  uint32_t one[] = {movie, 0};
  set_rate(movie, 1.0);
  int finished = 0;
  for (int i = 0; i < 300 && !finished; ++i) {
    pump(0.01);
    finished = call("_IsMovieDone", one);
  }
  assert(finished);
  assert(call("_GoToBeginningOfMovie", one) == 0);
  assert(!call("_IsMovieDone", one));
  assert(call("_GetMovieTime", one) == 0);
  set_rate(movie, 1.0);
  for (int i = 0; i < 30; ++i) {
    pump(0.01);
    assert(!call("_IsMovieDone", one));
  }
  assert(call("_GetMovieTime", one) > 0);
  finished = 0;
  for (int i = 0; i < 300 && !finished; ++i) {
    pump(0.01);
    finished = call("_IsMovieDone", one);
  }
  assert(finished);
  call("_DisposeMovie", one);
}

int main(void) {
  @autoreleasepool {
    /* An unrelated zero-argument import supplies a pointer at the guard
       page above its guest stack. Dispatch must not read even arguments[0]. */
    size_t page_size = (size_t)getpagesize();
    const uint32_t *guard = mmap(NULL, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(guard != MAP_FAILED);
    const char *unrelated[] = {"_CGLGetCurrentContext", "_glFinish", "_UnknownNoArgumentImport"};
    for (unsigned i = 0; i < sizeof unrelated / sizeof *unrelated; ++i) {
      uint64_t result = 0xabcdef;
      assert(!quicktime_bridge32_dispatch(unrelated[i], guard, &result));
      assert(result == 0xabcdef);
    }
    assert(!munmap((void *)guard, page_size));
    if (getenv("LP32_QUICKTIME_GUARD_SELFTEST")) {
      puts("QuickTime unrelated-import guard-page PASS");
      return 0;
    }
    setenv("LP32_MUTE_AUDIO", "1", 1);
    guest = mmap((void *)0x30000000, 0x10000, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    assert(guest == (void *)0x30000000);
    char long_path[] = "/tmp/lp32-qt-long-XXXXXX.wav";
    char short_path[] = "/tmp/lp32-qt-short-XXXXXX.wav";
    close(mkstemps(long_path, 4));
    close(mkstemps(short_path, 4));
    write_wav(long_path, 5.0);
    write_wav(short_path, 0.8);
    test_rate_ramp_does_not_block(long_path);
    test_loop_restart(short_path);
    unlink(long_path);
    unlink(short_path);
    printf("quicktime bridge tests passed\n");
  }
  return 0;
}
