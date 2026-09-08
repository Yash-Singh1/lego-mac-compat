#include <Carbon/Carbon.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <expat.h>
#include <unistd.h>
#include <AudioToolbox/AudioToolbox.h>
/* QuickDraw declarations removed from current SDK headers. */
extern OSErr DMGetDeskRegion(RgnHandle *);
extern Rect *GetRegionBounds(RgnHandle, Rect *);
extern RgnHandle NewRgn(void);
extern void DisposeRgn(RgnHandle);

struct converter_test_input { int16_t samples[4]; unsigned consumed; };
static OSStatus converter_test_callback(AudioConverterRef converter, UInt32 *bytes, void **data, void *opaque)
{
    (void)converter;
    struct converter_test_input *input = opaque;
    unsigned available = sizeof(input->samples) - input->consumed;
    if (*bytes > available) *bytes = available;
    *data = (char *)input->samples + input->consumed;
    input->consumed += *bytes;
    return 0;
}
static int check_converter(void)
{
    AudioStreamBasicDescription input = {8000, kAudioFormatLinearPCM,
        kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, 2, 1, 2, 1, 16, 0};
    AudioStreamBasicDescription output = {8000, kAudioFormatLinearPCM,
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 4, 1, 4, 1, 32, 0};
    struct { AudioConverterRef converter; uint32_t guard; } object = {NULL, 0x13572468};
    if (AudioConverterNew(&input, &output, &object.converter) || !object.converter || object.guard != 0x13572468) return -1;
    struct converter_test_input samples = {{-32768, -16384, 0, 16384}, 0};
    float converted[4] = {9, 9, 9, 9};
    UInt32 bytes = sizeof(converted);
    OSStatus status = AudioConverterFillBuffer(object.converter, converter_test_callback, &samples, &bytes, converted);
    AudioConverterDispose(object.converter);
    return status || bytes != sizeof(converted) || converted[0] != -1.0f ||
        converted[1] != -0.5f || converted[2] != 0 || converted[3] != 0.5f ? -2 : 0;
}

static volatile int async_read_done;
static void async_read_callback(ParmBlkPtr pb)
{
    FSForkIOParam *read = (FSForkIOParam *)pb;
    async_read_done = read->ioResult == eofErr && read->actualCount == 8 ? 1 : -1;
}

