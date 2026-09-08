#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Carbon/Carbon.h>
#import <ImageIO/ImageIO.h>
#include "movie_bridge.h"
#include "compat_runtime.h"
#include "objc_bridge.h"
#include "carbon_native.h"
#include <string.h>
#include <stdatomic.h>

static _Atomic unsigned active_movies;

bool movie_bridge32_active(void)
{
    return atomic_load_explicit(&active_movies, memory_order_relaxed) != 0;
}

void movie_bridge32_service_main(void)
{
    static _Thread_local bool servicing;
    if (!atomic_load_explicit(&active_movies, memory_order_relaxed) ||
        ![NSThread isMainThread] || servicing) return;
    servicing = true;
    /* The UI thread sleeps while TFU's render thread tasks movies. AVPlayer
       commits deferred transactions on the main queue; without a main-loop
       turn they stay queued until the user skips back to a native menu. */
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0001, false);
    /* Intro playback bypasses ReceiveNextEvent. Dispatch AppKit activation
       events and reconcile the real game window from this main-thread loop
       too, including while the player is paused or waiting after a seek. */
    carbon_native32_pump_appkit_events();
    servicing = false;
}

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static id object(uint32_t value) { return objc_bridge32_host_object(value); }
static uint32_t owned(id value) { return objc_bridge32_owned_object(value); }
static void release_handle(uint32_t value) { uint64_t ignored; objc_bridge32_dispatch("_CFRelease", &value, &ignored); }

@class LP32Movie;
@interface LP32MovieTrack : NSObject {
@public AVAssetTrack *track;
    LP32Movie *movie;
    bool muted;
}
@end
@implementation LP32MovieTrack
- (void)dealloc { [track release]; [super dealloc]; }
@end
@interface LP32MovieVisual : NSObject {
@public NSDictionary *attributes;
    AVPlayerItemVideoOutput *output;
    LP32Movie *movie; /* The movie retains its context. */
}
@end
@implementation LP32MovieVisual
- (void)dealloc { [attributes release]; [output release]; [super dealloc]; }
@end

@interface LP32Movie : NSObject {
@public AVURLAsset *asset;
    AVPlayerItem *item;
    AVPlayer *player;
    LP32MovieVisual *visual;
    uint32_t guest_handle, drawing_callback, drawing_refcon, drawing_flags;
    NSMutableDictionary *sample_times;
    NSMutableDictionary *track_handles;
    _Atomic bool ended;
    _Atomic bool play_requested, seek_pending;
    _Atomic unsigned seek_generation, finished_seek;
    CMTime seek_target;
    bool registered;
    int16_t box[4];
    unsigned delivered_frames;
    unsigned timing_queries, done_queries;
}
@end
@implementation LP32Movie
- (void)playbackEnded:(NSNotification *)notification {
    (void)notification;
    /* An EOF notification can arrive after a newer rewind was requested. */
    if (!seek_pending && CMTimeCompare([player currentTime], [asset duration]) >= 0) ended = true;
}
- (void)dealloc {
    if (registered) atomic_fetch_sub_explicit(&active_movies, 1, memory_order_relaxed);
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [player pause];
    if (visual) {
        [item removeOutput:visual->output];
        [visual->output release]; visual->output = nil;
        visual->movie = nil;
    }
    [sample_times release];
    for (LP32MovieTrack *track in [track_handles allValues]) {
        track->movie = nil;
        [track->track release]; track->track = nil;
    }
    [track_handles release];
    [visual release]; [player release]; [item release]; [asset release];
    [super dealloc];
}
@end

/* CVPixelBuffer base addresses may be above 4 GiB. A locked guest buffer is
   copied into guest memory, and copied back unless locked read-only. */
@interface LP32MoviePixels : NSObject {
@public CVPixelBufferRef buffer;
    uint32_t data, locks;
    size_t bytes;
    CVPixelBufferLockFlags lock_flags;
}
@end
@implementation LP32MoviePixels
- (void)dealloc {
    if (locks) CVPixelBufferUnlockBaseAddress(buffer, lock_flags);
    if (data) compat_runtime32_deallocate(data);
    if (buffer) CVPixelBufferRelease(buffer);
    [super dealloc];
}
@end

static void attach_visual(LP32Movie *movie, LP32MovieVisual *visual)
{
    if (movie->visual == visual) return;
    if (visual && visual->movie && visual->movie != movie)
        attach_visual(visual->movie, nil);
    if (movie->visual) {
        [movie->item removeOutput:movie->visual->output];
        [movie->visual->output release]; movie->visual->output = nil;
        movie->visual->movie = nil;
        [movie->visual release];
    }
    movie->visual = [visual retain];
    if (visual) {
        visual->movie = movie;
        if (!visual->output) {
            NSMutableDictionary *attributes = [NSMutableDictionary dictionaryWithDictionary:visual->attributes ?: @{}];
            if (!attributes[(id)kCVPixelBufferPixelFormatTypeKey]) attributes[(id)kCVPixelBufferPixelFormatTypeKey] = @(kCVPixelFormatType_32BGRA);
            if (getenv("LP32_TRACE_MOVIES")) fprintf(stderr, "compat32: movie pixel attributes=%s\n", [[attributes description] UTF8String]);
            visual->output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attributes];
            [visual->output setSuppressesPlayerRendering:YES];
        }
        [movie->item addOutput:visual->output];
    }
}

