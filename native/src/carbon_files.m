#import <Foundation/Foundation.h>
#import <Carbon/Carbon.h>
#include "carbon_files.h"
#include "objc_bridge.h"
#include "compat_runtime.h"
#include <stdint.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/fsgetpath.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* The i386 catalog record is packed to two-byte boundaries. In LP64 its
   permissions gain an eight-byte fileSec pointer and change the tail offsets. */
struct __attribute__((packed, aligned(2))) catalog32 {
    uint16_t node_flags;
    int16_t volume;
    uint32_t parent_id, node_id;
    uint8_t sharing, privileges, reserved1, reserved2;
    UTCDateTime created, modified, attributes_modified, accessed, backed_up;
    uint32_t uid, gid;
    uint8_t permission_reserved, access;
    uint16_t mode;
    uint32_t file_security;
    uint8_t finder[16], extended_finder[16];
    uint64_t data_logical, data_physical, resource_logical, resource_physical;
    uint32_t valence, encoding;
};
_Static_assert(sizeof(struct catalog32) == 144, "i386 FSCatalogInfo layout");

static NSMutableDictionary *catalog_references;
static pthread_mutex_t catalog_lock = PTHREAD_MUTEX_INITIALIZER;
static NSNumber *catalog_key(FSVolumeRefNum volume, uint32_t node) {
    return @(((uint64_t)(uint16_t)volume << 32) | node);
}
static void remember_catalog(const FSRef *ref, FSVolumeRefNum volume, uint32_t node) {
    if (!ref || !node) return;
    pthread_mutex_lock(&catalog_lock);
    if (!catalog_references) catalog_references = [[NSMutableDictionary alloc] init];
    [catalog_references setObject:[NSData dataWithBytes:ref length:sizeof(*ref)] forKey:catalog_key(volume, node)];
    pthread_mutex_unlock(&catalog_lock);
}

static void catalog_to_guest(struct catalog32 *g, const FSCatalogInfo *h)
{
    if (!g) return;
    memcpy(g, h, 56);
    g->uid = h->permissions.userID; g->gid = h->permissions.groupID;
    g->permission_reserved = h->permissions.reserved1; g->access = h->permissions.userAccess;
    g->mode = h->permissions.mode;
    g->file_security = objc_bridge32_owned_object(h->permissions.fileSec);
    memcpy(g->finder, h->finderInfo, 16); memcpy(g->extended_finder, h->extFinderInfo, 16);
    g->data_logical = h->dataLogicalSize; g->data_physical = h->dataPhysicalSize;
    g->resource_logical = h->rsrcLogicalSize; g->resource_physical = h->rsrcPhysicalSize;
    g->valence = h->valence; g->encoding = h->textEncodingHint;
}
static void catalog_to_host(FSCatalogInfo *h, const struct catalog32 *g, uint32_t fields)
{
    memset(h, 0, sizeof(*h));
    if (!g) return;
    memcpy(h, g, 56);
    h->permissions.userID = g->uid; h->permissions.groupID = g->gid;
    h->permissions.reserved1 = g->permission_reserved; h->permissions.userAccess = g->access;
    h->permissions.mode = g->mode;
    if (fields & kFSCatInfoFSFileSecurityRef) h->permissions.fileSec = objc_bridge32_host_object(g->file_security);
    memcpy(h->finderInfo, g->finder, 16); memcpy(h->extFinderInfo, g->extended_finder, 16);
    h->dataLogicalSize = g->data_logical; h->dataPhysicalSize = g->data_physical;
    h->rsrcLogicalSize = g->resource_logical; h->rsrcPhysicalSize = g->resource_physical;
    h->valence = g->valence; h->textEncodingHint = g->encoding;
}

void carbon_files32_make_spec(const void *reference, void *output)
{
    if (!output) return;
    FSCatalogInfo info = {0}; HFSUniStr255 name = {0};
    FSRef parent;
    if (FSGetCatalogInfo(reference, kFSCatInfoVolume | kFSCatInfoParentDirID | kFSCatInfoNodeID, &info, &name, NULL, &parent)) return;
    remember_catalog(reference, info.volume, info.nodeID);
    remember_catalog(&parent, info.volume, info.parentDirID);
    uint8_t *spec = output;
    memcpy(spec, &info.volume, 2); memcpy(spec + 2, &info.parentDirID, 4);
    CFStringRef string = CFStringCreateWithCharacters(NULL, name.unicode, name.length);
    if (string) { CFStringGetPascalString(string, spec + 6, 64, kCFStringEncodingMacRoman); CFRelease(string); }
}