static unsigned xml_allocations, xml_frees, xml_starts, xml_ends, xml_text_length;
static int xml_failed;
static void *xml_malloc(size_t size) { ++xml_allocations; return malloc(size); }
static void *xml_realloc(void *pointer, size_t size) { return realloc(pointer, size); }
static void xml_free(void *pointer) { if (pointer) ++xml_frees; free(pointer); }
static void xml_start(void *user, const char *name, const char **attributes)
{
    if (user != &xml_failed || strcmp(name, "root") || !attributes || !attributes[0] || !attributes[1] ||
        strcmp(attributes[0], "a") || strcmp(attributes[1], "value") || attributes[2]) xml_failed = 1;
    ++xml_starts;
}
static void xml_end(void *user, const char *name) {
    if (user != &xml_failed || strcmp(name, "root")) xml_failed = 1;
    ++xml_ends;
}
static void xml_text(void *user, const char *text, int length) {
    if (user != &xml_failed || length < 0 || xml_text_length + length > 5 ||
        memcmp(text, "hello" + xml_text_length, length)) xml_failed = 1;
    xml_text_length += length;
}
static const char *xml_parent_name, *xml_parent_attribute;
static void xml_nested_start(void *user, const char *name, const char **attributes) {
    if (!strcmp(name, "root")) { xml_parent_name = name; xml_parent_attribute = attributes[1]; }
    else if (strcmp(xml_parent_name, "root") || strcmp(xml_parent_attribute, "kept")) xml_failed = 1;
}
static int check_xml(void)
{
    XML_Memory_Handling_Suite memory = {xml_malloc, xml_realloc, xml_free};
    xml_allocations = xml_frees = xml_starts = xml_ends = xml_text_length = 0; xml_failed = 0;
    XML_Parser parser = XML_ParserCreate_MM(NULL, &memory, NULL);
    if (!parser) return -1;
    XML_SetUserData(parser, &xml_failed);
    XML_SetElementHandler(parser, xml_start, xml_end); XML_SetCharacterDataHandler(parser, xml_text);
    const char *first = "<root a='value'>he", *second = "llo</root>";
    if (XML_Parse(parser, first, strlen(first), 0) != XML_STATUS_OK ||
        XML_Parse(parser, second, strlen(second), 1) != XML_STATUS_OK ||
        XML_GetErrorCode(parser) != XML_ERROR_NONE || XML_GetCurrentLineNumber(parser) != 1) xml_failed = 1;
    XML_ParserFree(parser);
    if (xml_failed || xml_starts != 1 || xml_ends != 1 || xml_text_length != 5 || !xml_allocations || !xml_frees) return -2;
    parser = XML_ParserCreate_MM(NULL, NULL, NULL);
    if (!parser) return -4;
    XML_SetElementHandler(parser, xml_nested_start, NULL);
    const char *nested = "<root a='kept'><leaf a='next'/></root>";
    if (XML_Parse(parser, nested, strlen(nested), 1) != XML_STATUS_OK || xml_failed ||
        strcmp(xml_parent_name, "root") || strcmp(xml_parent_attribute, "kept")) return -5;
    XML_ParserFree(parser);
    parser = XML_ParserCreate_MM(NULL, NULL, NULL);
    if (!parser || XML_Parse(parser, "<root>", 6, 1) != XML_STATUS_ERROR || !XML_ErrorString(XML_GetErrorCode(parser))) return -3;
    XML_ParserFree(parser); return 0;
}

_Static_assert(sizeof(FSCatalogInfo) == 144, "guest catalog ABI");
_Static_assert(sizeof(FSRefParam) == 72, "guest parameter-block ABI");
_Static_assert(sizeof(HVolumeParam) == 122, "guest volume ABI");
_Static_assert(sizeof(ProcessInfoRec) == 60, "guest process ABI");
_Static_assert(sizeof(FSVolumeInfo) == 126, "guest volume-info ABI");
_Static_assert(sizeof(GDevice) == 62, "guest display device ABI");
_Static_assert(sizeof(PixMap) == 50, "guest pixmap ABI");
extern GDHandle GetMainDevice(void);
extern GDHandle GetDeviceList(void);
extern GDHandle GetNextDevice(GDHandle);
extern OSErr DMGetDisplayIDByGDevice(GDHandle, uint32_t *, Boolean);
extern OSErr DMGetGDeviceByDisplayID(uint32_t, GDHandle *, Boolean);
extern Boolean TestDeviceAttribute(GDHandle, short);

static int check_displays(void)
{
    GDHandle main = GetMainDevice();
    if (!main || !*main || !(*main)->gdPMap || !*(*main)->gdPMap ||
        (*(*main)->gdPMap)->pixelSize != 32 || !TestDeviceAttribute(main, 11)) return -1;
    unsigned count = 0;
    for (GDHandle device = GetDeviceList(); device; device = GetNextDevice(device)) {
        struct { uint32_t id, guard; } result = {0, 0x76543210};
        struct { GDHandle handle; uint32_t guard; } recovered = {NULL, 0x87654321};
        if (++count > 32 || !TestDeviceAttribute(device, 13) || !TestDeviceAttribute(device, 15) ||
            DMGetDisplayIDByGDevice(device, &result.id, false) || !result.id || result.guard != 0x76543210 ||
            DMGetGDeviceByDisplayID(result.id, &recovered.handle, false) || recovered.handle != device || recovered.guard != 0x87654321) return -2;
    }
    return count ? 0 : -3;
}