static LP32Movie *open_movie(NSURL *url, LP32MovieVisual *visual)
{
    if (!url || ![url isFileURL]) return nil;
    if (getenv("LP32_BACKGROUND_TEST") && getenv("LP32_CONTINUE_SKIP_INTROS") &&
        ([@"FMV-GoldGuy-WMV9_HD_STEREO.mov" isEqualToString:url.lastPathComponent] ||
         [@"Aspyr.mov" isEqualToString:url.lastPathComponent])) {
        /* Graphics-only replay: exercise the game's missing-intro path.
           Normal launches and movie regression tests keep full playback. */
        fprintf(stderr, "compat32: movie intro omitted name=%s\n", url.lastPathComponent.UTF8String);
        return nil;
    }
    LP32Movie *movie = [[[LP32Movie alloc] init] autorelease];
    movie->asset = [[AVURLAsset alloc] initWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey:@YES}];
    NSArray *tracks = [movie->asset tracksWithMediaType:AVMediaTypeVideo];
    if (![tracks count] || ![movie->asset isPlayable]) {
        fprintf(stderr, "compat32: cannot decode movie %s\n", [[url path] fileSystemRepresentation]);
        return nil;
    }
    CGSize size = [[tracks firstObject] naturalSize];
    movie->box[2] = size.height; movie->box[3] = size.width;
    movie->item = [[AVPlayerItem alloc] initWithAsset:movie->asset];
    [[NSNotificationCenter defaultCenter] addObserver:movie selector:@selector(playbackEnded:)
        name:AVPlayerItemDidPlayToEndTimeNotification object:movie->item];
    movie->player = [[AVPlayer alloc] initWithPlayerItem:movie->item];
    [movie->player setActionAtItemEnd:AVPlayerActionAtItemEndPause];
    [movie->player setAutomaticallyWaitsToMinimizeStalling:NO];
    if (getenv("LP32_HEADLESS") || getenv("LP32_MUTE_AUDIO")) [movie->player setMuted:YES];
    attach_visual(movie, visual);
    movie->registered = true;
    atomic_fetch_add_explicit(&active_movies, 1, memory_order_relaxed);
    fprintf(stderr, "compat32: AVFoundation movie %s %.0fx%.0f duration=%.2fs audio-tracks=%lu\n",
        [[[url path] lastPathComponent] UTF8String], size.width, size.height,
        CMTimeGetSeconds([movie->asset duration]), (unsigned long)[[movie->asset tracksWithMediaType:AVMediaTypeAudio] count]);
    return movie;
}

static CMTime movie_time(LP32Movie *movie)
{
    if (!movie) return kCMTimeZero;
    @synchronized (movie) {
        if (movie->seek_pending) return movie->seek_target;
    }
    return [movie->player currentTime];
}

static void seek_movie(LP32Movie *movie, CMTime time)
{
    if (!movie) return;
    unsigned generation;
    @synchronized (movie) {
        generation = ++movie->seek_generation;
        movie->seek_target = time;
        movie->seek_pending = true;
        movie->ended = false;
    }
    /* QuickTime updates its time synchronously. AVPlayer seeks asynchronously
       and pauses at EOF; keep the requested transport state across a rewind.
       Completions only publish state. The guest's MoviesTask applies playback
       so an old completion cannot restart a stopped/disposed movie. */
    [movie->player seekToTime:time toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero
        completionHandler:^(BOOL finished) {
            if (!finished) return;
            unsigned previous = movie->finished_seek;
            while (previous < generation && !atomic_compare_exchange_weak(
                &movie->finished_seek, &previous, generation)) {}
        }];
}

static void service_movie_seek(LP32Movie *movie)
{
    if (!movie || !movie->seek_pending || movie->finished_seek != movie->seek_generation) return;
    movie->seek_pending = false;
    movie->ended = false;
    if (movie->play_requested) [movie->player play];
}
static void trace_movie(LP32Movie *movie, const char *event)
{
    if (!movie || !getenv("LP32_TRACE_MOVIES")) return;
    fprintf(stderr, "compat32: movie %s name=%s time=%.3f rate=%.3f status=%ld item-status=%ld frames=%u error=%s\n",
            event, [[[[movie->asset URL] path] lastPathComponent] UTF8String],
            CMTimeGetSeconds(movie_time(movie)), [movie->player rate],
            (long)[movie->player status], (long)[movie->item status], movie->delivered_frames,
            [[[movie->item error] description] UTF8String] ?: "none");
    fprintf(stderr, "compat32: movie state item-current=%d asset-status=%ld tracks=%lu control=%ld waiting=%s visual=%p output=%p\n",
        movie->player.currentItem == movie->item, (long)[movie->asset statusOfValueForKey:@"playable" error:NULL],
        (unsigned long)movie->item.tracks.count, (long)movie->player.timeControlStatus,
        [movie->player.reasonForWaitingToPlay UTF8String] ?: "none", movie->visual, movie->visual ? movie->visual->output : nil);
}
/* Read compressed sample timing, without decoding frames. TFU builds its
   movie frame index with nextTimeMediaSample, so nominal FPS is insufficient. */
