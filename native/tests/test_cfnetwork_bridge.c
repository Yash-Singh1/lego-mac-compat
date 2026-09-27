#include "../src/cfnetwork_bridge.h"
#include <CFNetwork/CFNetwork.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>

uint64_t compat_runtime32_return_double(double x){uint64_t result;memcpy(&result,&x,8);return result;}
static CFTypeRef objects[128];
static unsigned count, retained, released, timer_fired, stream_fired;
uint32_t objc_bridge32_guest_object(void *object) {
  if (!object)
    return 0;
  for (unsigned i = 1; i <= count; ++i)
    if (objects[i] == object)
      return i;
  assert(count + 1 < 128);
  objects[++count] = CFRetain(object);
  return count;
}
void *objc_bridge32_host_object(uint32_t token) {
  assert(token <= count);
  return (void *)objects[token];
}
uint32_t compat_runtime32_call(uint32_t callback, const uint32_t *a,
                               uint32_t n) {
  if (callback == 1) {
    assert(n == 1 && (a[0] == 123 || a[0] == 456));
    retained++;
    return 456;
  } else if (callback == 2) {
    assert(n == 1 && a[0] == 456);
    released++;
  } else if (callback == 3) {
    assert(n == 2 && a[0] && a[1] == 456);
    timer_fired++;
  } else if (callback == 4) {
    assert(n == 3 && a[0] && a[2] == 456);
    if (a[1] == kCFStreamEventHasBytesAvailable)
      stream_fired++;
  } else
    assert(!"unknown callback");
  return 0;
}
static uint32_t token(CFTypeRef value) {
  return objc_bridge32_guest_object((void *)value);
}
static uint32_t dispatch(const char *name, uint32_t *a) {
  uint64_t result = 0;
  assert(cfnetwork_bridge32_dispatch(name, a, &result));
  return result;
}
int main(void) {
  uint32_t *low = mmap((void *)0x30000000, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  assert(low != MAP_FAILED);
  uint32_t *ctx = low;
  ctx[0] = 0;
  ctx[1] = 123;
  ctx[2] = 1;
  ctx[3] = 2;
  ctx[4] = 0;
  uint32_t a[12] = {0};
  CFURLRef url =
      CFURLCreateWithString(NULL, CFSTR("http://127.0.0.1/compat-test"), NULL);
  a[1] = token(CFSTR("POST"));
  a[2] = token(url);
  a[3] = cfnetwork_bridge32_pointer_import("_kCFHTTPVersion1_1");
  assert(a[3]);
  uint32_t request = dispatch("_CFHTTPMessageCreateRequest", a);
  assert(request);
  const UInt8 body[] = {0, 'a', 0xff};
  CFDataRef data = CFDataCreate(NULL, body, sizeof(body));
  a[0] = request;
  a[1] = token(data);
  dispatch("_CFHTTPMessageSetBody", a);
  uint32_t body_copy = dispatch("_CFHTTPMessageCopyBody", a);
  assert(CFEqual(objc_bridge32_host_object(body_copy), data));
  assert(cfnetwork_bridge32_pointer_import(
      "_kCFStreamPropertyHTTPResponseHeader"));
  assert(cfnetwork_bridge32_pointer_import("_kCFProxyHostNameKey"));
  assert(!cfnetwork_bridge32_pointer_import("_kCFNotARealConstant"));
  a[0] = 0;
  a[1] = 0;
  uint32_t response = dispatch("_CFHTTPMessageCreateEmpty", a);
  const char *wire = "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok";
  memcpy(low + 32, wire, strlen(wire));
  a[0] = response;
  a[1] = (uint32_t)(uintptr_t)(low + 32);
  a[2] = strlen(wire);
  assert(dispatch("_CFHTTPMessageAppendBytes", a));
  assert(dispatch("_CFHTTPMessageGetResponseStatusCode", a) == 201);
  uint32_t loop = dispatch("_CFRunLoopGetCurrent", a);
  assert(objc_bridge32_host_object(loop) == CFRunLoopGetCurrent());
  double date = CFAbsoluteTimeGetCurrent(), interval = 0;
  memset(a, 0, sizeof(a));
  memcpy(a + 1, &date, 8);
  memcpy(a + 3, &interval, 8);
  a[7] = 3;
  a[8] = (uint32_t)(uintptr_t)ctx;
  uint32_t timer = dispatch("_CFRunLoopTimerCreate", a);
  assert(timer && retained == 1);
  a[0] = loop;
  a[1] = timer;
  a[2] = token(kCFRunLoopDefaultMode);
  dispatch("_CFRunLoopAddTimer", a);
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.03, false);
  assert(timer_fired == 1);
  CFReadStreamRef stream = CFReadStreamCreateWithBytesNoCopy(
      NULL, body, sizeof(body), kCFAllocatorNull);
  uint32_t stream_token = token(stream);
  a[0] = stream_token;
  a[1] = kCFStreamEventHasBytesAvailable;
  a[2] = 4;
  a[3] = (uint32_t)(uintptr_t)ctx;
  assert(dispatch("_CFReadStreamSetClient", a));
  assert(retained >= 2);
  a[1] = loop;
  a[2] = token(kCFRunLoopDefaultMode);
  dispatch("_CFReadStreamScheduleWithRunLoop", a);
  assert(dispatch("_CFReadStreamOpen", a));
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.03, false);
  assert(stream_fired);
  a[1] = (uint32_t)(uintptr_t)(low + 64);
  a[2] = 8;
  assert(dispatch("_CFReadStreamRead", a) == sizeof(body));
  assert(!memcmp(low + 64, body, sizeof(body)));
  a[1] = 0;
  a[2] = 0;
  a[3] = 0;
  assert(dispatch("_CFReadStreamSetClient", a));
  assert(released <= retained);
  dispatch("_CFReadStreamClose", a);
  CFRelease(stream);

  /* MW2 reads its startup plist through file streams. Check 32-bit format
     and error outputs with canaries, then exercise the inverse write path. */
  char file_path[] = "/tmp/lp32-plist-XXXXXX";
  int fd = mkstemp(file_path);
  assert(fd >= 0);
  close(fd);
  CFURLRef file_url = CFURLCreateFromFileSystemRepresentation(NULL,
      (const UInt8 *)file_path, strlen(file_path), false);
  memset(a, 0, sizeof(a));
  a[1] = token(file_url);
  uint32_t output = dispatch("_CFWriteStreamCreateWithFile", a);
  a[0] = output;
  assert(dispatch("_CFWriteStreamOpen", a));
  low[100] = 0xabcddcba; low[101] = 0xffffffff; low[102] = 0x12344321;
  a[0] = token(CFSTR("MW2 plist round trip")); a[1] = output;
  a[2] = kCFPropertyListXMLFormat_v1_0; a[3] = 0;
  a[4] = (uint32_t)(uintptr_t)(low + 101);
  assert((int32_t)dispatch("_CFPropertyListWrite", a) > 0);
  assert(low[100] == 0xabcddcba && low[101] == 0 && low[102] == 0x12344321);
  a[0] = output; dispatch("_CFWriteStreamClose", a);
  a[0] = 0; a[1] = token(file_url);
  uint32_t input = dispatch("_CFReadStreamCreateWithFile", a);
  a[0] = input; assert(dispatch("_CFReadStreamOpen", a));
  low[104] = 0xfeedbeef; low[105] = 0; low[106] = 0xcafebabe;
  a[0] = 0; a[1] = input; a[2] = 0; a[3] = kCFPropertyListImmutable;
  a[4] = (uint32_t)(uintptr_t)(low + 105);
  a[5] = (uint32_t)(uintptr_t)(low + 101);
  uint32_t value = dispatch("_CFPropertyListCreateWithStream", a);
  assert(CFEqual(objc_bridge32_host_object(value), CFSTR("MW2 plist round trip")));
  assert(low[105] == kCFPropertyListXMLFormat_v1_0 && !low[101]);
  assert(low[104] == 0xfeedbeef && low[106] == 0xcafebabe);
  assert(low[100] == 0xabcddcba && low[102] == 0x12344321);
  a[0] = input; dispatch("_CFReadStreamClose", a);
  FILE *bad = fopen(file_path, "w"); assert(bad); fputs("invalid plist", bad); fclose(bad);
  a[0] = 0; a[1] = token(file_url);
  input = dispatch("_CFReadStreamCreateWithFile", a);
  a[0] = input; assert(dispatch("_CFReadStreamOpen", a));
  a[0] = 0; a[1] = input;
  assert(!dispatch("_CFPropertyListCreateWithStream", a));
  assert(low[101] && CFGetTypeID(objc_bridge32_host_object(low[101])) == CFErrorGetTypeID());
  assert(low[100] == 0xabcddcba && low[102] == 0x12344321);
  a[0] = input; dispatch("_CFReadStreamClose", a);
  CFRelease(file_url); unlink(file_path);
  CFRelease(data);
  CFRelease(url);
  for (unsigned i = 1; i <= count; ++i)
    CFRelease(objects[i]);
  assert(released == retained);
  munmap(low, 4096);
  puts("CFNetwork bridge: HTTP data, constants, run loops, timers, stream "
       "callbacks, file plists, error canaries and ownership PASS");
}