static int handler_calls;
static OSStatus inner_handler(EventHandlerCallRef call, EventRef event, void *user)
{
    (void)call;
    struct { UInt32 size, guard; } actual = {0, 0xcafebabe};
    UInt32 value = 0;
    if (GetEventParameter(event, 'valu', typeUInt32, NULL, 4, &actual.size, &value) ||
        actual.size != 4 || actual.guard != 0xcafebabe || value != 0x12345678 || user != (void *)7)
        return -1;
    ++handler_calls;
    return noErr;
}
static OSStatus outer_handler(EventHandlerCallRef call, EventRef event, void *user)
{
    (void)user;
    if (CallNextEventHandler(call, event)) return -1;
    ++handler_calls;
    return noErr;
}

struct worker_data { MPEventID event; TaskStorageIndex storage; volatile int failed; };
static OSStatus worker(void *opaque)
{
    struct worker_data *data = opaque;
    if (MPGetTaskStorageValue(data->storage) != NULL || MPSetTaskStorageValue(data->storage, (void *)0x5678)) data->failed = 1;
    MPSetEvent(data->event, 4);
    return noErr;
}

static EventLoopTimerRef expected_timer;
static unsigned timer_calls;
static void timer_callback(EventLoopTimerRef timer, void *user)
{
    if (timer == expected_timer && user == (void *)0x2468) ++timer_calls;
    QuitEventLoop(GetCurrentEventLoop());
}
static int check_timer(void)
{
    struct { EventLoopTimerRef timer; uint32_t guard; } out = {NULL, 0x12344321};
    if (InstallEventLoopTimer(GetCurrentEventLoop(), .002, kEventDurationForever,
        timer_callback, (void *)0x2468, &out.timer) || !out.timer || out.guard != 0x12344321) return -1;
    expected_timer = out.timer; timer_calls = 0;
    OSStatus status = RunCurrentEventLoop(.05);
    if (RemoveEventLoopTimer(out.timer) || status != eventLoopQuitErr || timer_calls != 1) return -2;
    RunCurrentEventLoop(.003);
    return timer_calls == 1 ? 0 : -3;
}

static int check_queue(void)
{
    EventRef event = NULL, received = NULL;
    EventTypeSpec type = {'tstQ', 7}; uint32_t value = 0x12344321, recovered = 0;
    if (CreateEvent(NULL, type.eventClass, type.eventKind, 1, 0, &event) ||
        SetEventParameter(event, 'valu', typeUInt32, 4, &value) ||
        PostEventToQueue(GetCurrentEventQueue(), event, kEventPriorityStandard)) return -1;
    ReleaseEvent(event);
    if (ReceiveNextEvent(1, &type, .02, true, &received) || !received ||
        GetEventParameter(received, 'valu', typeUInt32, NULL, 4, NULL, &recovered) || recovered != value) return -2;
    ReleaseEvent(received);
    return 0;
}

static int check_iterator(const FSRef *file)
{
    FSRef parent;
    if (FSGetCatalogInfo(file, 0, NULL, NULL, NULL, &parent)) return -1;
    struct { FSIterator iterator; uint32_t guard; } opened = {NULL, 0x24681357};
    if (FSOpenIterator(&parent, kFSIterateFlat, &opened.iterator) || !opened.iterator || opened.guard != 0x24681357) return -2;
    int found = 0, failed = 0;
    for (unsigned batch = 0; batch < 128; ++batch) {
        struct { FSCatalogInfo infos[2]; uint32_t guard; } records;
        struct { ItemCount value; uint32_t guard; } count = {0, 0x13572468};
        FSRef refs[2]; records.guard = 0x98765432;
        OSErr status = FSGetCatalogInfoBulk(opened.iterator, 2, &count.value, NULL,
            kFSCatInfoNodeFlags | kFSCatInfoDataSizes, records.infos, refs, NULL, NULL);
        if ((status && status != errFSNoMoreItems) || count.value > 2 || count.guard != 0x13572468 || records.guard != 0x98765432) { failed = 1; break; }
        for (unsigned i = 0; i < count.value; ++i) if (!FSCompareFSRefs(refs + i, file)) {
            if (records.infos[i].dataLogicalSize != 8) failed = 1;
            ++found;
        }
        if (status == errFSNoMoreItems) break;
    }
    if (FSCloseIterator(opened.iterator)) failed = 1;
    return !failed && found == 1 ? 0 : -3;
}