static NSArray *movie_sample_times(LP32Movie *movie, uint32_t type)
{
    /* QuickTime accepts media characteristics as well as concrete types.
       TFU requests VisualMediaCharacteristic ('eyes') to build its frame
       index; returning no samples makes it immediately dispose the movie. */
    if (type == 'eyes') type = 'vide';
    if (type == 'ears') type = 'soun';
    if (!movie->sample_times) movie->sample_times = [[NSMutableDictionary alloc] init];
    NSArray *cached = movie->sample_times[@(type)];
    if (cached) return cached;
    NSMutableArray *times = [NSMutableArray array];
    NSString *media = type == 'vide' ? AVMediaTypeVideo : type == 'soun' ? AVMediaTypeAudio : nil;
    for (AVAssetTrack *track in media ? [movie->asset tracksWithMediaType:media] : @[]) {
        AVAssetReader *reader = [[[AVAssetReader alloc] initWithAsset:movie->asset error:NULL] autorelease];
        AVAssetReaderTrackOutput *output = [[[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:nil] autorelease];
        output.alwaysCopiesSampleData = NO;
        if (![reader canAddOutput:output]) continue;
        [reader addOutput:output];
        if (![reader startReading]) continue;
        CMSampleBufferRef sample;
        while ((sample = [output copyNextSampleBuffer])) {
            CMTime pts = CMTimeConvertScale(CMSampleBufferGetPresentationTimeStamp(sample), 600, kCMTimeRoundingMethod_Default);
            CMTime duration = CMTimeConvertScale(CMSampleBufferGetDuration(sample), 600, kCMTimeRoundingMethod_Default);
            if (CMTIME_IS_NUMERIC(pts)) [times addObject:@[@(pts.value), @(CMTIME_IS_NUMERIC(duration) ? duration.value : 0)]];
            CFRelease(sample);
        }
    }
    [times sortUsingComparator:^NSComparisonResult(NSArray *a, NSArray *b) { return [a[0] compare:b[0]]; }];
    /* Preroll/edit-list samples can repeat the first presentation time with
       a zero duration. Expose each display instant once, keeping its real
       duration instead of presenting that empty leading sample as a frame. */
    NSMutableArray *unique = [NSMutableArray array];
    for (NSArray *sample in times) {
        NSArray *previous = [unique lastObject];
        if (previous && [previous[0] isEqual:sample[0]]) {
            if ([sample[1] longLongValue] > [previous[1] longLongValue])
                unique[unique.count - 1] = sample;
        } else [unique addObject:sample];
    }
    times = unique;
    if (getenv("LP32_TRACE_MOVIES")) fprintf(stderr, "compat32: movie samples type=%08x count=%lu\n", type, (unsigned long)[times count]);
    movie->sample_times[@(type)] = times;
    return times;
}
static int32_t movie_ticks(LP32Movie *movie) {
    CMTime time = CMTimeConvertScale(movie_time(movie), 600, kCMTimeRoundingMethod_Default);
    return CMTIME_IS_NUMERIC(time) ? (int32_t)time.value : 0;
}
static NSURL *data_reference_url(uint32_t reference, uint32_t type) {
    if (!reference || type != 'url ') return nil;
    uint32_t data = *(uint32_t *)(uintptr_t)reference;
    return data ? [NSURL URLWithString:[NSString stringWithUTF8String:(const char *)(uintptr_t)data]] : nil;
}

uint32_t movie_bridge32_pointer_import(const char *name)
{
#define KEY(s, value) if (!strcmp(name, "_" s)) return objc_bridge32_guest_object((id)(value))
    KEY("kCVPixelBufferBytesPerRowAlignmentKey", kCVPixelBufferBytesPerRowAlignmentKey);
    KEY("kCVPixelBufferHeightKey", kCVPixelBufferHeightKey);
    KEY("kCVPixelBufferPixelFormatTypeKey", kCVPixelBufferPixelFormatTypeKey);
    KEY("kCVPixelBufferWidthKey", kCVPixelBufferWidthKey);
    KEY("kQTVisualContextPixelBufferAttributesKey", @"PixelBufferAttributes");
#undef KEY
    return 0;
}

int movie_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
#define IS(s) (!strcmp(name, "_" s))
#define PTR(i) ((void *)(uintptr_t)a[i])
#define OBJ(i) object(a[i])
#define RETURN(v) do { *result = (uint64_t)(v); return 1; } while (0)
    if (strncmp(name, "_QT", 3) && strncmp(name, "_CV", 3) && !strstr(name, "Movie") && !strstr(name, "TrackAudio") && !IS("FindNextComponent")) return 0;
    @autoreleasepool {
    if (IS("FindNextComponent")) {
        const uint32_t *description = PTR(1);
        if (!description) return 0;
        uint32_t type = description[0], subtype = description[1];
        if (!((type == 'sdec' && subtype == '.mp3') ||
              ((type == 'grip' || type == 'grex') && subtype == 'JPEG'))) return 0;
        if (a[0]) RETURN(0);
        bool supported = false;
        if (type == 'sdec') {
            UInt32 bytes = 0;
            if (!AudioFormatGetPropertyInfo(kAudioFormatProperty_DecodeFormatIDs, 0, NULL, &bytes)) {
                AudioFormatID *formats = malloc(bytes);
                if (formats && !AudioFormatGetProperty(kAudioFormatProperty_DecodeFormatIDs, 0, NULL, &bytes, formats))
                    for (unsigned i = 0; i < bytes / sizeof(*formats); ++i) supported |= formats[i] == kAudioFormatMPEGLayer3;
                free(formats);
            }
        } else {
            CFArrayRef formats = type == 'grip' ? CGImageSourceCopyTypeIdentifiers() : CGImageDestinationCopyTypeIdentifiers();
            supported = [(NSArray *)formats containsObject:@"public.jpeg"];
            if (formats) CFRelease(formats);
        }
        static NSMutableDictionary *components;
        if (!components) components = [[NSMutableDictionary alloc] init];
        NSNumber *key = @(((uint64_t)type << 32) | subtype);
        if (!components[key]) components[key] = @{ @"type": @(type), @"subtype": @(subtype) };
        RETURN(supported ? objc_bridge32_guest_object(components[key]) : 0);
    }
    if (IS("EnterMovies")) RETURN(0);
    if (IS("QTPixelBufferContextCreate")) {
        if (!a[2]) RETURN((uint32_t)paramErr);
        LP32MovieVisual *visual = [[[LP32MovieVisual alloc] init] autorelease];
        NSDictionary *options = OBJ(1);
        visual->attributes = [options[@"PixelBufferAttributes"] copy];
        *(uint32_t *)PTR(2) = owned(visual); RETURN(0);
    }
    if (IS("QTAudioContextCreateForAudioDevice")) {
        if (!a[3]) RETURN((uint32_t)paramErr);
        if (a[1]) { fprintf(stderr, "compat32: movie audio-device selection is unavailable\n"); RETURN((uint32_t)unimpErr); }
        *(uint32_t *)PTR(3) = owned(@{}); RETURN(0);
    }
    if (IS("QTVisualContextRelease") || IS("QTAudioContextRelease") || IS("CVBufferRelease")) { release_handle(a[0]); RETURN(0); }
    if (IS("QTNewDataReferenceFromFSRef")) {
        if (!a[2] || !a[3]) RETURN((uint32_t)paramErr);
        NSURL *url = [(NSURL *)CFURLCreateFromFSRef(NULL, PTR(0)) autorelease];
        if (!url) RETURN((uint32_t)fnfErr);
        const char *string = [[url absoluteString] UTF8String];
        uint32_t size = (uint32_t)strlen(string) + 1;
        uint32_t reference = compat_runtime32_allocate(size + 8, 1);
        if (!reference) RETURN((uint32_t)memFullErr);
        *(uint32_t *)(uintptr_t)reference = reference + 8;
        *(uint32_t *)(uintptr_t)(reference + 4) = size;
        memcpy((void *)(uintptr_t)(reference + 8), string, size);
        *(uint32_t *)PTR(2) = reference; *(uint32_t *)PTR(3) = 'url ';
        RETURN(0);
    }
    if (IS("NewMovieFromProperties") || IS("NewMovieFromDataRef")) {
        NSURL *url = nil; LP32MovieVisual *visual = nil; uint32_t *out;
        if (IS("NewMovieFromProperties")) {
            struct property32 { uint32_t property_class, id, size, address; int32_t status; } *properties = PTR(1);
            out = PTR(4);
            for (unsigned i = 0; i < a[0]; ++i) {
                struct property32 *p = properties + i;
                uint32_t value = p->address && p->size >= 4 ? *(uint32_t *)(uintptr_t)p->address : 0;
                p->status = 0;
                if (p->property_class == 'dloc') {
                    if (p->id == 'cfur') url = object(value);
                    else if (p->id == 'cfpp' || p->id == 'cfnp') url = [NSURL fileURLWithPath:object(value)];
                    else if (p->id == 'dref' && p->size >= 8) {
                        uint32_t *ref = (void *)(uintptr_t)p->address;
                        url = data_reference_url(ref[0], ref[1]);
                    } else p->status = unimpErr;
                } else if (p->property_class == 'ctxt' && p->id == 'visu') visual = object(value);
                else if (p->property_class != 'mprp' && p->property_class != 'mins' && p->property_class != 'ctxt') p->status = unimpErr;
            }
            struct property32 *outputs = PTR(3);
            for (unsigned i = 0; i < a[2]; ++i) outputs[i].status = unimpErr;
        } else { out = PTR(0); url = data_reference_url(a[2], a[3]); }
        if (!out) RETURN((uint32_t)paramErr);
        *out = 0;
        LP32Movie *movie = open_movie(url, visual);
        if (!movie) RETURN((uint32_t)-2048); /* noMovieFound */
        *out = movie->guest_handle = owned(movie); RETURN(0);
    }
    if (IS("DisposeMovie")) { LP32Movie *movie = OBJ(0); trace_movie(movie, "dispose"); if (movie) { movie->play_requested = false; ++movie->seek_generation; } [movie->player pause]; release_handle(a[0]); RETURN(0); }
    if (IS("SetMovieDrawingCompleteProc")) {
        LP32Movie *movie = OBJ(0);
        if (movie) {
            movie->drawing_flags = a[1];
            movie->drawing_callback = a[2];
            movie->drawing_refcon = a[3];
        }
        RETURN(0);
    }
    if (IS("SetMovieVisualContext")) { LP32Movie *movie = OBJ(0); if (!movie) RETURN((uint32_t)paramErr); attach_visual(movie, OBJ(1)); RETURN(0); }
    if (IS("StartMovie")) { LP32Movie *movie = OBJ(0); trace_movie(movie, "start"); if (movie) movie->play_requested = true; [movie->player play]; RETURN(0); }
    if (IS("StopMovie")) { LP32Movie *movie = OBJ(0); trace_movie(movie, "stop"); if (movie) movie->play_requested = false; [movie->player pause]; RETURN(0); }
    if (IS("SetMovieActive")) { LP32Movie *movie = OBJ(0); if (movie && !a[1]) { movie->play_requested = false; [movie->player pause]; } RETURN(0); }
    if (IS("SetMovieVolume")) { LP32Movie *movie = OBJ(0); [movie->player setVolume:MAX(0, MIN(1, (int16_t)a[1] / 256.0))]; RETURN(0); }
    if (IS("GoToBeginningOfMovie") || IS("PrerollMovie") || IS("SetMovieTime") || IS("SetMovieTimeValue")) {
        LP32Movie *movie = OBJ(0); CMTime time = kCMTimeZero;
        if (IS("PrerollMovie") || IS("SetMovieTimeValue")) time = CMTimeMake((int32_t)a[1], 600);
        if (IS("SetMovieTime")) {
            uint32_t *record = PTR(1);
            /* Intel QuickTime's wide stores the low word first. */
            if (record && record[2]) time = CMTimeMake((int64_t)(((uint64_t)record[1] << 32) | record[0]), record[2]);
        }
        seek_movie(movie, time);
        if (getenv("LP32_TRACE_MOVIES")) fprintf(stderr, "compat32: movie seek via=%s target=%.3f\n", name, CMTimeGetSeconds(time));
        RETURN(0);
    }
    if (IS("GetMovieTime")) {
        LP32Movie *movie = OBJ(0); int32_t ticks = movie_ticks(movie);
        if (a[1]) { uint32_t *record = PTR(1); record[0] = ticks; record[1] = ticks < 0 ? UINT32_MAX : 0; record[2] = 600; record[3] = a[0]; }
        RETURN((uint32_t)ticks);
    }
    if (IS("GetMovieTimeBase")) RETURN(a[0]);
    if (IS("GetMovieNextInterestingTime")) {
        LP32Movie *movie = OBJ(0);
        int32_t found = -1, duration = 0, start = (int32_t)a[4];
        bool reverse = (int32_t)a[5] < 0, edge = a[1] & (1u << 14);
        if (movie && (a[1] & 1) && a[3]) {
            const uint32_t *types = PTR(3);
            for (unsigned i = 0; i < a[2]; ++i) {
                NSArray *times = movie_sample_times(movie, types[i]);
                for (NSArray *sample in reverse ? [times reverseObjectEnumerator] : times) {
                    int32_t tick = [sample[0] intValue];
                    bool matches = reverse ? (edge ? tick <= start : tick < start) : (edge ? tick >= start : tick > start);
                    if (!matches) continue;
                    if (found < 0 || (reverse ? tick > found : tick < found)) { found = tick; duration = [sample[1] intValue]; }
                    break;
                }
            }
        }
        if (a[6]) *(int32_t *)PTR(6) = found;
        if (a[7]) *(int32_t *)PTR(7) = duration;
        if (movie && getenv("LP32_TRACE_MOVIES") && (++movie->timing_queries <= 4 || found < 0))
            fprintf(stderr, "compat32: movie next-time flags=%08x count=%u type=%08x start=%d rate=%08x found=%d duration=%d\n", a[1], a[2], a[3] ? *(uint32_t *)PTR(3) : 0, start, a[5], found, duration);
        RETURN(0);
    }
    if (IS("GetMovieTimeScale")) RETURN(600);
    if (IS("GetMovieBox") || IS("SetMovieBox")) {
        LP32Movie *movie = OBJ(0); if (movie && a[1]) { if (IS("GetMovieBox")) memcpy(PTR(1), movie->box, 8); else memcpy(movie->box, PTR(1), 8); } RETURN(0);
    }
    if (IS("IsMovieDone")) {
        LP32Movie *movie = OBJ(0);
        if (!movie || [movie->player status] == AVPlayerStatusFailed) RETURN(1);
        if (getenv("LP32_TRACE_MOVIES") && (movie->done_queries++ < 4 || movie->done_queries % 600 == 0)) {
            CMTime current = movie_time(movie), duration = [movie->asset duration];
            fprintf(stderr, "compat32: movie done? current=%lld/%d flags=%u duration=%lld/%d flags=%u compare=%d\n", current.value,current.timescale,current.flags,duration.value,duration.timescale,duration.flags,CMTimeCompare(current,duration));
            trace_movie(movie, "poll");
        }
        RETURN(movie->ended || CMTimeCompare(movie_time(movie), [movie->asset duration]) >= 0);
    }
    if (IS("GetMovieIndTrackType")) {
        LP32Movie *movie = OBJ(0);
        if (getenv("LP32_TRACE_MOVIES")) fprintf(stderr, "compat32: movie track index=%u type=%08x flags=%08x\n", a[1], a[2], a[3]);
        NSString *type = (a[2] == 'soun' || a[2] == 'ears') ? AVMediaTypeAudio :
                        (a[2] == 'vide' || a[2] == 'eyes') ? AVMediaTypeVideo : nil;
        NSArray *tracks = type ? [movie->asset tracksWithMediaType:type] : @[];
        if (!a[1] || a[1] > [tracks count]) RETURN(0);
        AVAssetTrack *asset_track = tracks[a[1] - 1];
        if (!movie->track_handles) movie->track_handles = [[NSMutableDictionary alloc] init];
        LP32MovieTrack *track = movie->track_handles[@(asset_track.trackID)];
        if (!track) {
            track = [[[LP32MovieTrack alloc] init] autorelease];
            track->track = [asset_track retain]; track->movie = movie;
            movie->track_handles[@(asset_track.trackID)] = track;
        }
        RETURN(objc_bridge32_guest_object(track));
    }
    if (IS("SetTrackAudioMute")) {
        LP32MovieTrack *track = OBJ(0);
        if (!track || !track->movie) RETURN((uint32_t)paramErr);
        track->muted = a[1] != 0;
        NSMutableArray *parameters = [NSMutableArray array];
        for (LP32MovieTrack *entry in [track->movie->track_handles allValues]) {
            if (![entry->track.mediaType isEqual:AVMediaTypeAudio]) continue;
            AVMutableAudioMixInputParameters *input = [AVMutableAudioMixInputParameters audioMixInputParametersWithTrack:entry->track];
            [input setVolume:entry->muted ? 0 : 1 atTime:kCMTimeZero];
            [parameters addObject:input];
        }
        AVMutableAudioMix *mix = [AVMutableAudioMix audioMix]; mix.inputParameters = parameters;
        track->movie->item.audioMix = mix;
        if (getenv("LP32_TRACE_MOVIES")) fprintf(stderr, "compat32: movie track mute id=%d muted=%u\n", track->track.trackID, a[1]);
        RETURN(0);
    }
    if (IS("QTVisualContextIsNewImageAvailable")) {
        LP32MovieVisual *visual = OBJ(0);
        RETURN(visual && visual->movie && [visual->output hasNewPixelBufferForItemTime:movie_time(visual->movie)]);
    }
    if (IS("QTVisualContextCopyImageForTime")) {
        LP32MovieVisual *visual = OBJ(0);
        if (!a[3]) RETURN((uint32_t)paramErr);
        *(uint32_t *)PTR(3) = 0;
        if (!visual || !visual->movie) RETURN((uint32_t)paramErr);
        CVPixelBufferRef buffer = [visual->output copyPixelBufferForItemTime:movie_time(visual->movie) itemTimeForDisplay:NULL];
        if (buffer) {
            ++visual->movie->delivered_frames;
            LP32MoviePixels *pixels = [[[LP32MoviePixels alloc] init] autorelease]; pixels->buffer = buffer;
            *(uint32_t *)PTR(3) = owned(pixels);
        }
        RETURN(0);
    }
    if (IS("CVPixelBufferGetBytesPerRow")) { LP32MoviePixels *pixels = OBJ(0); RETURN(pixels ? CVPixelBufferGetBytesPerRow(pixels->buffer) : 0); }
    if (IS("CVPixelBufferGetBaseAddress")) { LP32MoviePixels *pixels = OBJ(0); RETURN(pixels ? pixels->data : 0); }
    if (IS("CVPixelBufferLockBaseAddress")) {
        LP32MoviePixels *pixels = OBJ(0); if (!pixels) RETURN((uint32_t)kCVReturnInvalidArgument);
        if (pixels->locks++) RETURN(0);
        pixels->lock_flags = a[1];
        CVReturn status = CVPixelBufferLockBaseAddress(pixels->buffer, pixels->lock_flags);
        if (status) { pixels->locks = 0; RETURN((uint32_t)status); }
        pixels->bytes = CVPixelBufferGetBytesPerRow(pixels->buffer) * CVPixelBufferGetHeight(pixels->buffer);
        pixels->data = pixels->bytes <= UINT32_MAX ? compat_runtime32_allocate((uint32_t)pixels->bytes, 0) : 0;
        if (!pixels->data) { CVPixelBufferUnlockBaseAddress(pixels->buffer, pixels->lock_flags); pixels->locks = 0; RETURN((uint32_t)kCVReturnAllocationFailed); }
        memcpy((void *)(uintptr_t)pixels->data, CVPixelBufferGetBaseAddress(pixels->buffer), pixels->bytes);
        RETURN(0);
    }
    if (IS("CVPixelBufferUnlockBaseAddress")) {
        LP32MoviePixels *pixels = OBJ(0); if (!pixels || !pixels->locks) RETURN((uint32_t)kCVReturnInvalidArgument);
        if (--pixels->locks) RETURN(0);
        if (!(pixels->lock_flags & kCVPixelBufferLock_ReadOnly)) memcpy(CVPixelBufferGetBaseAddress(pixels->buffer), (void *)(uintptr_t)pixels->data, pixels->bytes);
        compat_runtime32_deallocate(pixels->data); pixels->data = 0;
        RETURN((uint32_t)CVPixelBufferUnlockBaseAddress(pixels->buffer, pixels->lock_flags));
    }
    if (IS("QTVisualContextTask")) RETURN(0); /* AVFoundation manages output buffers. */
    if (IS("MoviesTask")) {
        /* Decode on AVFoundation's queues, but notify guest code on the
           thread servicing the movie, as QuickTime's task API expects. */
        LP32Movie *movie = OBJ(0);
        service_movie_seek(movie);
        if (movie && movie->drawing_callback &&
            ((movie->drawing_flags & 1) || (movie->visual &&
             [movie->visual->output hasNewPixelBufferForItemTime:movie_time(movie)]))) {
            uint32_t callback_arguments[] = {movie->guest_handle, movie->drawing_refcon};
            compat_runtime32_call(movie->drawing_callback, callback_arguments, 2);
        }
        RETURN(0);
    }
    }
    return 0;
#undef IS
#undef PTR
#undef OBJ
#undef RETURN
}

