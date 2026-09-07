#include "../src/cfnetwork_bridge.h"
#include <CFNetwork/CFNetwork.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

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
  CFRelease(data);
  CFRelease(url);
  for (unsigned i = 1; i <= count; ++i)
    CFRelease(objects[i]);
  assert(released == retained);
  munmap(low, 4096);
  puts("CFNetwork bridge: HTTP data, constants, run loops, timers, stream "
       "callbacks and ownership PASS");
}