static unsigned native_class_serial, native_constructs, native_initializes, native_destructs;
struct native_instance { HIObjectRef view; uint32_t value; };
static OSStatus native_class_handler(EventHandlerCallRef call, EventRef event, void *opaque)
{
    if (GetEventClass(event) != kEventClassHIObject) return eventNotHandledErr;
    switch (GetEventKind(event)) {
    case kEventHIObjectConstruct: {
        struct native_instance *instance = calloc(1, sizeof(*instance));
        struct { ByteCount size; uint32_t guard; } actual = {0, 0xaabbccdd};
        if (!instance || opaque != (void *)0x1234 || call ||
            GetEventParameter(event, kEventParamHIObjectInstance, typeHIObjectRef, NULL,
                sizeof(instance->view), &actual.size, &instance->view) ||
            actual.size != sizeof(instance->view) || actual.guard != 0xaabbccdd || !instance->view) return -1;
        ++native_constructs;
        return SetEventParameter(event, kEventParamHIObjectInstance, typeVoidPtr, sizeof(instance), &instance);
    }
    case kEventHIObjectInitialize: {
        struct native_instance *instance = opaque;
        if (!instance || CallNextEventHandler(call, event) ||
            GetEventParameter(event, 'valu', typeUInt32, NULL, 4, NULL, &instance->value) || instance->value != 0x87654321) return -1;
        ++native_initializes;
        return noErr;
    }
    case kEventHIObjectDestruct: {
        struct native_instance *instance = opaque;
        if (!instance || instance->value != 0x87654321) return -1;
        ++native_destructs; free(instance); return noErr;
    }
    default: return eventNotHandledErr;
    }
}

static int check_native_class(void)
{
    CFStringRef name = CFStringCreateWithFormat(NULL, NULL, CFSTR("org.32bitgoofy.CarbonFixture.%u"), native_class_serial++);
    EventTypeSpec types[] = {{kEventClassHIObject, kEventHIObjectConstruct},
        {kEventClassHIObject, kEventHIObjectInitialize}, {kEventClassHIObject, kEventHIObjectDestruct}};
    native_constructs = native_initializes = native_destructs = 0;
    if (HIObjectRegisterSubclass(name, CFSTR("com.apple.hiview"), 0, native_class_handler, 3, types, (void *)0x1234, NULL)) return -1;
    EventRef event; HIObjectRef object; uint32_t value = 0x87654321;
    if (CreateEvent(NULL, kEventClassHIObject, kEventHIObjectInitialize, 1, 0, &event) ||
        SetEventParameter(event, 'valu', typeUInt32, 4, &value) || HIObjectCreate(name, event, &object)) return -2;
    struct { HIObjectRef view; uint32_t guard; } control = {NULL, 0x13572468};
    UInt32 actual = 0;
    if (SetEventParameter(event, kEventParamDirectObject, typeControlRef, sizeof(object), &object) ||
        GetEventParameter(event, kEventParamDirectObject, typeControlRef, NULL, sizeof(control.view), &actual, &control.view) ||
        actual != 4 || control.view != object || control.guard != 0x13572468) return -5;
    ReleaseEvent(event); CFRelease(name);
    if (native_constructs != 1 || native_initializes != 1 || native_destructs) return -3;
    CFRelease(object);
    return native_destructs == 1 ? 0 : -4;
}