static bool movie_transport_self_test(LP32Movie *movie)
{
    uint32_t handle = owned(movie), scratch = compat_runtime32_allocate(16, 1);
    if (!scratch) { release_handle(handle); return false; }
    uint32_t *record = (void *)(uintptr_t)scratch;
    uint32_t args[] = {handle, scratch};
    uint64_t result;
    record[0] = 900; record[1] = 0; record[2] = 600; record[3] = handle;
    movie_bridge32_dispatch("_SetMovieTime", args, &result);
    memset(record, 0, 16);
    movie_bridge32_dispatch("_GetMovieTime", args, &result);
    bool ok = result == 900 && record[0] == 900 && record[1] == 0 && record[2] == 600 && record[3] == handle;
    movie_bridge32_dispatch("_StartMovie", args, &result);
    seek_movie(movie, CMTimeSubtract([movie->asset duration], CMTimeMake(60, 600)));
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:5];
    while ([deadline timeIntervalSinceNow] > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        service_movie_seek(movie);
        if (!movie->seek_pending && CMTimeCompare(movie_time(movie), [movie->asset duration]) >= 0 && !movie->player.rate) break;
    }
    ok &= [deadline timeIntervalSinceNow] > 0;
    for (unsigned i = 0; i < 32; ++i) movie_bridge32_dispatch("_GoToBeginningOfMovie", args, &result);
    deadline = [NSDate dateWithTimeIntervalSinceNow:5];
    while ([deadline timeIntervalSinceNow] > 0 && CMTimeGetSeconds(movie_time(movie)) < 0.15) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        service_movie_seek(movie);
    }
    ok &= !movie->seek_pending && movie->player.rate > 0 && CMTimeGetSeconds(movie_time(movie)) >= 0.15;
    for (unsigned i = 0; i < 32; ++i) movie_bridge32_dispatch("_GoToBeginningOfMovie", args, &result);
    movie_bridge32_dispatch("_StopMovie", args, &result);
    deadline = [NSDate dateWithTimeIntervalSinceNow:0.5];
    while ([deadline timeIntervalSinceNow] > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        service_movie_seek(movie);
    }
    ok &= !movie->seek_pending && !movie->player.rate && !movie->play_requested;
    movie_bridge32_dispatch("_StartMovie", args, &result);
    movie_bridge32_dispatch("_GoToBeginningOfMovie", args, &result);
    movie_bridge32_dispatch("_DisposeMovie", args, &result);
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
    service_movie_seek(movie);
    ok &= !movie->player.rate && !movie->play_requested;
    compat_runtime32_deallocate(scratch);
    fprintf(stderr, "movie transport self-test: %s Intel TimeRecord, EOF rewind, repeated seeks, stop/dispose during seek\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int movie_self_test(const char *path, LP32MovieVisual *visual)
{
    @autoreleasepool {
        LP32Movie *movie = open_movie([NSURL fileURLWithPath:[NSString stringWithUTF8String:path]], visual);
        if (!movie) return -1;
        /* Exercise the guest's characteristic-based frame enumeration, not
           just AVFoundation decoding (which can work while TFU skips it). */
        uint32_t scratch = compat_runtime32_allocate(16, 1);
        if (!scratch) return -1;
        uint32_t *words = (void *)(uintptr_t)scratch;
        uint32_t handle = owned(movie);
        uint32_t args[] = {handle, 0x4001, 1, scratch, 0, 0x10000, scratch + 4, scratch + 8};
        uint64_t ignored;
        words[0] = 'eyes';
        movie_bridge32_dispatch("_GetMovieNextInterestingTime", args, &ignored);
        bool timing_ok = (int32_t)words[1] == 0 && (int32_t)words[2] > 0;
        args[1] = 1;
        movie_bridge32_dispatch("_GetMovieNextInterestingTime", args, &ignored);
        timing_ok &= (int32_t)words[1] > 0 && (int32_t)words[2] > 0;
        args[4] = words[1]; args[5] = (uint32_t)-0x10000;
        movie_bridge32_dispatch("_GetMovieNextInterestingTime", args, &ignored);
        timing_ok &= (int32_t)words[1] == 0;
        if ([[movie->asset tracksWithMediaType:AVMediaTypeAudio] count]) {
            words[0] = 'ears'; args[1] = 0x4001; args[4] = 0; args[5] = 0x10000;
            movie_bridge32_dispatch("_GetMovieNextInterestingTime", args, &ignored);
            timing_ok &= (int32_t)words[1] >= 0 && (int32_t)words[2] > 0;
            uint32_t track_args[] = {handle, 1, 'ears', 2};
            movie_bridge32_dispatch("_GetMovieIndTrackType", track_args, &ignored);
            uint32_t track_handle = (uint32_t)ignored;
            timing_ok &= track_handle != 0;
            uint32_t mute_args[] = {track_handle, 1};
            movie_bridge32_dispatch("_SetTrackAudioMute", mute_args, &ignored);
            AVAudioMixInputParameters *input = [movie->item.audioMix.inputParameters firstObject];
            float start_volume = -1, end_volume = -1;
            CMTimeRange range;
            timing_ok &= ignored == 0 && [input getVolumeRampForTime:kCMTimeZero
                startVolume:&start_volume endVolume:&end_volume timeRange:&range] && start_volume == 0;
            mute_args[1] = 0;
            movie_bridge32_dispatch("_SetTrackAudioMute", mute_args, &ignored);
            input = [movie->item.audioMix.inputParameters firstObject];
            timing_ok &= ignored == 0 && [input getVolumeRampForTime:kCMTimeZero
                startVolume:&start_volume endVolume:&end_volume timeRange:&range] && start_volume == 1;
        }
        compat_runtime32_deallocate(scratch);
        release_handle(handle);
        if (!timing_ok) { fprintf(stderr, "movie self-test: FAIL characteristic sample timing\n"); return -1; }
        [movie->player setMuted:YES];
        [movie->player seekToTime:kCMTimeZero toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
        [movie->player seekToTime:kCMTimeZero toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
        [movie->player play];
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:10];
        CVPixelBufferRef buffer = NULL;
        while (!buffer && [deadline timeIntervalSinceNow] > 0 && [movie->player status] != AVPlayerStatusFailed) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
            buffer = [visual->output copyPixelBufferForItemTime:movie_time(movie) itemTimeForDisplay:NULL];
        }
        [movie->player pause];
        if (!buffer) { fprintf(stderr, "movie self-test: FAIL %s\n", [[[movie->player error] description] UTF8String] ?: "no decoded frame"); return -1; }
        CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        size_t bytes = CVPixelBufferGetBytesPerRow(buffer) * CVPixelBufferGetHeight(buffer);
        const uint8_t *data = CVPixelBufferGetBaseAddress(buffer); uint32_t checksum = 2166136261;
        for (size_t i = 0; i < bytes; ++i) checksum = (checksum ^ data[i]) * 16777619;
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        fprintf(stderr, "movie self-test: PASS decoded=%zux%zu bytes=%zu checksum=%08x\n", CVPixelBufferGetWidth(buffer), CVPixelBufferGetHeight(buffer), bytes, checksum);
        CVPixelBufferRelease(buffer);
        if (!movie_transport_self_test(movie)) return -1;
        return 0;
    }
}

int movie_bridge32_self_test(const char *path)
{
    @autoreleasepool {
        LP32MovieVisual *visual = [[[LP32MovieVisual alloc] init] autorelease];
        /* The game reuses one visual context across consecutive movies. */
        if (movie_self_test(path, visual)) return -1;
        return movie_self_test(path, visual);
    }
}