static OSErr catalog_path(FSVolumeRefNum volume_number, uint32_t node, char path[PATH_MAX])
{
    FSRef root; struct statfs volume;
    /* CarbonCore may synthesize small catalog IDs on APFS. They are not
       necessarily BSD inode numbers, so retain the FSRefs that produced them. */
    pthread_mutex_lock(&catalog_lock);
    NSData *reference = [catalog_references objectForKey:catalog_key(volume_number, node)];
    if (reference) memcpy(&root, [reference bytes], sizeof(root));
    pthread_mutex_unlock(&catalog_lock);
    if (reference) return FSRefMakePath(&root, (UInt8 *)path, PATH_MAX);
    OSErr status = FSGetVolumeInfo(volume_number, 0, NULL, 0, NULL, NULL, &root);
    if (!status) status = FSRefMakePath(&root, (UInt8 *)path, PATH_MAX);
    if (!status && node && (statfs(path, &volume) || fsgetpath(path, PATH_MAX, &volume.f_fsid, node) < 0)) status = fnfErr;
    return status;
}

struct __attribute__((packed, aligned(2))) ref_param32 {
    uint32_t queue_link;
    int16_t queue_type, trap;
    uint32_t command, completion;
    int16_t result;
    uint32_t name_pointer;
    int16_t volume, reserved1;
    uint8_t reserved2, reserved3;
    uint32_t reference, fields, catalog, name_length, name, directory_id;
    uint32_t spec, parent, output, encoding, output_name;
};
_Static_assert(sizeof(struct ref_param32) == 72, "i386 FSRefParam layout");

struct __attribute__((packed, aligned(2))) fork_io32 {
    uint32_t queue_link;
    int16_t queue_type, trap;
    uint32_t command, completion;
    volatile int16_t result;
    uint32_t reserved1;
    int16_t reserved2, fork;
    uint8_t reserved3;
    int8_t permissions;
    uint32_t reference, buffer, requested, actual;
    uint16_t position_mode;
    int64_t position;
};
_Static_assert(offsetof(struct fork_io32, position) == 46, "i386 FSForkIOParam position");
struct fork_request { FSForkIOParam native; uint32_t guest; };
static void fork_read_complete(ParmBlkPtr block)
{
    struct fork_request *request = (void *)block;
    uint32_t guest = request->guest;
    struct fork_io32 *p = (void *)(uintptr_t)guest;
    uint32_t callback = p->completion;
    p->actual = request->native.actualCount;
    p->position = request->native.positionOffset;
    /* Publish completion after the output fields and data are available. */
    __atomic_store_n(&p->result, request->native.ioResult, __ATOMIC_RELEASE);
    free(request);
    if (callback) compat_runtime32_call(callback, &guest, 1);
}

struct __attribute__((packed, aligned(2))) volume_param32 {
    uint32_t queue_link;
    int16_t queue_type, trap;
    uint32_t command, completion;
    int16_t result;
    uint32_t name;
    int16_t volume;
    uint32_t filler;
    int16_t index;
    uint32_t created, modified;
    uint16_t attributes, root_files, bitmap, allocation, total_blocks;
    uint32_t block_size, clump_size;
    uint16_t first_block;
    uint32_t next_id;
    uint16_t free_blocks, signature, drive;
    int16_t driver;
    uint16_t filesystem;
    uint32_t backup;
    uint16_t sequence;
    uint32_t writes, files, folders;
    uint8_t finder[32];
};
_Static_assert(sizeof(struct volume_param32) == 122, "i386 HVolumeParam layout");