int check_carbon(void)
{
    if (check_converter()) return -107;
    struct { RgnHandle region; uint32_t guard; } desktop = {NULL, 0x13572468};
    struct { Rect bounds; uint32_t guard; } desktop_bounds = {{0}, 0x87654321};
    if (DMGetDeskRegion(&desktop.region) || !desktop.region || desktop.guard != 0x13572468 ||
        GetRegionBounds(desktop.region, &desktop_bounds.bounds) != &desktop_bounds.bounds ||
        desktop_bounds.guard != 0x87654321 || desktop_bounds.bounds.right <= desktop_bounds.bounds.left ||
        desktop_bounds.bounds.bottom <= desktop_bounds.bounds.top) return -108;
    RgnHandle empty = NewRgn();
    if (!empty) return -109;
    Rect empty_bounds;
    GetRegionBounds(empty, &empty_bounds);
    DisposeRgn(empty);
    if (empty_bounds.top || empty_bounds.left || empty_bounds.bottom || empty_bounds.right) return -110;
    if (check_timer()) return -100;
    if (check_displays()) return -101;
    if (check_queue()) return -103;
    if (check_xml()) return -105;
    const char *path = getenv("LP32_CARBON_FIXTURE_FILE");
    FSRef ref, ref_again;
    if (!path || FSPathMakeRef((const UInt8 *)path, &ref, NULL)) return -1;
    if (check_iterator(&ref)) return -104;
    struct { FSVolumeInfo info; uint32_t guard; } volume_info = {0};
    struct { FSVolumeRefNum volume; uint16_t guard; } volume_ref = {0, 0xabba};
    volume_info.guard = 0x13572468;
    if (FSGetVolumeInfo(0, 1, &volume_ref.volume, kFSVolInfoSizes, &volume_info.info, NULL, NULL) ||
        !volume_ref.volume || volume_ref.guard != 0xabba || volume_info.guard != 0x13572468 ||
        !volume_info.info.totalBytes || volume_info.info.freeBytes > volume_info.info.totalBytes) return -102;
    struct { FSCatalogInfo info; uint32_t guard; } catalog;
    memset(&catalog, 0, sizeof(catalog)); catalog.guard = 0x12345678;
    if (FSGetCatalogInfo(&ref, kFSCatInfoVolume | kFSCatInfoNodeID | kFSCatInfoDataSizes | kFSCatInfoPermissions,
                        &catalog.info, NULL, NULL, NULL) || catalog.guard != 0x12345678 || catalog.info.dataLogicalSize != 8) return -2;
    FSRefParam pb;
    memset(&pb, 0, sizeof(pb));
    pb.ioVRefNum = catalog.info.volume; pb.ioDirID = catalog.info.nodeID; pb.newRef = &ref_again;
    if (PBMakeFSRefSync(&pb) || pb.ioResult || FSCompareFSRefs(&ref, &ref_again)) return -3;
    FSIORefNum file;
    if (FSOpenFork(&ref, 0, NULL, fsRdPerm, &file)) return -4;
    struct { ByteCount size; uint32_t guard; } count = {0, 0xdeadbeef};
    char bytes[10];
    OSErr status = FSReadFork(file, fsFromStart, 0, sizeof(bytes), bytes, &count.size);
    if (status != eofErr || count.size != 8 || count.guard != 0xdeadbeef || memcmp(bytes, "fixture\n", 8)) return -5;
    struct { FSForkIOParam read; uint32_t guard; } async = {0};
    memset(bytes, 0, sizeof(bytes));
    async.guard = 0x13572468;
    async.read.forkRefNum = file;
    async.read.buffer = bytes;
    async.read.requestCount = sizeof(bytes);
    async.read.positionMode = fsFromStart;
    async.read.ioCompletion = async_read_callback;
    async_read_done = 0;
    PBReadForkAsync(&async.read);
    for (unsigned i = 0; i < 2000 && !async_read_done; ++i) usleep(1000);
    if (async_read_done != 1 || async.guard != 0x13572468 || memcmp(bytes, "fixture\n", 8)) return -106;
    FSCloseFork(file);

    struct { HVolumeParam info; uint32_t guard; } volume;
    memset(&volume, 0, sizeof(volume)); volume.guard = 0xabcdef12;
    volume.info.ioVRefNum = catalog.info.volume;
    if (PBHGetVInfoSync((HParmBlkPtr)&volume.info) || volume.info.ioResult ||
        volume.guard != 0xabcdef12 || !volume.info.ioVNmAlBlks ||
        volume.info.ioVFrBlk > volume.info.ioVNmAlBlks || !volume.info.ioVAlBlkSiz) return -15;
    ProcessSerialNumber psn;
    struct { ProcessInfoRec info; uint32_t guard; } process;
    Str255 process_name;
    memset(&process, 0, sizeof(process)); process.guard = 0xdeadbeef;
    process.info.processInfoLength = sizeof(ProcessInfoRec); process.info.processName = process_name;
    if (GetCurrentProcess(&psn) || GetProcessInformation(&psn, &process.info) ||
        process.guard != 0xdeadbeef || !process_name[0] || process.info.processNumber.lowLongOfPSN != psn.lowLongOfPSN) return -16;

    EventRef event;
    EventHandlerRef inner, outer;
    EventTypeSpec type = {'TEST', 42};
    handler_calls = 0;
    if (InstallEventHandler(GetApplicationEventTarget(), inner_handler, 1, &type, (void *)7, &inner) ||
        InstallEventHandler(GetApplicationEventTarget(), outer_handler, 1, &type, NULL, &outer) ||
        CreateEvent(NULL, type.eventClass, type.eventKind, 10.5, 0, &event)) return -6;
    UInt32 value = 0x12345678;
    if (SetEventParameter(event, 'valu', typeUInt32, sizeof(value), &value) ||
        GetEventClass(event) != type.eventClass || GetEventKind(event) != type.eventKind || GetEventTime(event) != 10.5 ||
        SendEventToEventTarget(event, GetApplicationEventTarget()) || handler_calls != 2) return -7;
    RetainEvent(event);
    if (GetEventRetainCount(event) != 2) return -8;
    ReleaseEvent(event); ReleaseEvent(event);
    RemoveEventHandler(outer); RemoveEventHandler(inner);
    int native_status = check_native_class();
    if (native_status) return -20 + native_status;

    TaskStorageIndex storage;
    MPEventID ready;
    if (MPAllocateTaskStorageIndex(&storage) || MPSetTaskStorageValue(storage, (void *)0x1234) || MPCreateEvent(&ready)) return -9;
    MPEventFlags flags = 0;
    if (!MPWaitForEvent(ready, &flags, kDurationImmediate) || MPSetEvent(ready, 3) || MPWaitForEvent(ready, &flags, 1000) || flags != 3) return -10;
    struct worker_data data = {ready, storage, 0};
    MPTaskID task;
    if (MPCreateTask(worker, &data, 0, NULL, NULL, NULL, 0, &task) ||
        MPWaitForEvent(ready, &flags, 5000) || flags != 4 || data.failed || MPGetTaskStorageValue(storage) != (void *)0x1234) return -11;
    MPDeleteEvent(ready);

    CFStringRef formatted = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ %d %llu %.2f"), CFSTR("value"), -12, 4294967299ULL, 1.25);
    char formatted_bytes[96];
    if (!formatted || !CFStringGetCString(formatted, formatted_bytes, sizeof(formatted_bytes), kCFStringEncodingUTF8) ||
        strcmp(formatted_bytes, "value -12 4294967299 1.25")) return -13;
    CFRelease(formatted);
    formatted = CFStringCreateWithFormat(NULL, NULL, CFSTR("%2$@ %1$d"), -7, CFSTR("positional"));
    if (!formatted || !CFStringGetCString(formatted, formatted_bytes, sizeof(formatted_bytes), kCFStringEncodingUTF8) ||
        strcmp(formatted_bytes, "positional -7")) return -14;
    CFRelease(formatted);

    for (unsigned i = 0; i < 3000; ++i) {
        CFMutableStringRef value = CFStringCreateMutableCopy(NULL, 0, CFSTR("hello"));
        CFStringAppend(value, CFSTR(" world"));
        char buffer[32];
        if (!CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8) || strcmp(buffer, "hello world")) return -12;
        CFRelease(value);
    }
    return 0;
}
