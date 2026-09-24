#include "cfnetwork_bridge.h"
#include "compat_runtime.h"
#include "objc_bridge.h"
#include <CFNetwork/CFNetwork.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* Contexts retain guest tokens, never host-sized copies of guest callbacks. */
struct callback32 {
  uint32_t info, retain, release, describe, callback;
};
static const void *retain_context(const void *raw) {
  struct callback32 *copy = malloc(sizeof(*copy));
  if (!copy)
    return NULL;
  *copy = *(const struct callback32 *)raw;
  if (copy->retain)
    copy->info = compat_runtime32_call(copy->retain, &copy->info, 1);
  return copy;
}
static void release_context(const void *raw) {
  const struct callback32 *context = raw;
  if (context->release)
    compat_runtime32_call(context->release, &context->info, 1);
  free((void *)context);
}
static CFStringRef describe_context(const void *raw) {
  const struct callback32 *context = raw;
  if (!context->describe)
    return NULL;
  CFStringRef description = objc_bridge32_host_object(
      compat_runtime32_call(context->describe, &context->info, 1));
  return description ? CFRetain(description) : NULL;
}
struct source_context32 {
  uint32_t version, info, retain, release, describe, equal, hash;
  uint32_t schedule, cancel, perform;
};
static const void *retain_source_context(const void *raw) {
  struct source_context32 *copy = malloc(sizeof(*copy));
  if (!copy) return NULL;
  *copy = *(const struct source_context32 *)raw;
  if (copy->retain)
    copy->info = compat_runtime32_call(copy->retain, &copy->info, 1);
  return copy;
}
static void release_source_context(const void *raw) {
  const struct source_context32 *source = raw;
  if (source->release)
    compat_runtime32_call(source->release, &source->info, 1);
  free((void *)source);
}
static CFStringRef describe_source_context(const void *raw) {
  const struct source_context32 *source = raw;
  if (!source->describe) return NULL;
  CFStringRef value = objc_bridge32_host_object(
      compat_runtime32_call(source->describe, &source->info, 1));
  return value ? CFRetain(value) : NULL;
}
static Boolean equal_source_context(const void *left, const void *right) {
  const struct source_context32 *a = left, *b = right;
  if (!a->equal) return a->info == b->info;
  uint32_t args[] = {a->info, b->info};
  return compat_runtime32_call(a->equal, args, 2) != 0;
}
static CFHashCode hash_source_context(const void *raw) {
  const struct source_context32 *source = raw;
  if (!source->hash) return source->info;
  return compat_runtime32_call(source->hash, &source->info, 1);
}
static void schedule_source_context(void *raw, CFRunLoopRef loop, CFRunLoopMode mode) {
  const struct source_context32 *source = raw;
  if (!source->schedule) return;
  uint32_t args[] = {source->info, objc_bridge32_guest_object((void *)loop),
                     objc_bridge32_guest_object((void *)mode)};
  compat_runtime32_call(source->schedule, args, 3);
}
static void cancel_source_context(void *raw, CFRunLoopRef loop, CFRunLoopMode mode) {
  const struct source_context32 *source = raw;
  if (!source->cancel) return;
  uint32_t args[] = {source->info, objc_bridge32_guest_object((void *)loop),
                     objc_bridge32_guest_object((void *)mode)};
  compat_runtime32_call(source->cancel, args, 3);
}
static void perform_source_context(void *raw) {
  const struct source_context32 *source = raw;
  if (source->perform)
    compat_runtime32_call(source->perform, &source->info, 1);
}
static void *stream_retain(void *raw) { return (void *)retain_context(raw); }
static void stream_release(void *raw) { release_context(raw); }
static CFStringRef stream_describe(void *raw) { return describe_context(raw); }
static void stream_callback(CFReadStreamRef stream, CFStreamEventType event,
                            void *raw) {
  if (getenv("LP32_TRACE_NETWORK")) {
    CFStreamError error = CFReadStreamGetError(stream);
    fprintf(stderr,
            "compat32: HTTP stream event=%lu error-domain=%ld error=%d\n",
            (unsigned long)event, (long)error.domain, (int)error.error);
  }
  const struct callback32 *context = raw;
  uint32_t args[] = {objc_bridge32_guest_object((void *)stream),
                     (uint32_t)event, context->info};
  compat_runtime32_call(context->callback, args, 3);
}
static void write_stream_callback(CFWriteStreamRef stream, CFStreamEventType event,
                                  void *raw) {
  const struct callback32 *context = raw;
  uint32_t args[] = {objc_bridge32_guest_object((void *)stream),
                     (uint32_t)event, context->info};
  compat_runtime32_call(context->callback, args, 3);
}
static void write_guest_stream_error(uint32_t address, CFStreamError error) {
  if (!address) return;
  int32_t guest[] = {(int32_t)error.domain, error.error};
  memcpy((void *)(uintptr_t)address, guest, sizeof(guest));
}
static uint32_t guest_stream_error(CFStreamError *error) {
  if (!error) return 0;
  uint32_t address = compat_runtime32_allocate(8, 1);
  write_guest_stream_error(address, *error);
  return address;
}
static void net_service_callback(CFNetServiceRef service, CFStreamError *error,
                                 void *raw) {
  const struct callback32 *context = raw;
  uint32_t guest_error = guest_stream_error(error);
  uint32_t args[] = {objc_bridge32_guest_object((void *)service),
                     guest_error, context->info};
  compat_runtime32_call(context->callback, args, 3);
  if (guest_error) compat_runtime32_deallocate(guest_error);
}
static void net_service_browser_callback(CFNetServiceBrowserRef browser,
                                         CFOptionFlags flags, CFTypeRef item,
                                         CFStreamError *error, void *raw) {
  const struct callback32 *context = raw;
  uint32_t guest_error = guest_stream_error(error);
  uint32_t args[] = {objc_bridge32_guest_object((void *)browser),
                     (uint32_t)flags, objc_bridge32_guest_object((void *)item),
                     guest_error, context->info};
  compat_runtime32_call(context->callback, args, 5);
  if (guest_error) compat_runtime32_deallocate(guest_error);
}
static void proxy_configuration_callback(void *raw, CFArrayRef proxies,
                                         CFErrorRef error) {
  const struct callback32 *context = raw;
  uint32_t args[] = {context->info, objc_bridge32_guest_object((void *)proxies),
                     objc_bridge32_guest_object((void *)error)};
  compat_runtime32_call(context->callback, args, 3);
}
static void timer_callback(CFRunLoopTimerRef timer, void *raw) {
  const struct callback32 *context = raw;
  uint32_t args[] = {objc_bridge32_guest_object((void *)timer), context->info};
  compat_runtime32_call(context->callback, args, 2);
}
static uint32_t copied(CFTypeRef value) {
  uint32_t token = objc_bridge32_guest_object((void *)value);
  if (value)
    CFRelease(value);
  return token;
}
int cfnetwork_bridge32_dispatch(const char *name, const uint32_t *a,
                                uint64_t *out) {
#define IS(s) (!strcmp(name, s))
#define O(i) objc_bridge32_host_object(a[i])
#define P(i) ((void *)(uintptr_t)a[i])
  if (IS("_CFHTTPMessageCreateRequest")) {
    if (getenv("LP32_TRACE_NETWORK")) {
      char host[256] = {0};
      CFStringRef name = CFURLCopyHostName(O(2));
      if (name) {
        CFStringGetCString(name, host, sizeof(host), kCFStringEncodingUTF8);
        CFRelease(name);
      }
      /* Never log URL paths/queries, headers, or bodies: they may contain keys.
       */
      fprintf(stderr, "compat32: HTTP request host=%s\n", host);
    }
    *out = copied(CFHTTPMessageCreateRequest(NULL, O(1), O(2), O(3)));
  } else if (IS("_CFHTTPMessageCreateEmpty"))
    *out = copied(CFHTTPMessageCreateEmpty(NULL, a[1] != 0));
  else if (IS("_CFHTTPMessageSetBody")) {
    CFHTTPMessageSetBody(O(0), O(1));
    *out = 0;
  } else if (IS("_CFHTTPMessageSetHeaderFieldValue")) {
    CFHTTPMessageSetHeaderFieldValue(O(0), O(1), O(2));
    *out = 0;
  } else if (IS("_CFHTTPMessageAppendBytes"))
    *out = CFHTTPMessageAppendBytes(O(0), P(1), a[2]);
  else if (IS("_CFHTTPMessageCopyAllHeaderFields"))
    *out = copied(CFHTTPMessageCopyAllHeaderFields(O(0)));
  else if (IS("_CFHTTPMessageCopyBody"))
    *out = copied(CFHTTPMessageCopyBody(O(0)));
  else if (IS("_CFHTTPMessageCopyHeaderFieldValue"))
    *out = copied(CFHTTPMessageCopyHeaderFieldValue(O(0), O(1)));
  else if (IS("_CFHTTPMessageGetResponseStatusCode")) {
    *out = (uint32_t)CFHTTPMessageGetResponseStatusCode(O(0));
    if (getenv("LP32_TRACE_NETWORK"))
      fprintf(stderr, "compat32: HTTP response status=%u\n", (uint32_t)*out);
  } else if (IS("_CFReadStreamCreateForHTTPRequest"))
    *out = copied(CFReadStreamCreateForHTTPRequest(NULL, O(1)));
  else if (IS("_CFReadStreamCreateWithFile"))
    *out = copied(CFReadStreamCreateWithFile(NULL, O(1)));
  else if (IS("_CFReadStreamHasBytesAvailable"))
    *out = CFReadStreamHasBytesAvailable(O(0));
  else if (IS("_CFReadStreamOpen"))
    *out = CFReadStreamOpen(O(0));
  else if (IS("_CFReadStreamClose")) {
    CFReadStreamClose(O(0));
    *out = 0;
  } else if (IS("_CFReadStreamRead"))
    *out = (uint32_t)CFReadStreamRead(O(0), P(1), (int32_t)a[2]);
  else if (IS("_CFReadStreamCopyProperty"))
    *out = copied(CFReadStreamCopyProperty(O(0), O(1)));
  else if (IS("_CFReadStreamSetProperty"))
    *out = CFReadStreamSetProperty(O(0), O(1), O(2));
  else if (IS("_CFReadStreamScheduleWithRunLoop")) {
    CFReadStreamScheduleWithRunLoop(O(0), O(1), O(2));
    *out = 0;
  } else if (IS("_CFReadStreamUnscheduleFromRunLoop")) {
    CFReadStreamUnscheduleFromRunLoop(O(0), O(1), O(2));
    *out = 0;
  } else if (IS("_CFReadStreamSetClient")) {
    const uint32_t *guest = P(3);
    if (!a[2] || !a[1]) {
      *out = CFReadStreamSetClient(O(0), 0, NULL, NULL);
      return 1;
    }
    if (!guest || guest[0]) {
      *out = 0;
      return 1;
    }
    struct callback32 callback = {guest[1], guest[2], guest[3], guest[4], a[2]};
    CFStreamClientContext context = {0, &callback, stream_retain,
                                     stream_release, stream_describe};
    *out = CFReadStreamSetClient(O(0), a[1], stream_callback, &context);
  } else if (IS("_CFWriteStreamCreateWithFile"))
    *out = copied(CFWriteStreamCreateWithFile(NULL, O(1)));
  else if (IS("_CFWriteStreamOpen"))
    *out = CFWriteStreamOpen(O(0));
  else if (IS("_CFWriteStreamClose")) {
    CFWriteStreamClose(O(0)); *out = 0;
  } else if (IS("_CFWriteStreamWrite"))
    *out = (uint32_t)CFWriteStreamWrite(O(0), P(1), (CFIndex)(int32_t)a[2]);
  else if (IS("_CFWriteStreamCanAcceptBytes"))
    *out = CFWriteStreamCanAcceptBytes(O(0));
  else if (IS("_CFWriteStreamGetStatus"))
    *out = CFWriteStreamGetStatus(O(0));
  else if (IS("_CFWriteStreamCopyError"))
    *out = copied(CFWriteStreamCopyError(O(0)));
  else if (IS("_CFWriteStreamSetProperty"))
    *out = CFWriteStreamSetProperty(O(0), O(1), O(2));
  else if (IS("_CFWriteStreamScheduleWithRunLoop")) {
    CFWriteStreamScheduleWithRunLoop(O(0), O(1), O(2)); *out = 0;
  } else if (IS("_CFWriteStreamUnscheduleFromRunLoop")) {
    CFWriteStreamUnscheduleFromRunLoop(O(0), O(1), O(2)); *out = 0;
  } else if (IS("_CFWriteStreamSetClient")) {
    const uint32_t *guest = P(3);
    if (!a[2] || !a[1]) {
      *out = CFWriteStreamSetClient(O(0), 0, NULL, NULL);
      return 1;
    }
    if (!guest || guest[0]) { *out = 0; return 1; }
    struct callback32 callback = {guest[1], guest[2], guest[3], guest[4], a[2]};
    CFStreamClientContext context = {0, &callback, stream_retain,
                                     stream_release, stream_describe};
    *out = CFWriteStreamSetClient(O(0), a[1], write_stream_callback, &context);
  } else if (IS("_CFStreamCreatePairWithSocketToHost")) {
    CFReadStreamRef read = NULL; CFWriteStreamRef write = NULL;
    CFStreamCreatePairWithSocketToHost(NULL, O(1), a[2],
        a[3] ? &read : NULL, a[4] ? &write : NULL);
    if (a[3]) *(uint32_t *)P(3) = copied(read);
    if (a[4]) *(uint32_t *)P(4) = copied(write);
    *out = 0;
  } else if (IS("_CFStreamCreatePairWithSocketToNetService")) {
    CFReadStreamRef read = NULL; CFWriteStreamRef write = NULL;
    CFStreamCreatePairWithSocketToNetService(NULL, O(1),
        a[2] ? &read : NULL, a[3] ? &write : NULL);
    if (a[2]) *(uint32_t *)P(2) = copied(read);
    if (a[3]) *(uint32_t *)P(3) = copied(write);
    *out = 0;
  } else if (IS("_CFNetServiceBrowserCreate")) {
    const uint32_t *guest = P(2);
    if (!a[1] || !guest || guest[0]) { *out = 0; return 1; }
    struct callback32 callback = {guest[1], guest[2], guest[3], guest[4], a[1]};
    CFNetServiceClientContext context = {0, &callback, retain_context,
                                         release_context, describe_context};
    *out = copied(CFNetServiceBrowserCreate(NULL,
        net_service_browser_callback, &context));
  } else if (IS("_CFNetServiceBrowserInvalidate")) {
    CFNetServiceBrowserInvalidate(O(0)); *out = 0;
  } else if (IS("_CFNetServiceBrowserScheduleWithRunLoop")) {
    CFNetServiceBrowserScheduleWithRunLoop(O(0), O(1), O(2)); *out = 0;
  } else if (IS("_CFNetServiceBrowserUnscheduleFromRunLoop")) {
    CFNetServiceBrowserUnscheduleFromRunLoop(O(0), O(1), O(2)); *out = 0;
  } else if (IS("_CFNetServiceBrowserSearchForDomains")) {
    CFStreamError error = {0};
    *out = CFNetServiceBrowserSearchForDomains(O(0), a[1] != 0,
                                                a[2] ? &error : NULL);
    write_guest_stream_error(a[2], error);
  } else if (IS("_CFNetServiceBrowserSearchForServices")) {
    CFStreamError error = {0};
    *out = CFNetServiceBrowserSearchForServices(O(0), O(1), O(2),
                                                 a[3] ? &error : NULL);
    write_guest_stream_error(a[3], error);
  } else if (IS("_CFNetServiceBrowserStopSearch")) {
    CFStreamError error = {0};
    CFNetServiceBrowserStopSearch(O(0), a[1] ? &error : NULL);
    *out = 0;
    write_guest_stream_error(a[1], error);
  } else if (IS("_CFNetServiceCancel")) {
    CFNetServiceCancel(O(0)); *out = 0;
  } else if (IS("_CFNetServiceCreateDictionaryWithTXTData"))
    *out = copied(CFNetServiceCreateDictionaryWithTXTData(NULL, O(1)));
  else if (IS("_CFNetServiceGetName"))
    *out = objc_bridge32_guest_object((void *)CFNetServiceGetName(O(0)));
  else if (IS("_CFNetServiceGetTXTData"))
    *out = objc_bridge32_guest_object((void *)CFNetServiceGetTXTData(O(0)));
  else if (IS("_CFNetServiceSetClient")) {
    const uint32_t *guest = P(2);
    if (!a[1] || !guest) {
      *out = CFNetServiceSetClient(O(0), NULL, NULL); return 1;
    }
    if (guest[0]) { *out = 0; return 1; }
    struct callback32 callback = {guest[1], guest[2], guest[3], guest[4], a[1]};
    CFNetServiceClientContext context = {0, &callback, retain_context,
                                         release_context, describe_context};
    *out = CFNetServiceSetClient(O(0), net_service_callback, &context);
  } else if (IS("_CFNetServiceUnscheduleFromRunLoop")) {
    CFNetServiceUnscheduleFromRunLoop(O(0), O(1), O(2)); *out = 0;
  } else if (IS("_CFNetworkExecuteProxyAutoConfigurationURL")) {
    const uint32_t *guest = P(3);
    if (!a[2] || !guest || guest[0]) { *out = 0; return 1; }
    struct callback32 callback = {guest[1], guest[2], guest[3], guest[4], a[2]};
    CFStreamClientContext context = {0, &callback, stream_retain,
                                     stream_release, stream_describe};
    *out = copied(CFNetworkExecuteProxyAutoConfigurationURL(O(0), O(1),
        proxy_configuration_callback, &context));
  } else if (IS("_SCDynamicStoreCopyProxies"))
    *out = copied(SCDynamicStoreCopyProxies(O(0)));
  else if (IS("_CFNetworkCopySystemProxySettings"))
    *out = copied(CFNetworkCopySystemProxySettings());
  else if (IS("_CFNetworkCopyProxiesForURL"))
    *out = copied(CFNetworkCopyProxiesForURL(O(0), O(1)));
  else if (IS("_CFRunLoopGetMain"))
    *out = objc_bridge32_guest_object(CFRunLoopGetMain());
  else if (IS("_CFRunLoopGetCurrent"))
    *out = objc_bridge32_guest_object(CFRunLoopGetCurrent());
  else if (IS("_CFRunLoopRun")) {
    CFRunLoopRun();
    *out = 0;
  } else if (IS("_CFRunLoopStop")) {
    CFRunLoopStop(O(0));
    *out = 0;
  } else if (IS("_CFRunLoopWakeUp")) {
    CFRunLoopWakeUp(O(0));
    *out = 0;
  } else if (IS("_CFRunLoopAddSource")) {
    if (O(0) && O(1))
      CFRunLoopAddSource(O(0), O(1), O(2));
    *out = 0;
  } else if (IS("_CFRunLoopRemoveSource")) {
    if (O(0) && O(1))
      CFRunLoopRemoveSource(O(0), O(1), O(2));
    *out = 0;
  } else if (IS("_CFRunLoopContainsSource"))
    *out = O(0) && O(1) && CFRunLoopContainsSource(O(0), O(1), O(2));
  else if (IS("_CFRunLoopSourceInvalidate")) {
    CFRunLoopSourceInvalidate(O(0));
    *out = 0;
  } else if (IS("_CFRunLoopSourceSignal")) {
    CFRunLoopSourceSignal(O(0));
    *out = 0;
  } else if (IS("_CFRunLoopSourceCreate")) {
    const struct source_context32 *guest = P(2);
    if (!guest || guest->version) { *out = 0; return 1; }
    CFRunLoopSourceContext context = {
        0, (void *)guest, retain_source_context, release_source_context,
        describe_source_context, equal_source_context, hash_source_context,
        schedule_source_context, cancel_source_context, perform_source_context};
    *out = copied(CFRunLoopSourceCreate(NULL, (CFIndex)(int32_t)a[1], &context));
  } else if (IS("_CFRunLoopAddTimer")) {
    CFRunLoopAddTimer(O(0), O(1), O(2));
    *out = 0;
  } else if (IS("_CFRunLoopTimerInvalidate")) {
    CFRunLoopTimerInvalidate(O(0));
    *out = 0;
  } else if (IS("_CFRunLoopTimerSetNextFireDate")) {
    double date;
    memcpy(&date, a + 1, 8);
    CFRunLoopTimerSetNextFireDate(O(0), date);
    *out = 0;
  } else if (IS("_CFRunLoopGetNextTimerFireDate")) {
    *out=compat_runtime32_return_double(CFRunLoopGetNextTimerFireDate(O(0),O(1)));
  } else if (IS("_CFRunLoopTimerGetContext")) {
    CFRunLoopTimerContext context; CFRunLoopTimerGetContext(O(0), &context);
    const struct callback32 *c = context.info;
    uint32_t guest[] = {0, c->info, c->retain, c->release, c->describe};
    memcpy(P(1), guest, sizeof(guest)); *out = 0;
  } else if (IS("_CFRunLoopRemoveTimer")) {
    CFRunLoopRemoveTimer(O(0), O(1), O(2)); *out = 0;
  } else if (IS("_CFRunLoopRunInMode")) {
    double seconds; memcpy(&seconds, a+1, 8);
    *out = CFRunLoopRunInMode(O(0), seconds, a[3] != 0);
  } else if (IS("_CFRunLoopTimerCreate")) {
    double date, interval;
    memcpy(&date, a + 1, 8);
    memcpy(&interval, a + 3, 8);
    const uint32_t *guest = P(8);
    if (guest && guest[0]) {
      *out = 0;
      return 1;
    }
    struct callback32 callback = {0, 0, 0, 0, a[7]};
    if (guest) {
      callback.info = guest[1];
      callback.retain = guest[2];
      callback.release = guest[3];
      callback.describe = guest[4];
    }
    CFRunLoopTimerContext context = {0, &callback, retain_context,
                                     release_context, describe_context};
    *out = copied(CFRunLoopTimerCreate(
        NULL, date, interval, a[5], (int32_t)a[6], timer_callback, &context));
  } else
    return 0;
  return 1;
}
uint32_t cfnetwork_bridge32_pointer_import(const char *name) {
  if (strcmp(name, "_kCFRunLoopCommonModes") == 0)
    return objc_bridge32_guest_object((void *)kCFRunLoopCommonModes);
  if (strncmp(name, "_kCFHTTP", 8) &&
      strncmp(name, "_kCFStreamPropertyHTTP", 22) &&
      strncmp(name, "_kCFProxy", 9))
    return 0;
  /* Restrict dynamic imports to CFNetwork's CFString constant families. */
  void **value = dlsym(RTLD_DEFAULT, name + 1);
  return value ? objc_bridge32_guest_object(*value) : 0;
}