int carbon_files32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
#define IS(s) (!strcmp(name, "_" s))
#define PTR(i) ((void *)(uintptr_t)a[i])
#define RETURN(v) do { *result = (uint64_t)(v); return 1; } while (0)
    if (IS("FSOpenIterator")) {
        if (!a[2]) RETURN((uint32_t)paramErr);
        FSIterator iterator = NULL; OSErr status = FSOpenIterator(PTR(0), a[1], &iterator);
        uint32_t handle = !status ? objc_bridge32_owned_object([NSValue valueWithPointer:iterator]) : 0;
        if (!status && !handle) { FSCloseIterator(iterator); status = memFullErr; }
        *(uint32_t *)PTR(2) = handle; RETURN((uint32_t)status);
    }
    if (IS("FSCloseIterator")) {
        FSIterator iterator = [(NSValue *)objc_bridge32_host_object(a[0]) pointerValue];
        OSErr status = iterator ? FSCloseIterator(iterator) : paramErr;
        if (!status) { uint64_t ignored; objc_bridge32_dispatch("_CFRelease", a, &ignored); }
        RETURN((uint32_t)status);
    }
    if (IS("FSGetCatalogInfoBulk")) {
        FSIterator iterator = [(NSValue *)objc_bridge32_host_object(a[0]) pointerValue];
        if (!iterator || !a[1] || a[1] > 65536 || !a[2]) RETURN((uint32_t)paramErr);
        FSCatalogInfo *infos = calloc(a[1], sizeof(*infos));
        FSRef *refs = a[6] ? PTR(6) : a[7] ? calloc(a[1], sizeof(*refs)) : NULL;
        if (!infos || (a[7] && !refs)) { free(infos); RETURN((uint32_t)memFullErr); }
        ItemCount count = 0;
        OSErr status = FSGetCatalogInfoBulk(iterator, a[1], &count, PTR(3),
            a[4] | kFSCatInfoVolume | kFSCatInfoNodeID, infos, refs, NULL, PTR(8));
        *(uint32_t *)PTR(2) = (uint32_t)count;
        for (ItemCount i = 0; i < count && i < a[1]; ++i) {
            if (a[5]) catalog_to_guest((struct catalog32 *)PTR(5) + i, infos + i);
            if (refs) remember_catalog(refs + i, infos[i].volume, infos[i].nodeID);
            if (a[7]) carbon_files32_make_spec(refs + i, (char *)PTR(7) + i * 70);
            if (infos[i].permissions.fileSec) CFRelease(infos[i].permissions.fileSec);
        }
        if (!a[6]) free(refs);
        free(infos); RETURN((uint32_t)status);
    }
    if (IS("FSGetVolumeInfo")) {
        FSVolumeInfo info = {0}; FSVolumeRefNum volume = 0;
        OSErr status = FSGetVolumeInfo((int16_t)a[0], a[1], &volume, a[3], a[4] ? &info : NULL, PTR(5), PTR(6));
        if (a[2]) *(int16_t *)PTR(2) = volume;
        if (!status && a[4]) {
            _Static_assert(offsetof(FSVolumeInfo, driverRefNum) == 124, "FSVolumeInfo fixed-width prefix");
            memcpy(PTR(4), &info, 124);
            *(int16_t *)((char *)PTR(4) + 124) = info.driverRefNum;
        }
        if (!status && a[6]) remember_catalog(PTR(6), volume, 2);
        RETURN((uint32_t)status);
    }
    if (IS("PBHGetVInfoSync")) {
        struct volume_param32 *g = PTR(0);
        if (!g || g->index < 0) RETURN((uint32_t)paramErr);
        FSVolumeInfo info = {0}; HFSUniStr255 volume_name; FSVolumeRefNum volume;
        OSErr status = FSGetVolumeInfo(g->volume, g->index, &volume, kFSVolInfoGettableInfo, &info, &volume_name, NULL);
        if (!status) {
            g->volume = volume; g->created = info.createDate.lowSeconds;
            g->modified = info.modifyDate.lowSeconds; g->backup = info.backupDate.lowSeconds;
            g->attributes = info.flags; g->signature = info.signature;
            g->drive = info.driveNumber; g->driver = info.driverRefNum; g->filesystem = info.filesystemID;
            /* The legacy API's 16-bit block counts and 32-bit size product
               cannot represent modern volumes. Saturate at 4 GiB - 64 KiB,
               preserving accurate free space below that limit. */
            g->block_size = 65536;
            g->total_blocks = (uint16_t)MIN(UINT16_MAX, info.totalBytes / g->block_size);
            g->free_blocks = (uint16_t)MIN(UINT16_MAX, info.freeBytes / g->block_size);
            g->clump_size = info.dataClumpSize; g->next_id = info.nextCatalogID;
            g->files = info.fileCount; g->folders = info.folderCount;
            memcpy(g->finder, info.finderInfo, 32);
            if (g->name) {
                CFStringRef text = CFStringCreateWithCharacters(NULL, volume_name.unicode, volume_name.length);
                CFStringGetPascalString(text, (void *)(uintptr_t)g->name, 28, kCFStringEncodingMacRoman); CFRelease(text);
            }
        }
        g->result = status; RETURN((uint32_t)status);
    }
    if (IS("HGetVol")) {
        char path[PATH_MAX]; FSRef reference; FSCatalogInfo info = {0};
        OSErr status = getcwd(path, sizeof(path)) ? FSPathMakeRef((const UInt8 *)path, &reference, NULL) : fnfErr;
        if (!status) status = FSGetCatalogInfo(&reference, kFSCatInfoVolume | kFSCatInfoNodeID, &info, NULL, NULL, NULL);
        if (!status) {
            remember_catalog(&reference, info.volume, info.nodeID);
            if (a[1]) *(int16_t *)PTR(1) = info.volume;
            if (a[2]) *(uint32_t *)PTR(2) = info.nodeID;
            if (a[0]) {
                HFSUniStr255 name;
                status = FSGetVolumeInfo(info.volume, 0, NULL, 0, NULL, &name, NULL);
                if (!status) { CFStringRef text = CFStringCreateWithCharacters(NULL, name.unicode, name.length); CFStringGetPascalString(text, PTR(0), 28, kCFStringEncodingMacRoman); CFRelease(text); }
            }
        }
        RETURN((uint32_t)status);
    }
    if (IS("HSetVol")) {
        char path[PATH_MAX];
        OSErr status = catalog_path((FSVolumeRefNum)a[1], a[2], path);
        if (!status && chdir(path)) status = fnfErr;
        if (getenv("LP32_TRACE_CARBON_FILES")) fprintf(stderr, "carbon-files: HSetVol %d/%u -> %s status=%d\n", (int16_t)a[1], a[2], status ? "?" : path, status);
        RETURN((uint32_t)status);
    }
    if (IS("FSIsAliasFile")) RETURN((uint32_t)FSIsAliasFile(PTR(0), PTR(1), PTR(2)));
    if (IS("FSResolveAliasFile")) RETURN((uint32_t)FSResolveAliasFile(PTR(0), a[1] != 0, PTR(2), PTR(3)));
    if (IS("PBMakeFSRefSync")) {
        struct ref_param32 *g = PTR(0);
        if (!g) RETURN((uint32_t)paramErr);
        /* The PB entry point disappeared from LP64. Resolve its volume and
           catalog ID with the modern inode-to-path API, then append the name. */
        char path[PATH_MAX];
        OSErr status = catalog_path(g->volume, g->directory_id, path);
        if (!status && g->name_pointer && *(uint8_t *)(uintptr_t)g->name_pointer) {
            CFStringRef name = CFStringCreateWithPascalString(NULL, (void *)(uintptr_t)g->name_pointer, kCFStringEncodingMacRoman);
            NSString *full = [[NSString stringWithUTF8String:path] stringByAppendingPathComponent:(NSString *)name];
            status = FSPathMakeRef((const UInt8 *)[full fileSystemRepresentation], (void *)(uintptr_t)g->output, NULL);
            if (name) CFRelease(name);
        } else if (!status) status = FSPathMakeRef((UInt8 *)path, (void *)(uintptr_t)g->output, NULL);
        g->result = status;
        RETURN((uint32_t)status);
    }
    if (IS("FindFolder")) {
        uint32_t reference = compat_runtime32_allocate(sizeof(FSRef), 1);
        if (!reference) RETURN((uint32_t)memFullErr);
        uint32_t args[] = {a[0], a[1], a[2], reference};
        OSErr status = (OSErr)compat_runtime32_dispatch_import("_FSFindFolder", args);
        FSCatalogInfo info = {0};
        if (!status) status = FSGetCatalogInfo((void *)(uintptr_t)reference, kFSCatInfoVolume | kFSCatInfoNodeID, &info, NULL, NULL, NULL);
        if (!status) { remember_catalog((void *)(uintptr_t)reference, info.volume, info.nodeID); if (a[3]) *(int16_t *)PTR(3) = info.volume; if (a[4]) *(uint32_t *)PTR(4) = info.nodeID; }
        compat_runtime32_deallocate(reference);
        RETURN((uint32_t)status);
    }
    if (IS("FSGetCatalogInfo")) {
        FSCatalogInfo info = {0}; FSRef parent;
        OSErr status = FSGetCatalogInfo(PTR(0), a[1] | kFSCatInfoVolume | kFSCatInfoNodeID | kFSCatInfoParentDirID, &info, PTR(3), NULL, &parent);
        if (getenv("LP32_TRACE_CARBON_FILES")) {
            char path[PATH_MAX] = {0}; FSRefMakePath(PTR(0), (UInt8 *)path, sizeof(path));
            fprintf(stderr, "carbon-files: catalog %s fields=%x volume=%d parent=%u node=%u status=%d\n", path, a[1], info.volume, info.parentDirID, info.nodeID, status);
        }
        if (!status) {
            remember_catalog(PTR(0), info.volume, info.nodeID);
            remember_catalog(&parent, info.volume, info.parentDirID);
            if (a[5]) memcpy(PTR(5), &parent, sizeof(parent));
            catalog_to_guest(PTR(2), &info); carbon_files32_make_spec(PTR(0), PTR(4));
        }
        if (info.permissions.fileSec) CFRelease(info.permissions.fileSec);
        RETURN((uint32_t)status);
    }
    if (IS("FSSetCatalogInfo")) {
        FSCatalogInfo info; catalog_to_host(&info, PTR(2), a[1]);
        RETURN((uint32_t)FSSetCatalogInfo(PTR(0), a[1], &info));
    }
    if (IS("FSCreateDirectoryUnicode") || IS("FSCreateFileUnicode")) {
        FSCatalogInfo info; catalog_to_host(&info, PTR(4), a[3]); FSRef created;
        OSErr status = IS("FSCreateDirectoryUnicode") ?
            FSCreateDirectoryUnicode(PTR(0), a[1], PTR(2), a[3], a[4] ? &info : NULL, &created, NULL, PTR(7)) :
            FSCreateFileUnicode(PTR(0), a[1], PTR(2), a[3], a[4] ? &info : NULL, &created, NULL);
        if (!status) { if (a[5]) memcpy(PTR(5), &created, sizeof(created)); carbon_files32_make_spec(&created, PTR(6)); }
        RETURN((uint32_t)status);
    }
    if (IS("FSDeleteObject")) RETURN((uint32_t)FSDeleteObject(PTR(0)));
    if (IS("FSGetDataForkName")) RETURN((uint32_t)FSGetDataForkName(PTR(0)));
    if (IS("FSOpenFork")) RETURN((uint32_t)FSOpenFork(PTR(0), a[1], PTR(2), (SInt8)a[3], PTR(4)));
    if (IS("FSCloseFork")) RETURN((uint32_t)FSCloseFork((FSIORefNum)a[0]));
    if (IS("FSGetForkSize")) RETURN((uint32_t)FSGetForkSize((FSIORefNum)a[0], PTR(1)));
    if (IS("FSGetForkPosition")) RETURN((uint32_t)FSGetForkPosition((FSIORefNum)a[0], PTR(1)));
    if (IS("FSSetForkSize") || IS("FSSetForkPosition")) {
        int64_t value = (int64_t)((uint64_t)a[2] | (uint64_t)a[3] << 32);
        RETURN((uint32_t)(IS("FSSetForkSize") ? FSSetForkSize((FSIORefNum)a[0], (UInt16)a[1], value) : FSSetForkPosition((FSIORefNum)a[0], (UInt16)a[1], value)));
    }
    if (IS("PBReadForkAsync")) {
        struct fork_io32 *p = PTR(0);
        if (!p) RETURN((uint32_t)paramErr);
        struct fork_request *request = calloc(1, sizeof(*request));
        if (!request) { p->result = memFullErr; RETURN(0); }
        request->guest = a[0];
        request->native.forkRefNum = p->fork;
        request->native.buffer = (void *)(uintptr_t)p->buffer;
        request->native.requestCount = p->requested;
        request->native.positionMode = p->position_mode;
        request->native.positionOffset = p->position;
        request->native.ioCompletion = fork_read_complete;
        p->actual = 0;
        p->result = 1;
        PBReadForkAsync(&request->native);
        RETURN(0);
    }
    if (IS("FSReadFork") || IS("FSWriteFork")) {
        int64_t position = (int64_t)((uint64_t)a[2] | (uint64_t)a[3] << 32);
        ByteCount actual = 0;
        OSErr status = IS("FSReadFork") ?
            FSReadFork((FSIORefNum)a[0], (UInt16)a[1], position, a[4], PTR(5), &actual) :
            FSWriteFork((FSIORefNum)a[0], (UInt16)a[1], position, a[4], PTR(5), &actual);
        if (a[6]) *(uint32_t *)PTR(6) = (uint32_t)actual;
        RETURN((uint32_t)status);
    }
    return 0;
#undef IS
#undef PTR
#undef RETURN
}
