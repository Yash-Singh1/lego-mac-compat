#include "quicktime_bridge.h"
#include "carbon_bridge.h"
#include "compat_runtime.h"
#include "resource_bridge.h"
#import <AVFoundation/AVFoundation.h>
#import <Carbon/Carbon.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>
#include <limits.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"
/* The removed QuickTime movie API is backed by AVFoundation. Guest pointers
 * are never used as host Movie, GWorld, or TimeBase objects. */
@interface LP32Movie : NSObject {
@public
  AVPlayer *player;
  AVPlayerItemVideoOutput *output;
  AVAsset *asset;
  Rect bounds;
  uint32_t world, callback, callback_data;
  BOOL started;
}
@end
@implementation LP32Movie
- (void)dealloc {
  [player pause];
  [player release];
  [output release];
  [asset release];
  [super dealloc];
}
@end
@interface LP32GraphicsImporter : NSObject {
@public
  CGImageSourceRef source;
  CGImageRef image;
  Rect bounds;
  uint32_t world;
  int32_t matrix[9];
}
@end
@implementation LP32GraphicsImporter
- (void)dealloc {
  if (image)
    CGImageRelease(image);
  if (source)
    CFRelease(source);
  [super dealloc];
}
@end
static NSMutableDictionary *graphics_importers;
static uint32_t next_importer = 0x75200000;
static _Thread_local uint32_t current_world;
static BOOL select_image(LP32GraphicsImporter *importer, size_t index) {
  CGImageRef image =
      CGImageSourceCreateImageAtIndex(importer->source, index, NULL);
  if (!image)
    return NO;
  if (importer->image)
    CGImageRelease(importer->image);
  importer->image = image;
  importer->bounds = (Rect){0, 0, (int16_t)CGImageGetHeight(image),
                            (int16_t)CGImageGetWidth(image)};
  return YES;
}
struct world32 {
  uint32_t token, pixels, stride, format, pixmap, map_data, pixel_state;
  Rect bounds, clip;
  uint16_t foreground[3], background[3];
  struct world32 *next;
};
static struct world32 *worlds;
static uint32_t next_world = 0x75000000;
static NSMutableDictionary *movies, *movie_files;
static uint32_t next_movie = 0x75100000;
static uint16_t next_file = 1;
static struct { uint32_t function, scale, flags, refcon; } task_callbacks[16];
static LP32Movie *movie(uint32_t token) {
  return [movies objectForKey:@(token)];
}
static struct world32 *world(uint32_t token) {
  for (struct world32 *p = worlds; p; p = p->next)
    if (p->token == token)
      return p;
  return NULL;
}
static uint32_t new_movie(NSString *path) {
  LP32Movie *m = [[LP32Movie alloc] init];
  if (path) {
    m->asset = [[AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path]
                                    options:nil] retain];
    if (![m->asset isPlayable]) {
      [m release];
      return 0;
    }
    AVPlayerItem *item = [AVPlayerItem playerItemWithAsset:m->asset];
    m->output =
        [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:@{
          (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)
        }];
    [item addOutput:m->output];
    m->player = [[AVPlayer alloc] initWithPlayerItem:item];
    m->player.muted = getenv("LP32_MUTE_AUDIO") != NULL;
    AVAssetTrack *track =
        [[m->asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
    CGSize size = track.naturalSize;
    m->bounds = (Rect){0, 0, (int16_t)size.height, (int16_t)size.width};
  }
  if (!movies)
    movies = [[NSMutableDictionary alloc] init];
  uint32_t token = next_movie;
  next_movie += 16;
  [movies setObject:m forKey:@(token)];
  [m release];
  for (unsigned i = 0; i < 16; ++i) {
    if (!task_callbacks[i].function) continue;
    uint32_t args[] = {0, 0, task_callbacks[i].refcon};
    compat_runtime32_call(task_callbacks[i].function, args, 3);
  }
  return token;
}
static void movie_task(uint32_t token) {
  LP32Movie *m = movie(token);
  struct world32 *w = m ? world(m->world) : NULL;
  if (!m || !w || !m->output)
    return;
  CMTime time = m->player.currentTime;
  if (![m->output hasNewPixelBufferForItemTime:time])
    return;
  CVPixelBufferRef frame = [m->output copyPixelBufferForItemTime:time
                                              itemTimeForDisplay:NULL];
  if (!frame)
    return;
  CVPixelBufferLockBaseAddress(frame, kCVPixelBufferLock_ReadOnly);
  const unsigned char *source = CVPixelBufferGetBaseAddress(frame);
  size_t width = CVPixelBufferGetWidth(frame),
         height = CVPixelBufferGetHeight(frame),
         stride = CVPixelBufferGetBytesPerRow(frame);
  size_t dw = (uint16_t)(w->bounds.right - w->bounds.left),
         dh = (uint16_t)(w->bounds.bottom - w->bounds.top);
  if (width && height && dw && dh && w->stride >= dw * 4) {
    for (size_t y = 0; y < dh; ++y) {
      unsigned char *dest = (void *)(uintptr_t)(w->pixels + y * w->stride);
      const unsigned char *row = source + (y * height / dh) * stride;
      for (size_t x = 0; x < dw; ++x) {
        const unsigned char *p = row + (x * width / dw) * 4;
        if (w->format == k32BGRAPixelFormat)
          memcpy(dest + x * 4, p, 4);
        else if (w->format == k32RGBAPixelFormat) {
          dest[x * 4] = p[2];
          dest[x * 4 + 1] = p[1];
          dest[x * 4 + 2] = p[0];
          dest[x * 4 + 3] = p[3];
        } else {
          dest[x * 4] = p[3];
          dest[x * 4 + 1] = p[2];
          dest[x * 4 + 2] = p[1];
          dest[x * 4 + 3] = p[0];
        }
      }
    }
    if (m->callback) {
      uint32_t a[] = {token, m->callback_data};
      compat_runtime32_call(m->callback, a, 2);
    }
  }
  CVPixelBufferUnlockBaseAddress(frame, kCVPixelBufferLock_ReadOnly);
  CVPixelBufferRelease(frame);
}
int quicktime_bridge32_dispatch(const char *name, const uint32_t *a,
                                uint64_t *out) {
#define IS(s) (!strcmp(name, s))
#define P(i) ((void *)(uintptr_t)a[i])
  if (name[0] != '_' || !strchr("CEGMNODQISLURPFB", name[1]))
    return 0;
  @autoreleasepool {
    struct world32 *current = world(current_world);
    if (current && IS("_ClipRect")) {
      current->clip = *(Rect *)P(0);
      *out = 0;
      return 1;
    }
    if (current && IS("_SetClip")) {
      if (!carbon_bridge32_region_bounds(a[0], &current->clip))
        return 0;
      *out = 0;
      return 1;
    }
    if (current && IS("_GetClip")) {
      Rect r = current->clip;
      uint32_t args[] = {a[0], (uint32_t)(int32_t)r.left,
                         (uint32_t)(int32_t)r.top, (uint32_t)(int32_t)r.right,
                         (uint32_t)(int32_t)r.bottom};
      return carbon_bridge32_dispatch("_SetRectRgn", args, out);
    }
    if (IS("_GetPort") && current) {
      if (a[0])
        *(uint32_t *)P(0) = current_world;
      *out = 0;
      return 1;
    }
    if (IS("_SetPort") && world(a[0])) {
      current_world = a[0];
      *out = 0;
      return 1;
    }
    if (current && (IS("_RGBForeColor") || IS("_RGBBackColor") ||
                    IS("_GetForeColor") || IS("_GetBackColor"))) {
      uint16_t *color = IS("_RGBForeColor") || IS("_GetForeColor")
                            ? current->foreground
                            : current->background;
      if (IS("_GetForeColor") || IS("_GetBackColor"))
        memcpy(P(0), color, 6);
      else
        memcpy(color, P(0), 6);
      *out = 0;
      return 1;
    }
    if (current && (IS("_EraseRect") || IS("_PaintRect"))) {
      Rect rect = *(Rect *)P(0);
      const uint16_t *color =
          IS("_EraseRect") ? current->background : current->foreground;
      rect.left = MAX(rect.left, current->clip.left);
      rect.top = MAX(rect.top, current->clip.top);
      rect.right = MIN(rect.right, current->clip.right);
      rect.bottom = MIN(rect.bottom, current->clip.bottom);
      int width = current->bounds.right - current->bounds.left,
          height = current->bounds.bottom - current->bounds.top;
      for (int y = MAX(0, rect.top - current->bounds.top);
           y < MIN(height, rect.bottom - current->bounds.top); ++y)
        for (int x = MAX(0, rect.left - current->bounds.left);
             x < MIN(width, rect.right - current->bounds.left); ++x) {
          uint8_t *dest = (void *)(uintptr_t)(current->pixels +
                                              y * current->stride + x * 4);
          uint8_t r = color[0] >> 8, g = color[1] >> 8, b = color[2] >> 8;
          if (current->format == k32BGRAPixelFormat) {
            dest[0] = b;
            dest[1] = g;
            dest[2] = r;
            dest[3] = 255;
          } else if (current->format == k32RGBAPixelFormat) {
            dest[0] = r;
            dest[1] = g;
            dest[2] = b;
            dest[3] = 255;
          } else {
            dest[0] = 255;
            dest[1] = r;
            dest[2] = g;
            dest[3] = b;
          }
        }
      *out = 0;
      return 1;
    }
    if (IS("_SetIdentityMatrix")) {
      int32_t identity[9] = {1 << 16, 0, 0, 0, 1 << 16, 0, 0, 0, 1 << 30};
      memcpy(P(0), identity, sizeof(identity));
      *out = 0;
      return 1;
    }
    if (IS("_GetPixelsState") || IS("_SetPixelsState") || IS("_LockPixels") ||
        IS("_UnlockPixels") || IS("_GetPixBaseAddr")) {
      struct world32 *w = worlds;
      while (w && w->pixmap != a[0])
        w = w->next;
      if (!w)
        return 0;
      if (IS("_GetPixelsState"))
        *out = w->pixel_state;
      else if (IS("_GetPixBaseAddr"))
        *out = w->pixels;
      else if (IS("_LockPixels")) {
        w->pixel_state |= 0x80;
        *out = 1;
      } else {
        if (IS("_SetPixelsState"))
          w->pixel_state = a[1];
        else
          w->pixel_state &= ~0x80u;
        *out = 0;
      }
      return 1;
    }
    if (IS("_GetGraphicsImporterForFile")) {
      char path[PATH_MAX];
      int status = carbon_bridge32_path_from_spec(a[0], path, sizeof(path));
      uint32_t token = 0;
      if (!status) {
        LP32GraphicsImporter *importer = [[LP32GraphicsImporter alloc] init];
        importer->source = CGImageSourceCreateWithURL(
            (CFURLRef)
                [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]],
            NULL);
        if (importer->source && select_image(importer, 0)) {
          importer->matrix[0] = importer->matrix[4] = 1 << 16;
          importer->matrix[8] = 1 << 30;
          if (!graphics_importers)
            graphics_importers = [[NSMutableDictionary alloc] init];
          token = next_importer;
          next_importer += 16;
          [graphics_importers setObject:importer forKey:@(token)];
        } else
          status = -2048;
        [importer release];
      }
      if (a[1])
        *(uint32_t *)P(1) = token;
      *out = (uint32_t)status;
      return 1;
    }
    LP32GraphicsImporter *importer = [graphics_importers objectForKey:@(a[0])];
    if (importer) {
      if (IS("_CloseComponent")) {
        [graphics_importers removeObjectForKey:@(a[0])];
        *out = 0;
        return 1;
      }
      if (IS("_GraphicsImportGetNaturalBounds")) {
        *(Rect *)P(1) = (Rect){0, 0, (int16_t)CGImageGetHeight(importer->image),
                               (int16_t)CGImageGetWidth(importer->image)};
        *out = 0;
        return 1;
      }
      if (IS("_GraphicsImportSetBoundsRect")) {
        importer->bounds = *(Rect *)P(1);
        *out = 0;
        return 1;
      }
      if (IS("_GraphicsImportSetGWorld")) {
        importer->world = a[1];
        *out = 0;
        return 1;
      }
      if (IS("_GraphicsImportGetImageCount")) {
        *(uint32_t *)P(1) = (uint32_t)CGImageSourceGetCount(importer->source);
        *out = 0;
        return 1;
      }
      if (IS("_GraphicsImportSetImageIndex")) {
        *out = a[1] && select_image(importer, a[1] - 1) ? 0 : (uint32_t)-50;
        return 1;
      }
      if (IS("_GraphicsImportGetQuality")) {
        *(uint32_t *)P(1) = 0x200;
        *out = 0;
        return 1;
      }
      if (IS("_GraphicsImportGetMatrix") ||
          IS("_GraphicsImportGetDefaultMatrix")) {
        int32_t identity[9] = {1 << 16, 0, 0, 0, 1 << 16, 0, 0, 0, 1 << 30};
        memcpy(P(1),
               IS("_GraphicsImportGetMatrix") ? importer->matrix : identity,
               sizeof(identity));
        *out = 0;
        return 1;
      }
      if (IS("_GraphicsImportSetMatrix")) {
        memcpy(importer->matrix, P(1), sizeof(importer->matrix));
        *out = 0;
        return 1;
      }
      if (IS("_GraphicsImportGetImageDescription")) {
        uint32_t args[] = {86};
        uint64_t handle = 0;
        resource_bridge32_dispatch("_NewHandleClear", args, &handle);
        if (!handle) {
          *out = (uint32_t)-108;
          return 1;
        }
        unsigned char *desc = (void *)(uintptr_t)*(uint32_t *)(uintptr_t)handle;
        uint32_t size = 86, type = 0x72617720, res = 72 << 16;
        uint16_t width = CGImageGetWidth(importer->image),
                 height = CGImageGetHeight(importer->image), frames = 1,
                 depth = 32, clut = 0xffff;
        memcpy(desc, &size, 4);
        memcpy(desc + 4, &type, 4);
        memcpy(desc + 32, &width, 2);
        memcpy(desc + 34, &height, 2);
        memcpy(desc + 36, &res, 4);
        memcpy(desc + 40, &res, 4);
        memcpy(desc + 48, &frames, 2);
        memcpy(desc + 82, &depth, 2);
        memcpy(desc + 84, &clut, 2);
        *(uint32_t *)P(1) = (uint32_t)handle;
        *out = 0;
        return 1;
      }
      if (IS("_GraphicsImportDraw")) {
        struct world32 *w =
            world(importer->world ? importer->world : current_world);
        if (!w) {
          *out = (uint32_t)-50;
          return 1;
        }
        size_t width = w->bounds.right - w->bounds.left,
               height = w->bounds.bottom - w->bounds.top;
        CGBitmapInfo info =
            w->format == k32BGRAPixelFormat
                ? kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst
            : w->format == k32RGBAPixelFormat
                ? kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast
                : kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedFirst;
        CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
        CGContextRef context =
            CGBitmapContextCreate((void *)(uintptr_t)w->pixels, width, height,
                                  8, w->stride, color, info);
        CGColorSpaceRelease(color);
        if (!context) {
          *out = (uint32_t)-50;
          return 1;
        }
        CGContextTranslateCTM(context, 0, height);
        CGContextScaleCTM(context, 1, -1);
        Rect clip = w->clip;
        CGContextClipToRect(context,
                            CGRectMake(clip.left - w->bounds.left,
                                       clip.top - w->bounds.top,
                                       MAX(0, clip.right - clip.left),
                                       MAX(0, clip.bottom - clip.top)));
        const int32_t *m = importer->matrix;
        CGContextConcatCTM(
            context, CGAffineTransformMake(m[0] / 65536.0, m[1] / 65536.0,
                                           m[3] / 65536.0, m[4] / 65536.0,
                                           m[6] / 65536.0, m[7] / 65536.0));
        Rect b = importer->bounds;
        CGContextSetBlendMode(context, kCGBlendModeCopy);
        CGContextDrawImage(context,
                           CGRectMake(b.left - w->bounds.left,
                                      b.top - w->bounds.top, b.right - b.left,
                                      b.bottom - b.top),
                           importer->image);
        CGContextRelease(context);
        *out = 0;
        return 1;
      }
    }
    if (IS("_SetGWorld")) {
      current_world = a[0];
      *out = 0;
      return 1;
    }
    if (IS("_GetGWorld")) {
      if (a[0])
        *(uint32_t *)P(0) = current_world;
      if (a[1])
        *(uint32_t *)P(1) = 0;
      *out = 0;
      return 1;
    }
    if (IS("_GetCompressionInfo")) {
      /* Sound Manager's packed 20-byte CompressionInfo record. COD4 asks
         about uncompressed stereo PCM during platform initialization. */
      struct __attribute__((packed)) {
        uint32_t size, format;
        int16_t compression;
        uint16_t samples, packet_bytes, frame_bytes, sample_bytes, reserved;
      } info = {0};
      if (!a[4] || *(uint32_t *)P(4) < sizeof(info) || !a[2] || a[2] > 32 ||
          !a[3] || a[3] > 32 || (a[3] & 7)) {
        *out = (uint32_t)paramErr; return 1;
      }
      if (a[0] || a[1] != 'NONE') { *out = (uint32_t)unimpErr; return 1; }
      info.size = sizeof(info);
      info.format = a[1];
      info.samples = 1;
      info.packet_bytes = info.sample_bytes = a[3] / 8;
      info.frame_bytes = info.sample_bytes * a[2];
      memcpy(P(4), &info, sizeof(info));
      *out = 0; return 1;
    }
    if (IS("_QTGetTimeUntilNextTask")) {
      if (!a[0] || (int32_t)a[1] <= 0) { *out = (uint32_t)paramErr; return 1; }
      /* AVFoundation decodes asynchronously; MoviesTask presents its frames. */
      *(int32_t *)P(0) = [movies count] ? (int32_t)(((uint64_t)a[1] + 59) / 60) : (int32_t)a[1];
      *out = 0;
      return 1;
    }
    if (IS("_QTInstallNextTaskNeededSoonerCallback")) {
      if (!a[0] || (int32_t)a[1] <= 0) { *out = (uint32_t)paramErr; return 1; }
      for (unsigned i = 0; i < 16; ++i) {
        if (task_callbacks[i].function && (task_callbacks[i].function != a[0] || task_callbacks[i].refcon != a[3])) continue;
        task_callbacks[i].function = a[0]; task_callbacks[i].scale = a[1];
        task_callbacks[i].flags = a[2]; task_callbacks[i].refcon = a[3];
        *out = 0;
        return 1;
      }
      *out = (uint32_t)memFullErr;
      return 1;
    }
    if (IS("_QTUninstallNextTaskNeededSoonerCallback")) {
      for (unsigned i = 0; i < 16; ++i)
        if (task_callbacks[i].function == a[0] && task_callbacks[i].refcon == a[1])
          memset(&task_callbacks[i], 0, sizeof(task_callbacks[i]));
      *out = 0;
      return 1;
    }
    if (IS("_EnterMovies") || IS("_ExitMovies")) {
      *out = 0;
      return 1;
    }
    if (IS("_QTNewDataReferenceFromFSRef")) {
      if (!a[0] || !a[2] || !a[3]) { *out = (uint32_t)paramErr; return 1; }
      UInt8 path[PATH_MAX]; OSStatus status = FSRefMakePath(P(0), path, sizeof(path));
      if (!status) {
        NSString *url = [[NSURL fileURLWithPath:[NSString stringWithUTF8String:(char *)path]] absoluteString];
        const char *bytes = url.UTF8String; uint32_t size = (uint32_t)strlen(bytes) + 1;
        uint64_t handle = 0; resource_bridge32_dispatch("_NewHandle", &size, &handle);
        if (!handle) status = memFullErr;
        else {
          memcpy((void *)(uintptr_t)*(uint32_t *)(uintptr_t)handle, bytes, size);
          *(uint32_t *)P(2) = (uint32_t)handle; *(uint32_t *)P(3) = 0x75726c20; /* url */
        }
      }
      *out = (uint32_t)status; return 1;
    }
    if (IS("_NewMovieFromDataRef")) {
      if (!a[0] || !a[3] || a[4] != 0x75726c20) { *out = (uint32_t)paramErr; return 1; }
      uint64_t size = 0; resource_bridge32_dispatch("_GetHandleSize", a+3, &size);
      const char *bytes = (const void *)(uintptr_t)*(uint32_t *)P(3);
      NSURL *url = bytes && size && memchr(bytes, 0, size) ? [NSURL URLWithString:[NSString stringWithUTF8String:bytes]] : nil;
      uint32_t token = url.isFileURL ? new_movie(url.path) : 0;
      *(uint32_t *)P(0) = token;
      if (a[2]) *(int16_t *)P(2) = 0;
      *out = token ? 0 : (uint32_t)-2048; return 1;
    }
    if (IS("_GetTrackMedia") || IS("_GetMediaHandler") || IS("_GetMediaSampleDescription") ||
        IS("_SetTrackVolume") || IS("_MediaSetSoundBalance") || IS("_GetMediaDuration")) {
      uint32_t parent = a[0] & ~15u; LP32Movie *owner = movie(parent);
      unsigned kind = a[0] & 15u;
      if (!owner) { *out=(uint32_t)paramErr;return 1; }
      AVAssetTrack *track = kind == 1 ? [[owner->asset tracksWithMediaType:AVMediaTypeAudio] firstObject] :
                           kind == 2 ? [[owner->asset tracksWithMediaType:AVMediaTypeVideo] firstObject] : nil;
      if (!owner || !track) { *out = (uint32_t)paramErr; return 1; }
      if (IS("_GetTrackMedia") || IS("_GetMediaHandler")) *out = a[0];
      else if (IS("_SetTrackVolume")) { owner->player.volume = fmaxf(0,fminf(1,(int16_t)a[1]/256.0f)); *out=0; }
      else if (IS("_MediaSetSoundBalance")) *out = (int16_t)a[1] ? (uint32_t)unimpErr : 0;
      else if (IS("_GetMediaDuration")) *out = (uint32_t)CMTimeConvertScale(track.timeRange.duration,track.naturalTimeScale,kCMTimeRoundingMethod_Default).value;
      else {
        if (kind != 1 || a[1] != 1 || !a[2]) { *out=(uint32_t)paramErr;return 1; }
        CMAudioFormatDescriptionRef format = (CMAudioFormatDescriptionRef)[[track formatDescriptions] firstObject];
        const AudioStreamBasicDescription *asbd = format ? CMAudioFormatDescriptionGetStreamBasicDescription(format) : NULL;
        if (!asbd || !isfinite(asbd->mSampleRate) || asbd->mSampleRate < 0 || asbd->mSampleRate >= 65536) { *out=(uint32_t)paramErr;return 1; }
        uint32_t resize[]={a[2],36};uint64_t ignored;resource_bridge32_dispatch("_SetHandleSize",resize,&ignored);
        resource_bridge32_dispatch("_GetHandleSize",resize,&ignored);
        if ((int32_t)ignored < 36) { *out=(uint32_t)memFullErr;return 1; }
        unsigned char *description=(void *)(uintptr_t)*(uint32_t *)P(2);
        if(!description){*out=(uint32_t)memFullErr;return 1;}
        memset(description,0,36);uint32_t size=36,type=asbd->mFormatID,rate=(uint32_t)(asbd->mSampleRate*65536.0);
        uint16_t channels=(uint16_t)asbd->mChannelsPerFrame,bits=(uint16_t)(asbd->mBitsPerChannel?asbd->mBitsPerChannel:16),ref=1;
        memcpy(description,&size,4);memcpy(description+4,&type,4);memcpy(description+14,&ref,2);
        memcpy(description+24,&channels,2);memcpy(description+26,&bits,2);memcpy(description+32,&rate,4);*out=0;
      }
      return 1;
    }
    if (IS("_OpenMovieFile")) {
      char path[PATH_MAX];
      int status = carbon_bridge32_path_from_spec(a[0], path, sizeof(path));
      if (!status && access(path, R_OK))
        status = -43;
      if (!status) {
        if (!movie_files)
          movie_files = [[NSMutableDictionary alloc] init];
        uint16_t token = next_file++;
        [movie_files setObject:[NSString stringWithUTF8String:path]
                        forKey:@(token)];
        if (a[1])
          *(int16_t *)P(1) = (int16_t)token;
      }
      *out = (uint32_t)status;
      return 1;
    }
    if (IS("_CloseMovieFile")) {
      [movie_files removeObjectForKey:@((uint16_t)a[0])];
      *out = 0;
      return 1;
    }
    if (IS("_NewMovieFromFile")) {
      NSString *path = [movie_files objectForKey:@((uint16_t)a[1])];
      uint32_t token = path ? new_movie(path) : 0;
      if (a[0])
        *(uint32_t *)P(0) = token;
      if (a[5])
        *(uint8_t *)P(5) = 0;
      *out = token ? 0 : (uint32_t)-2048;
      return 1;
    }
    if (IS("_NewMovie")) {
      *out = new_movie(nil);
      return 1;
    }
    if (IS("_DisposeMovie")) {
      [movies removeObjectForKey:@(a[0])];
      *out = 0;
      return 1;
    }
    if (IS("_MoviesTask")) {
      if (a[0]) movie_task(a[0]);
      else for (NSNumber *token in [movies allKeys]) movie_task([token unsignedIntValue]);
      *out = 0;
      return 1;
    }
    if (IS("_NewGWorldFromPtr") || IS("_QTNewGWorldFromPtr")) {
      if (a[0])
        *(uint32_t *)P(0) = 0;
      if (!a[0] || !a[2] || !a[6]) {
        *out = (uint32_t)-50;
        return 1;
      }
      Rect bounds = *(Rect *)P(2);
      int width = bounds.right - bounds.left,
          height = bounds.bottom - bounds.top;
      if ((a[1] != 32 && a[1] != k32ARGBPixelFormat &&
           a[1] != k32BGRAPixelFormat && a[1] != k32RGBAPixelFormat) ||
          width <= 0 || height <= 0 || a[7] < (uint32_t)width * 4 ||
          (uint64_t)a[6] + (uint64_t)a[7] * height > UINT32_MAX) {
        *out = (uint32_t)-50;
        return 1;
      }
      struct world32 *w = calloc(1, sizeof(*w));
      if (!w) {
        *out = (uint32_t)-108;
        return 1;
      }
      w->token = next_world;
      next_world += 16;
      w->format = a[1];
      w->bounds = *(Rect *)P(2);
      w->background[0] = w->background[1] = w->background[2] = 65535;
      w->clip = w->bounds;
      w->pixels = a[6];
      w->stride = a[7];
      w->pixmap = compat_runtime32_allocate(4, 1);
      w->map_data = compat_runtime32_allocate(50, 1);
      if (!w->pixmap || !w->map_data) {
        compat_runtime32_deallocate(w->pixmap);
        compat_runtime32_deallocate(w->map_data);
        free(w);
        *out = (uint32_t)-108;
        return 1;
      }
      *(uint32_t *)(uintptr_t)w->pixmap = w->map_data;
      unsigned char *p = (void *)(uintptr_t)w->map_data;
      memcpy(p, &w->pixels, 4);
      uint16_t rowbytes = (uint16_t)(0x8000 | (w->stride & 0x3fff));
      memcpy(p + 4, &rowbytes, 2);
      memcpy(p + 6, &w->bounds, 8);
      uint16_t pixel[4] = {16, 32, 3, 8};
      memcpy(p + 30, pixel, 8);
      memcpy(p + 38, &w->format, 4);
      w->next = worlds;
      worlds = w;
      if (a[0])
        *(uint32_t *)P(0) = w->token;
      *out = 0;
      return 1;
    }
    if (IS("_DisposeGWorld")) {
      for (struct world32 **p = &worlds; *p; p = &(*p)->next)
        if ((*p)->token == a[0]) {
          struct world32 *w = *p;
          *p = w->next;
          compat_runtime32_deallocate(w->pixmap);
          compat_runtime32_deallocate(w->map_data);
          free(w);
          break;
        }
      *out = 0;
      return 1;
    }
    if (IS("_GetGWorldPixMap")) {
      struct world32 *w = world(a[0]);
      *out = w ? w->pixmap : 0;
      return 1;
    }
    if (IS("_IsValidPort") && world(a[0])) {
      *out = 1;
      return 1;
    }
    if (IS("_GetPortBounds") && world(a[0])) {
      if (a[1])
        *(Rect *)P(1) = world(a[0])->bounds;
      *out = a[1];
      return 1;
    }
    LP32Movie *m = movie(a[0]);
    if (!m)
      return 0;
    if (IS("_GetMovieBox")) {
      if (a[1])
        *(Rect *)P(1) = m->bounds;
      *out = 0;
      return 1;
    }
    if (IS("_SetMovieBox")) {
      m->bounds = *(Rect *)P(1);
      *out = 0;
      return 1;
    }
    if (IS("_SetMovieGWorld")) {
      m->world = a[1];
      *out = 0;
      return 1;
    }
    if (IS("_SetMovieDrawingCompleteProc")) {
      m->callback = a[2];
      m->callback_data = a[3];
      *out = 0;
      return 1;
    }
    if (IS("_GetMovieIndTrackType")) {
      unsigned kind = a[2] == 0x65617273 || a[2] == 0x736f756e ? 1 : a[2] == 0x65796573 || a[2] == 0x76696465 ? 2 : 0;
      NSArray *tracks = kind ? [m->asset tracksWithMediaType:kind == 1 ? AVMediaTypeAudio : AVMediaTypeVideo] : nil;
      *out = a[1] == 1 && tracks.count ? a[0] + kind : 0; return 1;
    }
    if (IS("_GetMoviePreferredRate")) { *out=65536;return 1; }
    if (IS("_GetMovieDuration")) { *out=(uint32_t)CMTimeConvertScale(m->asset.duration,600,kCMTimeRoundingMethod_Default).value;return 1; }
    if (IS("_PrerollMovie")) { *out=m->player.status == AVPlayerStatusFailed ? (uint32_t)-2048 : 0;return 1; }
    if (IS("_SetMovieRate")) { m->started = YES; m->player.rate = (int32_t)a[1]/65536.0f; *out=0;return 1; }
    if (IS("_StartMovie")) {
      m->started = YES;
      [m->player play];
      *out = 0;
      return 1;
    }
    if (IS("_StopMovie")) {
      [m->player pause];
      *out = 0;
      return 1;
    }
    if (IS("_GoToBeginningOfMovie") || IS("_SetMovieTimeValue")) {
      [m->player seekToTime:CMTimeMake(
                                IS("_GoToBeginningOfMovie") ? 0 : (int32_t)a[1],
                                600)
            toleranceBefore:kCMTimeZero
             toleranceAfter:kCMTimeZero];
      *out = 0;
      return 1;
    }
    if (IS("_IsMovieDone")) {
      *out = m->player.status == AVPlayerStatusFailed ||
             (m->started &&
              CMTimeCompare(m->player.currentTime, m->asset.duration) >= 0);
      return 1;
    }
    if (IS("_GetMovieTimeScale")) {
      *out = 600;
      return 1;
    }
    if (IS("_GetMovieTimeBase")) {
      *out = a[0];
      return 1;
    }
    if (IS("_GetMovieTime") || IS("_GetTimeBaseStartTime") ||
        IS("_GetTimeBaseStopTime")) {
      int64_t value =
          IS("_GetTimeBaseStartTime")
              ? 0
              : CMTimeConvertScale(IS("_GetTimeBaseStopTime")
                                       ? m->asset.duration
                                       : m->player.currentTime,
                                   600, kCMTimeRoundingMethod_Default)
                    .value;
      unsigned record_arg = IS("_GetMovieTime") ? 1 : 2;
      int32_t scale = IS("_GetMovieTime") ? 600 : (int32_t)a[1];
      if (scale > 0 && scale != 600)
        value = value * scale / 600;
      if (a[record_arg]) {
        uint32_t record[] = {(uint32_t)(value >> 32), (uint32_t)value,
                             (uint32_t)scale, a[0]};
        memcpy(P(record_arg), record, sizeof(record));
      }
      *out = (uint32_t)value;
      return 1;
    }
  }
  return 0;
#undef IS
#undef P
}
