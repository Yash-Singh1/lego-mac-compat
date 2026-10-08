/* Metal backend: devices, command submission, render passes, pipelines and
 * resources.
 *
 * Conventions: every Metal texture stores GL's rows in GL order (row 0 is
 * GL's bottom row). Shaders flip clip-space Y so rasterization writes GL
 * row r to Metal row r; viewport, scissor and glReadPixels rectangles then
 * need no conversion, and only presentation to a CAMetalLayer flips. */
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/message.h>

#include "ffgen.h"
#include "shader_depth.h"

static void remember_function_source(id<MTLFunction> function, NSString *source);

/* -[MTLBuffer contents] returns an inner pointer, so ARC retains and
   autoreleases the buffer at every property access; the buffers here are
   owned elsewhere. */
static inline void *mtl_contents(__unsafe_unretained id<MTLBuffer> buffer)
{
    return ((void *(*)(id, SEL))objc_msgSend)(buffer, @selector(contents));
}

struct glm_backend_context;
struct draw_stages;
static void *transient_alloc_raw(struct glm_backend_context *b, NSUInteger length, NSUInteger *offset);
static void *prepare_draw_raw(struct glm_context *ctx, const struct draw_stages *st, uint32_t metal_primitive,
                              uint32_t primitive_count, uint32_t instances, MTLVertexDescriptor *layout,
                              uint32_t layout_id, bool capture);
#define transient_alloc(b, length, offset) ((__bridge id<MTLBuffer>)transient_alloc_raw((b), (length), (offset)))
/* Fragment variant bit (with the alpha test function): flat colour inputs. */
enum { GLM_VARIANT_FLAT_COLORS = 0x100 };
#define prepare_draw(...) ((__bridge id<MTLRenderCommandEncoder>)prepare_draw_raw(__VA_ARGS__))
#include "glm_internal.h"
#define GLM_APPLE_TYPES_ONLY
#include "glm_apple_gets.h"
#include "legacy_block.h"
#include "programs.h"
#include "arb_program.h"
#include "queries.h"
#include "client.h"

#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>

static id<MTLDevice> device;

/* Queues explicitly prepared by the host before context creation. */
static NSMutableArray<id<MTLCommandQueue>> *warm_queues;
static pthread_mutex_t warm_lock = PTHREAD_MUTEX_INITIALIZER;

static bool warm_up(unsigned queue_count, unsigned submissions)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    if (!target) return false;
    warm_queues = [NSMutableArray array];
    for (unsigned q = 0; q < queue_count; ++q) {
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) return false;
        NSMutableArray *buffers = [NSMutableArray array];
        for (unsigned i = 0; i < submissions; ++i) {
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            if (!commands) return false;
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
            [[commands blitCommandEncoder] endEncoding];
            [commands commit];
            [buffers addObject:commands];
        }
        for (id<MTLCommandBuffer> commands in buffers) [commands waitUntilCompleted];
        [warm_queues addObject:queue];
    }
    return true;
}

bool glm_backend_initialize(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        device = MTLCreateSystemDefaultDevice();
    });
    return device != nil;
}

GLM_EXPORT bool glmetal_prewarm_command_queues(unsigned queues, unsigned submissions)
{
    if (!queues || queues > 64 || !submissions || submissions > 64 || !glm_backend_initialize()) return false;
    static dispatch_once_t once;
    static bool ready;
    dispatch_once(&once, ^{ @autoreleasepool { ready = warm_up(queues, submissions); } });
    return ready;
}

void *glm_backend_device(void) { return (__bridge void *)device; }

void *glm_backend_new_queue(void)
{
    id<MTLCommandQueue> queue = nil;
    pthread_mutex_lock(&warm_lock);
    queue = warm_queues.lastObject;
    if (queue) [warm_queues removeLastObject];
    pthread_mutex_unlock(&warm_lock);
    if (!queue) queue = [device newCommandQueue];
    return (__bridge_retained void *)queue;
}

void glm_backend_release(void *object)
{
    if (object) CFRelease(object);
}

const char *glm_renderer_name(void)
{
    static char name[128];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        glm_backend_initialize();
        snprintf(name, sizeof name, "%s", device ? device.name.UTF8String : "Apple GPU");
    });
    return name;
}

/* GLMETAL_STALLS=1: compiles and pipeline builds on the draw path, which
   show as hitches in games (summed per second). */
/* Time spent per kind of stall since the last present: a frame over
   250 ms reports what GLMetal did during it (always on; the application's
   log then says whether a hitch was compiling, pipelines, ...). */
static struct { const char *what; unsigned count; double ms; } frame_stalls[16];
static pthread_mutex_t frame_stalls_lock = PTHREAD_MUTEX_INITIALIZER;

void glm_note_stall(const char *what, uint64_t started)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("GLMETAL_STALLS") != NULL;
    uint64_t elapsed = glm_now_ns() - started;
    double ms = elapsed / 1e6;
    if (enabled && ms >= 2) fprintf(stderr, "glmetal: stall %.1f ms %s\n", ms, what);
    pthread_mutex_lock(&frame_stalls_lock);
    for (int i = 0; i < 16; ++i)
        if (frame_stalls[i].what == what || !frame_stalls[i].what) {
            frame_stalls[i].what = what;
            ++frame_stalls[i].count;
            frame_stalls[i].ms += ms;
            break;
        }
    pthread_mutex_unlock(&frame_stalls_lock);
}

static void report_slow_frame(void)
{
    static uint64_t last;
    uint64_t now = glm_now_ns();
    double frame_ms = last ? (now - last) / 1e6 : 0;
    last = now;
    pthread_mutex_lock(&frame_stalls_lock);
    if (frame_ms > 250) {
        char line[1024];
        int n = snprintf(line, sizeof line, "glmetal: slow frame %.0f ms:", frame_ms);
        for (int i = 0; i < 16 && frame_stalls[i].what && n < (int)sizeof line - 80; ++i)
            if (frame_stalls[i].count)
                n += snprintf(line + n, sizeof line - (size_t)n, " %s x%u %.0f ms;", frame_stalls[i].what,
                              frame_stalls[i].count, frame_stalls[i].ms);
        fprintf(stderr, "%s\n", line);
    }
    for (int i = 0; i < 16; ++i) {
        frame_stalls[i].count = 0;
        frame_stalls[i].ms = 0;
    }
    pthread_mutex_unlock(&frame_stalls_lock);
}


/* Buffer contents versions, unique across buffers (a recycled MTLBuffer
   address never matches an old memo). */
static uint64_t next_content_generation(void)
{
    static uint64_t counter;
    return __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);
}

/* ---- per-context state --------------------------------------------------- */

/* Which code runs each stage: the GLSL program, else an enabled ARB
   program, else fixed function (GL 2.1 2.15, ARB_*_program). */
struct draw_stages {
    struct glm_program *program, *vertex_program, *fragment_program;
    struct glm_arb_program *arb_vertex, *arb_fragment;
    bool ff_vertex, ff_fragment, core;
};

struct glm_backend_context {
    struct glm_context *ctx;
    id<MTLCommandQueue> queue;
    id<MTLCommandBuffer> commands;
    dispatch_group_t completions; /* callbacks must finish before this context is freed */
    id<MTLRenderCommandEncoder> encoder;
    bool framebuffer_dirty;
    bool pass_srgb;           /* GL_FRAMEBUFFER_SRGB the open pass was built for */
    __strong id<MTLTexture> linear_views[8]; /* render_view: recent linear views of sRGB attachments */
    int linear_view_next;
    /* Attachments of the open render pass. */
    id<MTLTexture> color[GLM_MAX_DRAW_BUFFERS];
    id<MTLTexture> depth, stencil;
    NSUInteger width, height, samples;
    /* Separate depth and stencil images Metal cannot pair in one pass (one
       of them a combined format): the pass renders into a combined scratch
       texture, filled from them when it opens and copied back when it ends. */
    struct {
        id<MTLTexture> scratch, depth, stencil;
        NSUInteger dslice, dlevel, sslice, slevel, width, height;
    } mixed;
    id<MTLTexture> mixed_cache;
    /* Transient vertex/index/uniform memory for the open command buffer. */
    id<MTLBuffer> transient;
    NSUInteger transient_used;
    NSMutableArray *transients;
    NSMutableArray<id<MTLBuffer>> *retiring; /* buffer storage replaced since the last commit (storage_pool) */
    NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *pipelines;
    NSMutableDictionary<NSData *, id<MTLFunction>> *ff_vertex, *ff_fragment;
    NSMutableDictionary<NSData *, id<MTLDepthStencilState>> *depth_states;
    NSMutableDictionary<NSData *, id<MTLSamplerState>> *samplers;
    id<MTLLibrary> utility_library;
    id<MTLFunction> clear_vertex, clear_fragment;
    id<MTLFunction> clear_typed[3][GLM_MAX_DRAW_BUFFERS]; /* glClearBuffer: float, int, uint per draw buffer */
    int clear_only;          /* >= 0: clear pipelines write only this draw buffer */
    id<MTLFunction> present_vertex, present_fragment;
    id<MTLRenderPipelineState> present_pipeline;
    MTLPixelFormat present_format;
    /* Default framebuffer storage. */
    id<MTLTexture> default_color, default_depth;
    uint64_t submitted_serial;
    volatile uint64_t completed_serial;
    /* Occlusion queries: a visibility buffer per command buffer, attached
       to render passes started while a query is active. */
    id<MTLBuffer> visibility;
    NSUInteger visibility_used;
    uint64_t pass_id;
    bool pass_has_visibility;
    int64_t counting_offset; /* -1: counting disabled on the open encoder */
    /* State already set on the open encoder, to skip redundant calls. */
    struct encoder_cache {
        __unsafe_unretained id pipeline, depth_state;
        bool raster_valid;
        MTLWinding winding;
        MTLCullMode cull;
        MTLTriangleFillMode fill;
        MTLDepthClipMode clip;
        float bias[2];
        uint32_t stencil[2];
        float blend[4];
        bool viewport_valid, scissor_valid;
        MTLViewport viewport;
        MTLScissorRect scissor;
        __unsafe_unretained id buffers[2][31];
        NSUInteger offsets[2][31];
        __unsafe_unretained id textures[2][96], samplers[2][16];
    } cache;
    uint64_t encoder_serial; /* bumped for every new render encoder */
    uint32_t vertex_id_base; /* fourth word of GLMPoint, independent of state serial */
    uint32_t vertex_id_offset, vertex_id_mapped;
    uint32_t tess_mode;      /* pipelines being built tessellate: 1 | partition << 1 | cw << 4 */
    /* Vertex ranges of recent indexed draws from buffer objects. */
    struct index_range_memo {
        void *storage;
        uint64_t generation;
        uintptr_t offset;
        GLsizei count;
        GLenum type;
        uint32_t restart, low, high;
        /* Restart rewritten into a list (restart_list): id<MTLBuffer>, retained. */
        void *list;
        uint32_t list_count, list_primitive;
    } index_ranges[256];
    bool viewport_array;     /* the draw selects viewports (gl_ViewportIndex): set all of them */
    __unsafe_unretained id<MTLFunction> vertex_override; /* this draw's vertex function variant, if any */
    __unsafe_unretained id<MTLFunction> fragment_override; /* this draw's border emulation variant, if any */
    uint32_t border_mask;    /* sampler slots whose border is emulated (fragment_override) */
    __unsafe_unretained id<MTLFunction> alpha_override; /* fragment_override when it is the alpha test variant */
    float alpha_ref;
    bool depth_clamp_active;
    float border_data[64][4]; /* GLMBorder: colours by slot, then GL_CLAMP axes */
    uint8_t topology;        /* MTLPrimitiveTopologyClass for pipelines being built (layered output), 0: unspecified */
    NSUInteger layers;       /* render target array length of the open pass (layered attachments), 0: none */
    id<MTLFunction> clear_vertex_layered;
    id<MTLFunction> image_vertex, image_fragment, bitmap_fragment;
    /* The last full draw setup, reused while nothing it depends on changed. */
    struct {
        bool valid;
        uint64_t serial, encoder_serial;
        uint32_t layout_id;
        __unsafe_unretained id vertex_override, fragment_override;
        struct draw_stages st;
        struct glm_ff_key key;
    } fast;
    /* One-entry memos in front of the pipeline/depth/function caches. */
    uint8_t last_pipeline_key[256];
    id<MTLRenderPipelineState> last_pipeline;
    uint8_t last_depth_key[64];
    id<MTLDepthStencilState> last_depth;
    struct glm_ff_key last_ff_key;
    id<MTLFunction> last_ff_vertex, last_ff_fragment;
    /* Uniform blocks already in this command buffer's transient memory. */
    uint64_t upload_serial;
    float ff_uploaded[sizeof(struct glm_ff_uniforms) / sizeof(float)];
    size_t ff_size;
    NSUInteger ff_offset;
    id<MTLBuffer> ff_buffer;
    bool ff_valid;
    struct glm_legacy_block legacy_uploaded, legacy_next;
    NSUInteger legacy_offset;
    id<MTLBuffer> legacy_buffer;
    bool legacy_valid;
};

/* ---- encoder state ------------------------------------------------------- */

enum { STAGE_VERTEX = 1, STAGE_FRAGMENT = 2, STAGE_BOTH = 3 };

static void enc_buffer(struct glm_backend_context *b, unsigned stages, __unsafe_unretained id<MTLBuffer> buffer, NSUInteger offset,
                       NSUInteger slot)
{
    for (int stage = 0; stage < 2; ++stage) {
        if (!(stages & (1u << stage))) continue;
        struct encoder_cache *c = &b->cache;
        if (c->buffers[stage][slot] == buffer) {
            if (c->offsets[stage][slot] == offset) continue;
            if (stage == 0) [b->encoder setVertexBufferOffset:offset atIndex:slot];
            else [b->encoder setFragmentBufferOffset:offset atIndex:slot];
        } else if (stage == 0) {
            [b->encoder setVertexBuffer:buffer offset:offset atIndex:slot];
        } else {
            [b->encoder setFragmentBuffer:buffer offset:offset atIndex:slot];
        }
        c->buffers[stage][slot] = buffer;
        c->offsets[stage][slot] = offset;
    }
}

static void enc_bytes(struct glm_backend_context *b, unsigned stages, const void *bytes, NSUInteger length,
                      NSUInteger slot)
{
    if (stages & STAGE_VERTEX) {
        [b->encoder setVertexBytes:bytes length:length atIndex:slot];
        b->cache.buffers[0][slot] = nil;
    }
    if (stages & STAGE_FRAGMENT) {
        [b->encoder setFragmentBytes:bytes length:length atIndex:slot];
        b->cache.buffers[1][slot] = nil;
    }
}

GLM_HIDDEN void glm_encoder_texture(struct glm_context *ctx, unsigned stages, void *texture_pointer, void *sampler_pointer,
                                    NSUInteger slot)
{
    struct glm_backend_context *b = ctx->backend;
    id<MTLTexture> texture = (__bridge id<MTLTexture>)texture_pointer;
    id<MTLSamplerState> sampler = (__bridge id<MTLSamplerState>)sampler_pointer;
    for (int stage = 0; stage < 2; ++stage) {
        if (!(stages & (1u << stage))) continue;
        struct encoder_cache *c = &b->cache;
        if (slot < 96 && c->textures[stage][slot] != texture) {
            if (stage == 0) [b->encoder setVertexTexture:texture atIndex:slot];
            else [b->encoder setFragmentTexture:texture atIndex:slot];
            c->textures[stage][slot] = texture;
        }
        if (slot < 16 && c->samplers[stage][slot] != sampler) {
            if (stage == 0) [b->encoder setVertexSamplerState:sampler atIndex:slot];
            else [b->encoder setFragmentSamplerState:sampler atIndex:slot];
            c->samplers[stage][slot] = sampler;
        }
    }
}

static void enc_pipeline(struct glm_backend_context *b, id<MTLRenderPipelineState> pipeline)
{
    if (b->cache.pipeline == pipeline) return;
    [b->encoder setRenderPipelineState:pipeline];
    b->cache.pipeline = pipeline;
}

static void enc_depth(struct glm_backend_context *b, id<MTLDepthStencilState> state)
{
    if (b->cache.depth_state == state) return;
    [b->encoder setDepthStencilState:state];
    b->cache.depth_state = state;
}

static void enc_raster(struct glm_backend_context *b, MTLWinding winding, MTLCullMode cull, MTLTriangleFillMode fill,
                       float units, float factor, MTLDepthClipMode clip, uint32_t front_ref, uint32_t back_ref,
                       const float blend[4])
{
    struct encoder_cache *c = &b->cache;
    id<MTLRenderCommandEncoder> e = b->encoder;
    bool all = !c->raster_valid;
    if (all || c->winding != winding) [e setFrontFacingWinding:winding];
    if (all || c->cull != cull) [e setCullMode:cull];
    if (all || c->fill != fill) [e setTriangleFillMode:fill];
    if (all || c->bias[0] != units || c->bias[1] != factor) [e setDepthBias:units slopeScale:factor clamp:0];
    if (all || c->clip != clip) [e setDepthClipMode:clip];
    if (all || c->stencil[0] != front_ref || c->stencil[1] != back_ref)
        [e setStencilFrontReferenceValue:front_ref backReferenceValue:back_ref];
    if (all || memcmp(c->blend, blend, sizeof c->blend))
        [e setBlendColorRed:blend[0] green:blend[1] blue:blend[2] alpha:blend[3]];
    c->raster_valid = true;
    c->winding = winding;
    c->cull = cull;
    c->fill = fill;
    c->bias[0] = units;
    c->bias[1] = factor;
    c->clip = clip;
    c->stencil[0] = front_ref;
    c->stencil[1] = back_ref;
    memcpy(c->blend, blend, sizeof c->blend);
}

struct glm_backend_context *glm_backend_context_create(struct glm_context *ctx)
{
    struct glm_backend_context *b = calloc(1, sizeof *b);
    b->ctx = ctx;
    b->queue = (__bridge id<MTLCommandQueue>)ctx->share->queue;
    b->completions = dispatch_group_create();
    b->transients = [NSMutableArray array];
    b->retiring = [NSMutableArray array];
    b->clear_only = -1;
    b->pipelines = [NSMutableDictionary dictionary];
    b->ff_vertex = [NSMutableDictionary dictionary];
    b->ff_fragment = [NSMutableDictionary dictionary];
    b->depth_states = [NSMutableDictionary dictionary];
    b->samplers = [NSMutableDictionary dictionary];
    b->framebuffer_dirty = true;
    return b;
}

static id<MTLCommandBuffer> command_buffer(struct glm_backend_context *b);

/* Copies the depth or stencil plane of a w x h region between depth/stencil
   textures through a buffer (blits between formats need one). */
static void copy_plane(struct glm_backend_context *b, id<MTLTexture> src, NSUInteger sslice, NSUInteger slevel,
                       id<MTLTexture> dst, NSUInteger dslice, NSUInteger dlevel, NSUInteger w, NSUInteger h, bool stencil)
{
    NSUInteger row = (w * (stencil ? 1 : 4) + 255) & ~(NSUInteger)255, length = row * h;
    id<MTLBuffer> staging = [src.device newBufferWithLength:length options:MTLResourceStorageModePrivate];
    MTLBlitOption plane = stencil ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil;
    id<MTLBlitCommandEncoder> blit = [command_buffer(b) blitCommandEncoder];
    [blit copyFromTexture:src sourceSlice:sslice sourceLevel:slevel sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:staging destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:length
                  options:src.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? plane : MTLBlitOptionNone];
    [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:length sourceSize:MTLSizeMake(w, h, 1)
               toTexture:dst destinationSlice:dslice destinationLevel:dlevel destinationOrigin:MTLOriginMake(0, 0, 0)
                 options:dst.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? plane : MTLBlitOptionNone];
    [blit endEncoding];
}

static void end_encoder(struct glm_backend_context *b)
{
    if (b->encoder) {
        [b->encoder endEncoding];
        b->encoder = nil;
    }
    if (b->mixed.scratch) {
        id<MTLTexture> scratch = b->mixed.scratch;
        b->mixed.scratch = nil;
        copy_plane(b, scratch, 0, 0, b->mixed.depth, b->mixed.dslice, b->mixed.dlevel, b->mixed.width, b->mixed.height, false);
        copy_plane(b, scratch, 0, 0, b->mixed.stencil, b->mixed.sslice, b->mixed.slevel, b->mixed.width, b->mixed.height, true);
        b->mixed.depth = b->mixed.stencil = nil;
    }
}

void glm_backend_context_destroy(struct glm_backend_context *b)
{
    if (!b) return;
    /* Flush even an empty tail so previous submissions are also drained.
       waitUntilCompleted alone does not wait for completion-handler bodies. */
    glm_backend_flush(b->ctx, true);
    dispatch_group_wait(b->completions, DISPATCH_TIME_FOREVER);
    pthread_mutex_lock(&b->ctx->share->lock);
    for (GLuint i = 0; i < b->ctx->share->buffers.capacity; ++i) {
        struct glm_buffer *buffer = b->ctx->share->buffers.items[i];
        if (!buffer) continue;
        if (buffer->gpu_reader == b) {
            buffer->gpu_reader = NULL;
            int kept = 0;
            for (int j = 0; j < buffer->gpu_read_count; ++j)
                if (buffer->gpu_reads[j].serial == UINT64_MAX)
                    buffer->gpu_reads[kept++] = buffer->gpu_reads[j];
            buffer->gpu_read_count = kept;
        }
        if (buffer->gpu_writer == b) {
            buffer->gpu_writer = NULL;
            buffer->gpu_write_serial = 0;
        }
    }
    pthread_mutex_unlock(&b->ctx->share->lock);
    b->commands = nil;
    b->completions = nil;
    b->transient = nil;
    b->transients = nil;
    b->pipelines = nil;
    b->ff_vertex = b->ff_fragment = nil;
    b->depth_states = nil;
    b->samplers = nil;
    b->mixed_cache = nil;
    b->mixed.scratch = b->mixed.depth = b->mixed.stencil = nil;
    b->queue = nil;
    b->retiring = nil;
    for (int i = 0; i < 8; ++i) b->linear_views[i] = nil;
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) b->color[i] = nil;
    b->depth = b->stencil = nil;
    b->default_color = b->default_depth = nil;
    b->utility_library = nil;
    b->clear_vertex = b->clear_fragment = b->clear_vertex_layered = nil;
    for (int kind = 0; kind < 3; ++kind)
        for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) b->clear_typed[kind][i] = nil;
    b->present_vertex = b->present_fragment = nil;
    b->present_pipeline = nil;
    b->image_vertex = b->image_fragment = b->bitmap_fragment = nil;
    b->visibility = nil;
    b->last_pipeline = nil;
    b->last_depth = nil;
    b->last_ff_vertex = b->last_ff_fragment = nil;
    b->ff_buffer = b->legacy_buffer = nil;
    for (int i = 0; i < 256; ++i)
        if (b->index_ranges[i].list) CFRelease(b->index_ranges[i].list);
    free(b);
}

static void *(*shared_allocate)(size_t);
static void (*shared_deallocate)(void *, size_t);

GLM_EXPORT bool glmetal_set_shared_buffer_allocator(void *(*allocate)(size_t),
                                                   void (*deallocate)(void *, size_t))
{
    if (device || (!!allocate != !!deallocate)) return false;
    shared_allocate = allocate;
    shared_deallocate = deallocate;
    return true;
}

/* Shared storage uses Metal's allocator unless the host supplied one. */
id<MTLBuffer> glm_new_shared_buffer(NSUInteger length, const void *bytes)
{
    size_t page = (size_t)getpagesize();
    size_t size = ((size_t)(length ? length : 1) + page - 1) & ~(page - 1);
    if (!shared_allocate)
        return bytes ? [device newBufferWithBytes:bytes length:MAX(length, 1u) options:MTLResourceStorageModeShared]
                     : [device newBufferWithLength:MAX(length, 1u) options:MTLResourceStorageModeShared];
    void *memory = shared_allocate(size);
    if (!memory) return nil;
    void (*release)(void *, size_t) = shared_deallocate;
    if (bytes) memcpy(memory, bytes, length);
    id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:memory length:size options:MTLResourceStorageModeShared
                                                deallocator:^(void *pointer, NSUInteger bytes_mapped) {
                                                    release(pointer, bytes_mapped);
                                                }];
    if (!buffer) release(memory, size);
    return buffer;
}

/* Completed transient blocks, reused instead of reallocating each frame. */
void glm_backend_buffer_retire(struct glm_context *ctx, struct glm_buffer *buffer);

/* Buffer object storage recycled by power-of-two size class: games orphan
   dynamic buffers (glBufferData) every frame, and a new MTLBuffer each
   time costs more than the data copy. Storage is reused once the command
   buffer after its last use completed. */
enum { POOL_MIN_SHIFT = 12, POOL_CLASSES = 13, POOL_PER_CLASS = 1024 };
static NSMutableArray<id<MTLBuffer>> *storage_pool[POOL_CLASSES];
static pthread_mutex_t storage_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static int storage_class(NSUInteger length)
{
    int shift = POOL_MIN_SHIFT;
    while (((NSUInteger)1 << shift) < length) ++shift;
    return shift - POOL_MIN_SHIFT < POOL_CLASSES ? shift - POOL_MIN_SHIFT : -1;
}

static id<MTLBuffer> pooled_storage(NSUInteger length)
{
    static int no_pool = -1;
    if (no_pool < 0) no_pool = getenv("GLMETAL_NO_BUFFER_POOL") != NULL;
    if (no_pool) return glm_new_shared_buffer(length, NULL);
    int c = storage_class(length);
    if (c < 0) return glm_new_shared_buffer(length, NULL);
    id<MTLBuffer> buffer = nil;
    pthread_mutex_lock(&storage_pool_lock);
    buffer = storage_pool[c].lastObject;
    if (buffer) [storage_pool[c] removeLastObject];
    pthread_mutex_unlock(&storage_pool_lock);
    return buffer ? buffer : glm_new_shared_buffer((NSUInteger)1 << (c + POOL_MIN_SHIFT), NULL);
}

static void storage_return(id<MTLBuffer> buffer)
{
    int c = storage_class(buffer.length);
    if (c < 0 || buffer.length != (NSUInteger)1 << (c + POOL_MIN_SHIFT)) return;
    pthread_mutex_lock(&storage_pool_lock);
    if (!storage_pool[c]) storage_pool[c] = [NSMutableArray array];
    /* Small per-draw uploads can consume hundreds of buffers before a
       command buffer completes. Keep up to 8 MB of these per class instead
       of dropping all but 32 at each completion. Preserve the existing
       large-buffer limit of 32 buffers / 64 MB, with at least two slots. */
    NSUInteger small_limit = MIN((NSUInteger)POOL_PER_CLASS,
                                 MAX((NSUInteger)32, ((NSUInteger)8 << 20) >> (c + POOL_MIN_SHIFT)));
    NSUInteger limit = MIN(small_limit, MAX((NSUInteger)2, ((NSUInteger)64 << 20) >> (c + POOL_MIN_SHIFT)));
    if (storage_pool[c].count < limit) [storage_pool[c] addObject:buffer];
    pthread_mutex_unlock(&storage_pool_lock);
}

static NSMutableArray<id<MTLBuffer>> *transient_pool;
static pthread_mutex_t transient_pool_lock = PTHREAD_MUTEX_INITIALIZER;
enum { TRANSIENT_BLOCK = 4u << 20 };

static id<MTLCommandBuffer> command_buffer(struct glm_backend_context *b)
{
    if (!b->commands) b->commands = [b->queue commandBuffer];
    return b->commands;
}

void glm_backend_flush(struct glm_context *ctx, bool wait)
{
    struct glm_backend_context *b = ctx->backend;
    end_encoder(b);
    id<MTLCommandBuffer> commands = b->commands;
    if (commands) {
        NSArray *held = [b->transients copy];
        NSArray *retired = [b->retiring copy];
        [b->retiring removeAllObjects];
        uint64_t serial = ++b->submitted_serial;
        struct glm_backend_context *owner = b;
        dispatch_group_t completions = b->completions;
        dispatch_group_enter(completions);
        [commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
            static int gpu_time = -1;
            if (gpu_time < 0) gpu_time = getenv("GLMETAL_GPU_TIME") != NULL;
            if (gpu_time) {
                static double total;
                static unsigned count;
                total += done.GPUEndTime - done.GPUStartTime;
                if (++count % 60 == 0) {
                    fprintf(stderr, "glmetal: GPU %.3f ms per command buffer\n", total / 60 * 1e3);
                    total = 0;
                }
            }
            /* Transient memory lives until the GPU is done, then returns to
               the pool (bounded, so a burst does not pin memory). */
            pthread_mutex_lock(&transient_pool_lock);
            if (!transient_pool) transient_pool = [NSMutableArray array];
            for (id<MTLBuffer> block in held)
                if (block.length == TRANSIENT_BLOCK && transient_pool.count < 16) [transient_pool addObject:block];
            pthread_mutex_unlock(&transient_pool_lock);
            for (id<MTLBuffer> storage in retired) storage_return(storage);
            uint64_t current = __atomic_load_n(&owner->completed_serial, __ATOMIC_ACQUIRE);
            while (current < serial &&
                   !__atomic_compare_exchange_n(&owner->completed_serial, &current, serial, false, __ATOMIC_ACQ_REL,
                                                __ATOMIC_ACQUIRE)) {
            }
            dispatch_group_leave(completions);
        }];
        [commands commit];
        b->visibility = nil;
        b->ff_valid = b->legacy_valid = false;
        [b->transients removeAllObjects];
        b->transient = nil;
        b->commands = nil;
        b->framebuffer_dirty = true;
    }
    if (wait && commands) [commands waitUntilCompleted];
}

/* Bump allocator over 4 MB chunks, retained until the command buffer that
   used them completes. */
/* Raw pointer returns: an object return costs ARC's retain/autorelease
   handshake even when unretained (the macros below bridge back). */
static void *transient_alloc_raw(struct glm_backend_context *b, NSUInteger length, NSUInteger *offset)
{
    length = (length + 255) & ~(NSUInteger)255;
    if (!b->transient || b->transient_used + length > b->transient.length) {
        NSUInteger size = length > TRANSIENT_BLOCK ? length : TRANSIENT_BLOCK;
        id<MTLBuffer> reused = nil;
        if (size == TRANSIENT_BLOCK) {
            pthread_mutex_lock(&transient_pool_lock);
            reused = transient_pool.lastObject;
            if (reused) [transient_pool removeLastObject];
            pthread_mutex_unlock(&transient_pool_lock);
        }
        b->transient = reused ? reused : glm_new_shared_buffer(size, NULL);
        b->transient_used = 0;
        [b->transients addObject:b->transient];
    }
    *offset = b->transient_used;
    b->transient_used += length;
    return (__bridge void *)b->transient;
}

/* ---- textures and renderbuffers ------------------------------------------ */

void glm_backend_renderbuffer_storage(struct glm_context *ctx, struct glm_renderbuffer *rb)
{
    (void)ctx;
    glm_backend_release(rb->backend);
    rb->backend = NULL;
    if (!rb->width || !rb->height) return;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)rb->metal_format
                                                                                 width:(NSUInteger)rb->width
                                                                                height:(NSUInteger)rb->height
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if (rb->samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = (NSUInteger)rb->samples;
        d.storageMode = MTLStorageModePrivate;
    } else {
        d.storageMode = MTLStorageModePrivate;
    }
    rb->backend = (__bridge_retained void *)[device newTextureWithDescriptor:d];
}

GLM_HIDDEN id<MTLTexture> glm_texture_view_for_attachment(struct glm_context *ctx, const struct glm_attachment *a,
                                                          NSUInteger *slice, NSUInteger *level);

static id<MTLTexture> attachment_texture(struct glm_context *ctx, const struct glm_attachment *a, NSUInteger *slice,
                                         NSUInteger *level)
{
    *slice = *level = 0;
    if (a->type == GL_RENDERBUFFER) {
        struct glm_renderbuffer *rb = glm_renderbuffer_get(ctx, a->name);
        return rb ? (__bridge id<MTLTexture>)rb->backend : nil;
    }
    if (a->type == GL_TEXTURE) {
        id<MTLTexture> t = glm_texture_view_for_attachment(ctx, a, slice, level);
        if (a->layered) *slice = 0;
        return t;
    }
    return nil;
}

/* Layers a layered attachment spans (render target array length). */
static NSUInteger attachment_layers(const struct glm_attachment *a, id<MTLTexture> t)
{
    if (!a->layered || !t) return 0;
    switch (t.textureType) {
    case MTLTextureTypeCube: return 6;
    case MTLTextureTypeCubeArray: return 6 * t.arrayLength;
    case MTLTextureType3D: return t.depth;
    default: return t.arrayLength;
    }
}

GLenum glm_framebuffer_status(struct glm_context *ctx, struct glm_framebuffer *fb)
{
    bool any = false;
    NSUInteger width = 0, height = 0;
    const struct glm_attachment *all[GLM_MAX_DRAW_BUFFERS + 2];
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) all[i] = &fb->color[i];
    all[GLM_MAX_DRAW_BUFFERS] = &fb->depth;
    all[GLM_MAX_DRAW_BUFFERS + 1] = &fb->stencil;
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS + 2; ++i) {
        if (all[i]->type == GL_NONE) continue;
        NSUInteger slice, level;
        id<MTLTexture> t = attachment_texture(ctx, all[i], &slice, &level);
        if (!t) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        NSUInteger w = MAX(t.width >> level, 1u), h = MAX(t.height >> level, 1u);
        if (any && (w != width || h != height)) {
            /* GL 3 allows mixed sizes (the intersection renders); keep the
               smallest like Apple's implementation. */
            width = MIN(width, w);
            height = MIN(height, h);
        } else if (!any) {
            width = w;
            height = h;
        }
        any = true;
    }
    return any ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
}

void glm_backend_framebuffer_changed(struct glm_context *ctx) { ctx->backend->framebuffer_dirty = true; }

static bool ensure_encoder(struct glm_context *ctx);

bool glm_backend_supports_samples(unsigned samples) { return [device supportsTextureSampleCount:samples]; }

unsigned glm_backend_draw_samples(struct glm_context *ctx)
{
    return ensure_encoder(ctx) ? (unsigned)ctx->backend->samples : 1;
}

static unsigned long glm_trace_frames;

static MTLPixelFormat linear_format(MTLPixelFormat f)
{
    switch (f) {
    case MTLPixelFormatRGBA8Unorm_sRGB: return MTLPixelFormatRGBA8Unorm;
    case MTLPixelFormatBGRA8Unorm_sRGB: return MTLPixelFormatBGRA8Unorm;
    default: return MTLPixelFormatInvalid;
    }
}

/* `t` with an sRGB format viewed as its linear twin (a cached view). */
static id<MTLTexture> linear_view(struct glm_backend_context *b, id<MTLTexture> t)
{
    MTLPixelFormat linear = linear_format(t.pixelFormat);
    if (!linear) return t;
    enum { N = sizeof b->linear_views / sizeof b->linear_views[0] };
    for (int i = 0; i < N; ++i)
        if (b->linear_views[i].parentTexture == t) return b->linear_views[i];
    id<MTLTexture> view = [t newTextureViewWithPixelFormat:linear];
    if (!view) return t;
    b->linear_views[b->linear_view_next] = view;
    b->linear_view_next = (b->linear_view_next + 1) % N;
    return view;
}

/* With GL_FRAMEBUFFER_SRGB disabled (the default) GL writes sRGB colour
   attachments without encoding; Metal always encodes into an sRGB format,
   so those render through a linear view of the texture. */
static id<MTLTexture> render_view(struct glm_context *ctx, id<MTLTexture> t)
{
    return ctx->state.framebuffer_srgb ? t : linear_view(ctx->backend, t);
}

/* Opens a render pass on the current draw framebuffer if needed. Returns
   false when there is nothing to draw into. */
static bool ensure_encoder(struct glm_context *ctx)
{
    struct glm_backend_context *b = ctx->backend;
    if (b->encoder && !b->framebuffer_dirty && b->pass_srgb == ctx->state.framebuffer_srgb) return true;
    end_encoder(b);
    b->framebuffer_dirty = false;
    b->pass_srgb = ctx->state.framebuffer_srgb;
    struct glm_framebuffer *fb = glm_framebuffer_get(ctx, ctx->draw_framebuffer);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    b->width = b->height = 0;
    b->samples = 1;
    b->layers = 0;
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) b->color[i] = nil;
    b->depth = b->stencil = nil;
    if (ctx->draw_framebuffer == 0) {
        if (!ctx->presents) glm_error(ctx, GL_INVALID_FRAMEBUFFER_OPERATION);
        if (!b->default_color) return false;
        b->color[0] = b->default_color;
        b->depth = b->stencil = b->default_depth;
    } else {
        /* Incomplete framebuffers are not drawn to (and some, such as
           compressed attachments, Metal cannot render to at all). Checked
           again on the next draw. */
        if (!fb || glm_framebuffer_check(ctx, fb) != GL_FRAMEBUFFER_COMPLETE) {
            glm_error(ctx, GL_INVALID_FRAMEBUFFER_OPERATION);
            b->framebuffer_dirty = true;
            return false;
        }
        for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) {
            GLenum buffer = fb->draw_buffers[i];
            if (buffer < GL_COLOR_ATTACHMENT0 || buffer >= GL_COLOR_ATTACHMENT0 + GLM_MAX_DRAW_BUFFERS) continue;
            const struct glm_attachment *a = &fb->color[buffer - GL_COLOR_ATTACHMENT0];
            NSUInteger slice, level;
            id<MTLTexture> t = attachment_texture(ctx, a, &slice, &level);
            if (!t) continue;
            b->color[i] = render_view(ctx, t);
            pass.colorAttachments[i].slice = slice;
            pass.colorAttachments[i].level = level;
            NSUInteger layers = attachment_layers(a, t);
            if (layers) pass.renderTargetArrayLength = layers;
        }
        NSUInteger slice, level;
        id<MTLTexture> d = attachment_texture(ctx, &fb->depth, &slice, &level);
        if (attachment_layers(&fb->depth, d)) pass.renderTargetArrayLength = attachment_layers(&fb->depth, d);
        pass.depthAttachment.slice = slice;
        pass.depthAttachment.level = level;
        id<MTLTexture> s = attachment_texture(ctx, &fb->stencil, &slice, &level);
        pass.stencilAttachment.slice = slice;
        pass.stencilAttachment.level = level;
        b->depth = d;
        b->stencil = s;
        MTLPixelFormat df = d.pixelFormat, sf = s.pixelFormat, combined = MTLPixelFormatDepth32Float_Stencil8;
        if (d && s && d != s && (df == combined || sf == combined)) {
            NSUInteger dlevel = pass.depthAttachment.level, slevel = pass.stencilAttachment.level;
            NSUInteger w = MIN(MAX(d.width >> dlevel, 1u), MAX(s.width >> slevel, 1u));
            NSUInteger h = MIN(MAX(d.height >> dlevel, 1u), MAX(s.height >> slevel, 1u));
            bool copyable = (df == combined || df == MTLPixelFormatDepth32Float) && (sf == combined || sf == MTLPixelFormatStencil8) &&
                            d.sampleCount == 1 && s.sampleCount == 1 && !pass.renderTargetArrayLength;
            if (!copyable) {
                b->stencil = nil;
            } else {
                id<MTLTexture> scratch = b->mixed_cache;
                if (!scratch || scratch.width != w || scratch.height != h) {
                    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:combined width:w height:h mipmapped:NO];
                    td.usage = MTLTextureUsageRenderTarget;
                    td.storageMode = MTLStorageModePrivate;
                    scratch = b->mixed_cache = [d.device newTextureWithDescriptor:td];
                }
                copy_plane(b, d, pass.depthAttachment.slice, dlevel, scratch, 0, 0, w, h, false);
                copy_plane(b, s, pass.stencilAttachment.slice, slevel, scratch, 0, 0, w, h, true);
                b->mixed.scratch = scratch;
                b->mixed.depth = d;
                b->mixed.stencil = s;
                b->mixed.dslice = pass.depthAttachment.slice;
                b->mixed.dlevel = dlevel;
                b->mixed.sslice = pass.stencilAttachment.slice;
                b->mixed.slevel = slevel;
                b->mixed.width = w;
                b->mixed.height = h;
                b->depth = b->stencil = scratch;
                pass.depthAttachment.slice = pass.depthAttachment.level = 0;
                pass.stencilAttachment.slice = pass.stencilAttachment.level = 0;
            }
        }
    }
    b->layers = pass.renderTargetArrayLength;
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) {
        if (!b->color[i]) continue;
        pass.colorAttachments[i].texture = b->color[i];
        pass.colorAttachments[i].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[i].storeAction = MTLStoreActionStore;
        NSUInteger level = pass.colorAttachments[i].level;
        NSUInteger w = MAX(b->color[i].width >> level, 1u), h = MAX(b->color[i].height >> level, 1u);
        b->width = b->width ? MIN(b->width, w) : w;
        b->height = b->height ? MIN(b->height, h) : h;
        b->samples = b->color[i].sampleCount;
    }
    if (b->depth) {
        pass.depthAttachment.texture = b->depth;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        if (!b->width) { b->width = b->depth.width; b->height = b->depth.height; }
        b->samples = b->depth.sampleCount;
    }
    if (b->stencil) {
        MTLPixelFormat f = b->stencil.pixelFormat;
        if (f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatStencil8 || f == MTLPixelFormatDepth24Unorm_Stencil8) {
            pass.stencilAttachment.texture = b->stencil;
            pass.stencilAttachment.loadAction = MTLLoadActionLoad;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
            /* Stencil-only framebuffers take their size from it. */
            if (!b->width) {
                NSUInteger level = pass.stencilAttachment.level;
                b->width = MAX(b->stencil.width >> level, 1u);
                b->height = MAX(b->stencil.height >> level, 1u);
                b->samples = b->stencil.sampleCount;
            }
        } else {
            b->stencil = nil;
        }
    }
    if (b->depth) {
        MTLPixelFormat f = b->depth.pixelFormat;
        if (f == MTLPixelFormatStencil8) {
            pass.depthAttachment.texture = nil;
            b->depth = nil;
        }
    }
    if (!b->width || !b->height) return false;
    pass.renderTargetWidth = b->width;
    pass.renderTargetHeight = b->height;
    b->pass_has_visibility = false;
    b->counting_offset = -1;
    ++b->pass_id;
    if (ctx->active_queries[GLM_QUERY_OCCLUSION]) {
        enum { VISIBILITY_BYTES = 64 * 1024 };
        if (!b->visibility || b->visibility_used + 8 > b->visibility.length) {
            b->visibility = glm_new_shared_buffer(VISIBILITY_BYTES, NULL);
            memset(mtl_contents(b->visibility), 0, VISIBILITY_BYTES);
            b->visibility_used = 0;
        }
        pass.visibilityResultBuffer = b->visibility;
        b->pass_has_visibility = true;
    }
    b->encoder = [command_buffer(b) renderCommandEncoderWithDescriptor:pass];
    memset(&b->cache, 0, sizeof b->cache);
    ++b->encoder_serial;
    return b->encoder != nil;
}

/* ---- pipeline state ------------------------------------------------------ */

static MTLBlendFactor blend_factor(GLenum f)
{
    switch (f) {
    case GL_ZERO: return MTLBlendFactorZero;
    case GL_ONE: return MTLBlendFactorOne;
    case GL_SRC_COLOR: return MTLBlendFactorSourceColor;
    case GL_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case GL_DST_COLOR: return MTLBlendFactorDestinationColor;
    case GL_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case GL_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case GL_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case GL_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case GL_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case GL_CONSTANT_COLOR: return MTLBlendFactorBlendColor;
    case GL_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendColor;
    case GL_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
    case GL_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
    case GL_SRC1_COLOR: return MTLBlendFactorSource1Color;
    case GL_ONE_MINUS_SRC1_COLOR: return MTLBlendFactorOneMinusSource1Color;
    case GL_SRC1_ALPHA: return MTLBlendFactorSource1Alpha;
    case GL_ONE_MINUS_SRC1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
    default: return MTLBlendFactorOne;
    }
}

static MTLBlendOperation blend_operation(GLenum e)
{
    switch (e) {
    case GL_FUNC_SUBTRACT: return MTLBlendOperationSubtract;
    case GL_FUNC_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case GL_MIN: return MTLBlendOperationMin;
    case GL_MAX: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

static MTLCompareFunction compare_function(GLenum f)
{
    switch (f) {
    case GL_NEVER: return MTLCompareFunctionNever;
    case GL_LESS: return MTLCompareFunctionLess;
    case GL_EQUAL: return MTLCompareFunctionEqual;
    case GL_LEQUAL: return MTLCompareFunctionLessEqual;
    case GL_GREATER: return MTLCompareFunctionGreater;
    case GL_NOTEQUAL: return MTLCompareFunctionNotEqual;
    case GL_GEQUAL: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

static MTLStencilOperation stencil_operation(GLenum op)
{
    switch (op) {
    case GL_ZERO: return MTLStencilOperationZero;
    case GL_REPLACE: return MTLStencilOperationReplace;
    case GL_INCR: return MTLStencilOperationIncrementClamp;
    case GL_DECR: return MTLStencilOperationDecrementClamp;
    case GL_INVERT: return MTLStencilOperationInvert;
    case GL_INCR_WRAP: return MTLStencilOperationIncrementWrap;
    case GL_DECR_WRAP: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

struct pipeline_key {
    uint64_t vertex_function, fragment_function; /* object addresses */
    uint32_t color_formats[GLM_MAX_DRAW_BUFFERS];
    uint32_t depth_format, stencil_format, samples;
    struct {
        uint8_t enabled, write_mask;
        uint16_t src_rgb, dst_rgb, src_alpha, dst_alpha, eq_rgb, eq_alpha;
    } blend[GLM_MAX_DRAW_BUFFERS];
    uint8_t alpha_to_coverage, rasterization_disabled;
    uint32_t tess_mode;
    uint32_t topology;
    uint32_t vertex_layout; /* interned GPU-fetch layout id, 0: the function's own, ~0: clears */
};

/* Vertex layout 0: GLM_MAX_ATTRIBS float4 attributes interleaved. */
static MTLVertexDescriptor *stream_layout(void)
{
    static MTLVertexDescriptor *layout;
    if (!layout) {
        layout = [MTLVertexDescriptor vertexDescriptor];
        for (int i = 0; i < GLM_MAX_ATTRIBS; ++i) {
            layout.attributes[i].format = MTLVertexFormatFloat4;
            layout.attributes[i].offset = (NSUInteger)i * 16;
            layout.attributes[i].bufferIndex = GLM_SLOT_STREAM;
        }
        layout.layouts[GLM_SLOT_STREAM].stride = GLM_MAX_ATTRIBS * 16;
        layout.layouts[GLM_SLOT_STREAM].stepFunction = MTLVertexStepFunctionPerVertex;
    }
    return layout;
}

static id<MTLRenderPipelineState> pipeline_for(struct glm_backend_context *b, id<MTLFunction> vertex,
                                               id<MTLFunction> fragment, const struct glm_state *s, bool for_clear,
                                               GLbitfield clear_mask, MTLVertexDescriptor *layout, uint32_t layout_id,
                                               bool no_raster)
{
    struct pipeline_key key;
    memset(&key, 0, sizeof key);
    key.vertex_function = (uint64_t)(uintptr_t)(__bridge void *)vertex;
    key.fragment_function = (uint64_t)(uintptr_t)(__bridge void *)fragment;
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) {
        if (!b->color[i]) continue;
        key.color_formats[i] = (uint32_t)b->color[i].pixelFormat;
        bool write = for_clear ? (clear_mask & GL_COLOR_BUFFER_BIT) != 0 && (b->clear_only < 0 || b->clear_only == i) : true;
        uint8_t mask = 0;
        if (write) {
            if (s->color_mask[i][0]) mask |= MTLColorWriteMaskRed;
            if (s->color_mask[i][1]) mask |= MTLColorWriteMaskGreen;
            if (s->color_mask[i][2]) mask |= MTLColorWriteMaskBlue;
            if (s->color_mask[i][3]) mask |= MTLColorWriteMaskAlpha;
        }
        key.blend[i].write_mask = mask;
        /* A logic op replaces blending (GL 2.1 4.1.10). */
        bool logic_op = s->color_logic_op && s->logic_op != GL_COPY;
        if (!for_clear && s->blend[i].enabled && !logic_op) {
            key.blend[i].enabled = 1;
            key.blend[i].src_rgb = (uint16_t)s->blend[i].src_rgb;
            key.blend[i].dst_rgb = (uint16_t)s->blend[i].dst_rgb;
            key.blend[i].src_alpha = (uint16_t)s->blend[i].src_alpha;
            key.blend[i].dst_alpha = (uint16_t)s->blend[i].dst_alpha;
            key.blend[i].eq_rgb = (uint16_t)s->blend[i].eq_rgb;
            key.blend[i].eq_alpha = (uint16_t)s->blend[i].eq_alpha;
        }
    }
    key.depth_format = b->depth ? (uint32_t)b->depth.pixelFormat : 0;
    key.stencil_format = b->stencil ? (uint32_t)b->stencil.pixelFormat : 0;
    key.samples = (uint32_t)b->samples;
    key.alpha_to_coverage = !for_clear && s->sample_alpha_to_coverage && b->samples > 1;
    key.rasterization_disabled = no_raster;
    key.tess_mode = for_clear ? 0 : b->tess_mode;
    key.topology = b->topology;
    key.vertex_layout = for_clear ? UINT32_MAX : layout_id;
    _Static_assert(sizeof key <= sizeof b->last_pipeline_key, "pipeline memo");
    if (b->last_pipeline && !memcmp(&key, b->last_pipeline_key, sizeof key)) return b->last_pipeline;
    NSData *data = [NSData dataWithBytes:&key length:sizeof key];
    id<MTLRenderPipelineState> state = b->pipelines[data];
    if (state) {
        memcpy(b->last_pipeline_key, &key, sizeof key);
        b->last_pipeline = state;
        return state;
    }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vertex;
    d.fragmentFunction = fragment;
    if (!for_clear) d.vertexDescriptor = layout ? layout : stream_layout();
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) {
        if (!key.color_formats[i]) continue;
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        c.pixelFormat = (MTLPixelFormat)key.color_formats[i];
        c.writeMask = key.blend[i].write_mask;
        if (key.blend[i].enabled) {
            c.blendingEnabled = YES;
            c.sourceRGBBlendFactor = blend_factor(key.blend[i].src_rgb);
            c.destinationRGBBlendFactor = blend_factor(key.blend[i].dst_rgb);
            c.sourceAlphaBlendFactor = blend_factor(key.blend[i].src_alpha);
            c.destinationAlphaBlendFactor = blend_factor(key.blend[i].dst_alpha);
            c.rgbBlendOperation = blend_operation(key.blend[i].eq_rgb);
            c.alphaBlendOperation = blend_operation(key.blend[i].eq_alpha);
        }
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)key.depth_format;
    d.stencilAttachmentPixelFormat = (MTLPixelFormat)key.stencil_format;
    d.rasterSampleCount = key.samples ? key.samples : 1;
    d.alphaToCoverageEnabled = key.alpha_to_coverage;
    if (key.topology) d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)key.topology;
    if (key.tess_mode) {
        d.maxTessellationFactor = 64;
        d.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
        d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
        d.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
        d.tessellationPartitionMode = (MTLTessellationPartitionMode)((key.tess_mode >> 1) & 7);
        d.tessellationOutputWindingOrder = (key.tess_mode >> 4) & 1 ? MTLWindingClockwise : MTLWindingCounterClockwise;
    }
    if (key.rasterization_disabled) {
        d.rasterizationEnabled = NO;
        d.fragmentFunction = nil;
    }
    NSError *error = nil;
    uint64_t started = glm_now_ns();
    state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    glm_note_stall("pipeline", started);
    if (!state) {
        glm_log("pipeline creation failed: %s", error.localizedDescription.UTF8String);
        return nil;
    }
    b->pipelines[data] = state;
    memcpy(b->last_pipeline_key, &key, sizeof key);
    b->last_pipeline = state;
    return state;
}

static id<MTLDepthStencilState> depth_stencil_for(struct glm_backend_context *b, const struct glm_state *s,
                                                  bool for_clear, GLbitfield clear_mask)
{
    struct {
        uint16_t depth_func;
        uint8_t depth_write, stencil;
        struct { uint16_t func, fail, zfail, zpass; uint32_t read, write; } face[2];
    } key;
    memset(&key, 0, sizeof key);
    if (for_clear) {
        key.depth_func = (clear_mask & GL_DEPTH_BUFFER_BIT) && b->depth ? GL_ALWAYS : 0;
        key.depth_write = (clear_mask & GL_DEPTH_BUFFER_BIT) && s->depth_mask && b->depth;
        if ((clear_mask & GL_STENCIL_BUFFER_BIT) && b->stencil) {
            key.stencil = 1;
            for (int f = 0; f < 2; ++f) {
                key.face[f].func = GL_ALWAYS;
                key.face[f].fail = key.face[f].zfail = key.face[f].zpass = GL_REPLACE;
                key.face[f].read = 0xff;
                key.face[f].write = s->stencil[0].write_mask & 0xff;
            }
        }
    } else {
        if (s->depth_test && b->depth) {
            key.depth_func = (uint16_t)s->depth_func;
            key.depth_write = s->depth_mask;
        }
        if (s->stencil_test && b->stencil) {
            key.stencil = 1;
            /* GL front/back are relative to GL winding; Metal's front is the
               face the backend declared front, which matches GL's front. */
            for (int f = 0; f < 2; ++f) {
                const struct glm_stencil_face *face = f ? glm_stencil_back(s) : &s->stencil[0];
                key.face[f].func = (uint16_t)face->func;
                key.face[f].fail = (uint16_t)face->fail;
                key.face[f].zfail = (uint16_t)face->zfail;
                key.face[f].zpass = (uint16_t)face->zpass;
                key.face[f].read = face->value_mask & 0xff;
                key.face[f].write = face->write_mask & 0xff;
            }
        }
    }
    _Static_assert(sizeof key <= sizeof b->last_depth_key, "depth memo");
    if (b->last_depth && !memcmp(&key, b->last_depth_key, sizeof key)) return b->last_depth;
    NSData *data = [NSData dataWithBytes:&key length:sizeof key];
    id<MTLDepthStencilState> state = b->depth_states[data];
    if (state) {
        memcpy(b->last_depth_key, &key, sizeof key);
        b->last_depth = state;
        return state;
    }
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = key.depth_func ? compare_function(key.depth_func) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = key.depth_write;
    if (key.stencil) {
        MTLStencilDescriptor *faces[2] = {[MTLStencilDescriptor new], [MTLStencilDescriptor new]};
        for (int f = 0; f < 2; ++f) {
            faces[f].stencilCompareFunction = compare_function(key.face[f].func);
            faces[f].stencilFailureOperation = stencil_operation(key.face[f].fail);
            faces[f].depthFailureOperation = stencil_operation(key.face[f].zfail);
            faces[f].depthStencilPassOperation = stencil_operation(key.face[f].zpass);
            faces[f].readMask = key.face[f].read;
            faces[f].writeMask = key.face[f].write;
        }
        d.frontFaceStencil = faces[0];
        d.backFaceStencil = faces[1];
    }
    state = [device newDepthStencilStateWithDescriptor:d];
    b->depth_states[data] = state;
    memcpy(b->last_depth_key, &key, sizeof key);
    b->last_depth = state;
    return state;
}

static void apply_viewport_scissor(struct glm_backend_context *b, const struct glm_state *s, bool full)
{
    if (b->viewport_array && !full) {
        /* Every index of ARB_viewport_array, for gl_ViewportIndex. */
        MTLViewport viewports[GLM_MAX_VIEWPORTS];
        MTLScissorRect scissors[GLM_MAX_VIEWPORTS];
        for (int i = 0; i < GLM_MAX_VIEWPORTS; ++i) {
            viewports[i] = (MTLViewport){s->viewports[i][0], s->viewports[i][1], s->viewports[i][2], s->viewports[i][3],
                                         s->depth_ranges[i][0], s->depth_ranges[i][1]};
            const GLint *r = i ? s->scissors[i] : s->scissor;
            bool test = i ? s->scissor_tests[i] : s->scissor_test;
            scissors[i] = (MTLScissorRect){0, 0, b->width, b->height};
            if (test) {
                NSInteger x0 = MAX(r[0], 0), y0 = MAX(r[1], 0);
                NSInteger x1 = MIN((NSInteger)r[0] + r[2], (NSInteger)b->width), y1 = MIN((NSInteger)r[1] + r[3], (NSInteger)b->height);
                scissors[i] = x1 <= x0 || y1 <= y0 ? (MTLScissorRect){0, 0, 0, 0}
                                                   : (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0),
                                                                      (NSUInteger)(y1 - y0)};
            }
        }
        [b->encoder setViewports:viewports count:GLM_MAX_VIEWPORTS];
        [b->encoder setScissorRects:scissors count:GLM_MAX_VIEWPORTS];
        b->cache.viewport_valid = b->cache.scissor_valid = false;
        return;
    }
    MTLViewport viewport = {(double)s->viewport[0], (double)s->viewport[1], (double)s->viewport[2],
                            (double)s->viewport[3], s->depth_range[0], s->depth_range[1]};
    if (full) viewport = (MTLViewport){0, 0, (double)b->width, (double)b->height, 0, 1};
    if (!b->cache.viewport_valid || memcmp(&b->cache.viewport, &viewport, sizeof viewport)) {
        [b->encoder setViewport:viewport];
        b->cache.viewport = viewport;
        b->cache.viewport_valid = true;
    }
    MTLScissorRect scissor = {0, 0, b->width, b->height};
    if (s->scissor_test) {
        NSInteger x0 = MAX(s->scissor[0], 0), y0 = MAX(s->scissor[1], 0);
        NSInteger x1 = MIN((NSInteger)s->scissor[0] + s->scissor[2], (NSInteger)b->width);
        NSInteger y1 = MIN((NSInteger)s->scissor[1] + s->scissor[3], (NSInteger)b->height);
        if (x1 <= x0 || y1 <= y0) {
            scissor = (MTLScissorRect){0, 0, 0, 0};
        } else {
            scissor = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
        }
    }
    if (!b->cache.scissor_valid || memcmp(&b->cache.scissor, &scissor, sizeof scissor)) {
        [b->encoder setScissorRect:scissor];
        b->cache.scissor = scissor;
        b->cache.scissor_valid = true;
    }
}

static bool scissor_empty(struct glm_backend_context *b, const struct glm_state *s)
{
    if (!s->scissor_test) return false;
    NSInteger x0 = MAX(s->scissor[0], 0), y0 = MAX(s->scissor[1], 0);
    NSInteger x1 = MIN((NSInteger)s->scissor[0] + s->scissor[2], (NSInteger)b->width);
    NSInteger y1 = MIN((NSInteger)s->scissor[1] + s->scissor[3], (NSInteger)b->height);
    return x1 <= x0 || y1 <= y0;
}

/* ---- clear --------------------------------------------------------------- */

static NSString *const utility_source =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct ClearOut { float4 position [[position]]; };\n"
     "struct ClearValues { float4 color; float depth; };\n"
     "vertex ClearOut clear_vertex(uint vid [[vertex_id]], constant ClearValues &v [[buffer(0)]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2) * 2.0 - 1.0;\n"
     "  ClearOut o; o.position = float4(p, v.depth, 1.0); return o; }\n"
     "struct ClearLayerOut { float4 position [[position]]; uint layer [[render_target_array_index]]; };\n"
     "vertex ClearLayerOut clear_vertex_layered(uint vid [[vertex_id]], uint layer [[instance_id]],\n"
     "                                          constant ClearValues &v [[buffer(0)]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2) * 2.0 - 1.0;\n"
     "  ClearLayerOut o; o.position = float4(p, v.depth, 1.0); o.layer = layer; return o; }\n"
     /* glDrawPixels / glBitmap: a window-space rectangle of an image. */
     "struct ImageArgs { float4 rect; float4 color; float2 size; float depth; float pad; };\n"
     "struct ImageOut { float4 position [[position]]; float2 uv; };\n"
     "vertex ImageOut image_vertex(uint vid [[vertex_id]], constant ImageArgs &a [[buffer(0)]]) {\n"
     "  float2 c = float2(vid & 1, vid >> 1);\n"
     "  float2 p = mix(a.rect.xy, a.rect.zw, c) / a.size * 2.0 - 1.0;\n"
     "  ImageOut o; o.position = float4(p.x, -p.y, a.depth, 1.0); o.uv = c; return o; }\n"
     "struct ImageColor { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]];\n"
     "  float4 c4 [[color(4)]]; float4 c5 [[color(5)]]; float4 c6 [[color(6)]]; float4 c7 [[color(7)]]; };\n"
     "fragment ImageColor image_fragment(ImageOut in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
     "  uint2 p = uint2(min(in.uv * float2(t.get_width(), t.get_height()), float2(t.get_width() - 1, t.get_height() - 1)));\n"
     "  float4 c = t.read(p); ImageColor o; o.c0 = o.c1 = o.c2 = o.c3 = o.c4 = o.c5 = o.c6 = o.c7 = c; return o; }\n"
     "fragment ImageColor bitmap_fragment(ImageOut in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
     "                                    constant ImageArgs &a [[buffer(0)]]) {\n"
     "  uint2 p = uint2(min(in.uv * float2(t.get_width(), t.get_height()), float2(t.get_width() - 1, t.get_height() - 1)));\n"
     "  if (t.read(p).r < 0.5) discard_fragment();\n"
     "  ImageColor o; o.c0 = o.c1 = o.c2 = o.c3 = o.c4 = o.c5 = o.c6 = o.c7 = a.color; return o; }\n"
     /* glDrawPixels / glCopyPixels of depth (with the raster colour) and of
        stencil indices (the fragment's stencil reference, written by REPLACE). */
     "struct ImageDepth { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]];\n"
     "  float4 c4 [[color(4)]]; float4 c5 [[color(5)]]; float4 c6 [[color(6)]]; float4 c7 [[color(7)]];\n"
     "  float depth [[depth(any)]]; };\n"
     "fragment ImageDepth depth_image_fragment(ImageOut in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
     "                                         constant ImageArgs &a [[buffer(0)]]) {\n"
     "  uint2 p = uint2(min(in.uv * float2(t.get_width(), t.get_height()), float2(t.get_width() - 1, t.get_height() - 1)));\n"
     "  ImageDepth o; o.c0 = o.c1 = o.c2 = o.c3 = o.c4 = o.c5 = o.c6 = o.c7 = a.color; o.depth = t.read(p).r; return o; }\n"
     "struct ImageStencil { uint stencil [[stencil]]; };\n"
     "fragment ImageStencil stencil_image_fragment(ImageOut in [[stage_in]], texture2d<uint> t [[texture(0)]]) {\n"
     "  uint2 p = uint2(min(in.uv * float2(t.get_width(), t.get_height()), float2(t.get_width() - 1, t.get_height() - 1)));\n"
     "  ImageStencil o; o.stencil = t.read(p).r & 0xffu; return o; }\n"
     "struct ClearColor { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]];\n"
     "  float4 c4 [[color(4)]]; float4 c5 [[color(5)]]; float4 c6 [[color(6)]]; float4 c7 [[color(7)]]; };\n"
     "fragment ClearColor clear_fragment(constant ClearValues &v [[buffer(0)]]) {\n"
     "  ClearColor o; o.c0 = o.c1 = o.c2 = o.c3 = o.c4 = o.c5 = o.c6 = o.c7 = v.color; return o; }\n"
     /* glClearBuffer: one draw buffer, with its type's bits. */
     "#define CLEAR(T, name, i) struct name##_out { T c [[color(i)]]; };\\\n"
     "  fragment name##_out name(constant ClearValues &v [[buffer(0)]]) { name##_out o; o.c = as_type<T>(v.color); return o; }\n"
     "#define CLEARS(i) CLEAR(float4, clear_f##i, i) CLEAR(int4, clear_i##i, i) CLEAR(uint4, clear_u##i, i)\n"
     "CLEARS(0) CLEARS(1) CLEARS(2) CLEARS(3) CLEARS(4) CLEARS(5) CLEARS(6) CLEARS(7)\n"
     /* Presentation: the back buffer holds GL rows bottom-up, the drawable
        top-down. */
     "struct PresentOut { float4 position [[position]]; float2 uv; };\n"
     "vertex PresentOut present_vertex(uint vid [[vertex_id]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
     "  PresentOut o; o.position = float4(p * 2.0 - 1.0, 0.0, 1.0); o.uv = float2(p.x, p.y); return o; }\n"
     "fragment float4 present_fragment(PresentOut in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
     "                                 sampler s [[sampler(0)]]) {\n"
     "  return float4(t.sample(s, in.uv).rgb, 1.0); }\n";

static id<MTLFunction> utility_function(struct glm_backend_context *b, NSString *name)
{
    if (!b->utility_library) {
        NSError *error = nil;
        b->utility_library = [device newLibraryWithSource:utility_source options:nil error:&error];
        if (!b->utility_library) glm_log("utility library: %s", error.localizedDescription.UTF8String);
    }
    return [b->utility_library newFunctionWithName:name];
}

void glm_backend_clear(struct glm_context *ctx, GLbitfield mask)
{
    struct glm_backend_context *b = ctx->backend;
    const struct glm_state *s = &ctx->state;
    if (!ensure_encoder(ctx) || scissor_empty(b, s)) return;
    if (!b->clear_vertex) {
        b->clear_vertex = utility_function(b, @"clear_vertex");
        b->clear_fragment = utility_function(b, @"clear_fragment");
        b->clear_vertex_layered = utility_function(b, @"clear_vertex_layered");
    }
    /* Layered attachments clear every layer, an instance each. */
    if (b->layers) b->topology = MTLPrimitiveTopologyClassTriangle;
    id<MTLRenderPipelineState> pipeline = pipeline_for(b, b->layers ? b->clear_vertex_layered : b->clear_vertex,
                                                       b->clear_fragment, s, true, mask, nil, 0, false);
    b->topology = 0;
    if (!pipeline) return;
    enc_pipeline(b, pipeline);
    enc_depth(b, depth_stencil_for(b, s, true, mask));
    uint32_t ref = (uint32_t)s->clear_stencil & 0xff;
    enc_raster(b, b->cache.raster_valid ? b->cache.winding : MTLWindingClockwise, MTLCullModeNone, MTLTriangleFillModeFill,
               0, 0, MTLDepthClipModeClip, ref, ref, b->cache.raster_valid ? b->cache.blend : (const float[4]){0, 0, 0, 0});
    apply_viewport_scissor(b, s, true);
    struct { float color[4]; float depth; float pad[3]; } values;
    memcpy(values.color, s->clear_color, sizeof values.color);
    values.depth = (float)s->clear_depth;
    enc_bytes(b, STAGE_BOTH, &values, sizeof values, 0);
    /* Clears are not fragments of primitives: keep them out of occlusion
       counts. */
    if (b->counting_offset >= 0) [b->encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    [b->encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:MAX(b->layers, 1u)];
    if (b->counting_offset >= 0)
        [b->encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(NSUInteger)b->counting_offset];
}

/* glClearBuffer{f,i,ui}v on a color buffer: `kind` 0 float, 1 int, 2
   unsigned; `values` four 32-bit values. */
void glm_backend_clear_buffer(struct glm_context *ctx, int drawbuffer, int kind, const void *values)
{
    struct glm_backend_context *b = ctx->backend;
    const struct glm_state *s = &ctx->state;
    if (!ensure_encoder(ctx) || scissor_empty(b, s) || drawbuffer < 0 || drawbuffer >= GLM_MAX_DRAW_BUFFERS ||
        !b->color[drawbuffer])
        return;
    if (!b->clear_vertex) {
        b->clear_vertex = utility_function(b, @"clear_vertex");
        b->clear_fragment = utility_function(b, @"clear_fragment");
    }
    if (!b->clear_typed[kind][drawbuffer])
        b->clear_typed[kind][drawbuffer] =
            utility_function(b, [NSString stringWithFormat:@"clear_%c%d", "fiu"[kind], drawbuffer]);
    if (!b->clear_vertex_layered) b->clear_vertex_layered = utility_function(b, @"clear_vertex_layered");
    b->clear_only = drawbuffer;
    if (b->layers) b->topology = MTLPrimitiveTopologyClassTriangle;
    id<MTLRenderPipelineState> pipeline = pipeline_for(b, b->layers ? b->clear_vertex_layered : b->clear_vertex,
                                                       b->clear_typed[kind][drawbuffer], s, true, GL_COLOR_BUFFER_BIT, nil, 0,
                                                       false);
    b->topology = 0;
    b->clear_only = -1;
    if (!pipeline) return;
    enc_pipeline(b, pipeline);
    enc_depth(b, depth_stencil_for(b, s, true, GL_COLOR_BUFFER_BIT));
    enc_raster(b, b->cache.raster_valid ? b->cache.winding : MTLWindingClockwise, MTLCullModeNone, MTLTriangleFillModeFill,
               0, 0, MTLDepthClipModeClip, 0, 0, b->cache.raster_valid ? b->cache.blend : (const float[4]){0, 0, 0, 0});
    apply_viewport_scissor(b, s, true);
    struct { uint32_t color[4]; float depth; float pad[3]; } clear;
    memset(&clear, 0, sizeof clear);
    memcpy(clear.color, values, sizeof clear.color);
    enc_bytes(b, STAGE_BOTH, &clear, sizeof clear, 0);
    if (b->counting_offset >= 0) [b->encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    [b->encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:MAX(b->layers, 1u)];
    if (b->counting_offset >= 0)
        [b->encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(NSUInteger)b->counting_offset];
}

/* glDrawPixels, glCopyPixels and glBitmap: `rgba` (width x height floats,
   bottom row first) as fragments over the window rectangle x0,y0 - x1,y1
   at window depth `depth`, through the current per-fragment state. With
   `bitmap`, the red channel is a mask and the fragments take `color`. */
void glm_backend_draw_image(struct glm_context *ctx, const float *rgba, GLsizei width, GLsizei height, float x0, float y0,
                            float x1, float y1, float depth, bool bitmap, const float *color)
{
    struct glm_backend_context *b = ctx->backend;
    const struct glm_state *s = &ctx->state;
    if (width <= 0 || height <= 0 || !ensure_encoder(ctx) || scissor_empty(b, s)) return;
    if (!b->image_vertex) {
        b->image_vertex = utility_function(b, @"image_vertex");
        b->image_fragment = utility_function(b, @"image_fragment");
        b->bitmap_fragment = utility_function(b, @"bitmap_fragment");
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                 width:(NSUInteger)width
                                                                                height:(NSUInteger)height
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> image = [device newTextureWithDescriptor:d];
    [image replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height) mipmapLevel:0 withBytes:rgba
             bytesPerRow:(NSUInteger)width * 16];
    id<MTLRenderPipelineState> pipeline =
        pipeline_for(b, b->image_vertex, bitmap ? b->bitmap_fragment : b->image_fragment, s, false, 0, nil, 0, false);
    if (!pipeline) return;
    enc_pipeline(b, pipeline);
    enc_depth(b, depth_stencil_for(b, s, false, 0));
    enc_raster(b, b->cache.raster_valid ? b->cache.winding : MTLWindingClockwise, MTLCullModeNone, MTLTriangleFillModeFill,
               0, 0, MTLDepthClipModeClamp, (uint32_t)s->stencil[0].ref & 0xff, (uint32_t)glm_stencil_back(s)->ref & 0xff,
               s->blend_color);
    apply_viewport_scissor(b, s, true);
    struct { float rect[4], color[4], size[2], depth, pad; } args = {
        {x0, y0, x1, y1}, {0, 0, 0, 0}, {(float)b->width, (float)b->height}, depth, 0};
    if (color) memcpy(args.color, color, sizeof args.color);
    enc_bytes(b, STAGE_BOTH, &args, sizeof args, 0);
    glm_encoder_texture(ctx, STAGE_FRAGMENT, (__bridge void *)image, NULL, 0);
    b->fast.valid = false;
    [b->encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

/* glDrawPixels / glCopyPixels of depth values (0..1, drawn with the raster
   colour through the depth test) and / or stencil indices (written through
   the stencil write mask). */
void glm_backend_draw_depth_stencil(struct glm_context *ctx, const float *depth, const uint32_t *stencil, GLsizei width,
                                    GLsizei height, float x0, float y0, float x1, float y1)
{
    struct glm_backend_context *b = ctx->backend;
    const struct glm_state *s = &ctx->state;
    if (width <= 0 || height <= 0 || !ensure_encoder(ctx) || scissor_empty(b, s)) return;
    if (!b->image_vertex) {
        b->image_vertex = utility_function(b, @"image_vertex");
        b->image_fragment = utility_function(b, @"image_fragment");
        b->bitmap_fragment = utility_function(b, @"bitmap_fragment");
    }
    for (int pass = 0; pass < 2; ++pass) {
        bool is_stencil = pass == 1;
        if (is_stencil ? !stencil || !b->stencil : !depth || !b->depth) continue;
        if (!ensure_encoder(ctx)) return;
        MTLTextureDescriptor *d = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:is_stencil ? MTLPixelFormatR32Uint : MTLPixelFormatR32Float
                                         width:(NSUInteger)width
                                        height:(NSUInteger)height
                                     mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> image = [device newTextureWithDescriptor:d];
        [image replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height) mipmapLevel:0
                   withBytes:is_stencil ? (const void *)stencil : (const void *)depth
                 bytesPerRow:(NSUInteger)width * 4];
        id<MTLFunction> fragment = utility_function(b, is_stencil ? @"stencil_image_fragment" : @"depth_image_fragment");
        /* Stencil: no colour or depth writes (a clear-like pipeline). */
        id<MTLRenderPipelineState> pipeline = pipeline_for(b, b->image_vertex, fragment, s, is_stencil,
                                                           is_stencil ? GL_STENCIL_BUFFER_BIT : 0, nil, 0, false);
        if (!pipeline) return;
        enc_pipeline(b, pipeline);
        enc_depth(b, depth_stencil_for(b, s, is_stencil, is_stencil ? GL_STENCIL_BUFFER_BIT : 0));
        enc_raster(b, b->cache.raster_valid ? b->cache.winding : MTLWindingClockwise, MTLCullModeNone,
                   MTLTriangleFillModeFill, 0, 0, MTLDepthClipModeClamp, (uint32_t)s->stencil[0].ref & 0xff,
                   (uint32_t)glm_stencil_back(s)->ref & 0xff, s->blend_color);
        apply_viewport_scissor(b, s, true);
        struct { float rect[4], color[4], size[2], depth, pad; } args = {
            {x0, y0, x1, y1}, {0, 0, 0, 0}, {(float)b->width, (float)b->height}, 0, 0};
        memcpy(args.color, s->raster_color, sizeof args.color);
        enc_bytes(b, STAGE_BOTH, &args, sizeof args, 0);
        glm_encoder_texture(ctx, STAGE_FRAGMENT, (__bridge void *)image, NULL, 0);
        b->fast.valid = false;
        [b->encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
}

void glm_backend_upload_encoded(struct glm_context *ctx)
{
    if (ctx->presents) return;
    pthread_mutex_lock(&ctx->share->lock);
    bool shared = ctx->share->refcount > 1;
    pthread_mutex_unlock(&ctx->share->lock);
    if (shared) glm_backend_flush(ctx, false);
}

/* ---- presentation -------------------------------------------------------- */

/* GLMETAL_DUMP_PRESENT=path[:interval]: writes the presented back buffer of
   every interval-th frame (default 300) to path.<frame>.ppm, top row first:
   what the window shows, independent of the application's own readbacks. */
static void dump_presented(struct glm_backend_context *b, id<MTLCommandBuffer> commands)
{
    static char path[1024];
    static long interval = -1;
    if (interval < 0) {
        const char *env = getenv("GLMETAL_DUMP_PRESENT");
        interval = 0;
        if (env && env[0]) {
            snprintf(path, sizeof path, "%s", env);
            char *colon = strrchr(path, ':');
            interval = colon ? atol(colon + 1) : 300;
            if (colon) *colon = 0;
            if (interval <= 0) interval = 300;
        }
    }
    if (!interval || glm_trace_frames % (unsigned long)interval) return;
    NSUInteger width = b->default_color.width, height = b->default_color.height, row = width * 4;
    id<MTLBuffer> staging = glm_new_shared_buffer(row * height, NULL);
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:b->default_color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1) toBuffer:staging destinationOffset:0
      destinationBytesPerRow:row destinationBytesPerImage:row * height];
    [blit endEncoding];
    char file[1100];
    snprintf(file, sizeof file, "%s.%lu.ppm", path, glm_trace_frames);
    NSString *name = @(file);
    [commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        FILE *f = fopen(name.UTF8String, "wb");
        if (!f) return;
        fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)width, (unsigned long)height);
        const uint8_t *pixels = mtl_contents(staging);
        for (NSUInteger y = height; y-- > 0;) /* GL rows run bottom up */
            for (NSUInteger x = 0; x < width; ++x) fwrite(pixels + y * row + x * 4, 3, 1, f);
        fclose(f);
    }];
}

/* Copies the back buffer into the layer's next drawable and presents it
   with the frame's command buffer. Returns false without a drawable. */
bool glm_backend_present(struct glm_context *ctx, void *layer_pointer, bool opaque)
{
    struct glm_backend_context *b = ctx->backend;
    CAMetalLayer *layer = (__bridge CAMetalLayer *)layer_pointer;
    if (!b->default_color || !layer) return false;
    end_encoder(b);
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) {
        glm_backend_flush(ctx, false);
        return false;
    }
    if (!b->present_vertex) {
        b->present_vertex = utility_function(b, @"present_vertex");
        b->present_fragment = utility_function(b, @"present_fragment");
    }
    MTLPixelFormat format = drawable.texture.pixelFormat;
    if (!b->present_pipeline || b->present_format != format) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = b->present_vertex;
        d.fragmentFunction = b->present_fragment;
        d.colorAttachments[0].pixelFormat = format;
        NSError *error = nil;
        b->present_pipeline = [device newRenderPipelineStateWithDescriptor:d error:&error];
        b->present_format = format;
        if (!b->present_pipeline) {
            glm_log("present pipeline: %s", error.localizedDescription.UTF8String);
            return false;
        }
    }
    bool scaled = drawable.texture.width != b->default_color.width || drawable.texture.height != b->default_color.height;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = scaled ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> commands = command_buffer(b);
    id<MTLRenderCommandEncoder> e = [commands renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:b->present_pipeline];
    [e setFragmentTexture:b->default_color atIndex:0];
    [e setFragmentSamplerState:sampler atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    (void)opaque;
    dump_presented(b, commands);
    ++glm_trace_frames;
    report_slow_frame();
    [commands presentDrawable:drawable];
    glm_backend_flush(ctx, false);
    return true;
}

GLM_HIDDEN void glm_backend_default_size(struct glm_context *ctx, GLsizei *width, GLsizei *height)
{
    struct glm_backend_context *b = ctx->backend;
    *width = b->default_color ? (GLsizei)b->default_color.width : 0;
    *height = b->default_color ? (GLsizei)b->default_color.height : 0;
}

/* ---- occlusion queries --------------------------------------------------- */

void glm_backend_occlusion_draw(struct glm_context *ctx, struct glm_query *q)
{
    struct glm_backend_context *b = ctx->backend;
    if (!b->pass_has_visibility || b->visibility_used + 8 > b->visibility.length) {
        /* The pass predates the query (or ran out of counters): restart it
           with a visibility buffer attached. */
        b->framebuffer_dirty = true;
        if (!ensure_encoder(ctx)) return;
    }
    if (q->pass_id != b->pass_id) {
        NSUInteger offset = b->visibility_used;
        b->visibility_used += 8;
        glm_query_add_segment(q, (__bridge_retained void *)b->visibility, (uint32_t)offset);
        q->pass_id = b->pass_id;
        b->counting_offset = (int64_t)offset;
        [b->encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:offset];
    }
}

void glm_backend_occlusion_end(struct glm_context *ctx)
{
    struct glm_backend_context *b = ctx->backend;
    if (b->encoder && b->counting_offset >= 0) [b->encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    b->counting_offset = -1;
}

uint64_t glm_backend_pending_serial(struct glm_context *ctx)
{
    struct glm_backend_context *b = ctx->backend;
    return b->submitted_serial + (b->commands ? 1 : 0);
}

uint64_t glm_backend_buffer_read_u64(void *buffer, uint32_t offset)
{
    uint64_t v;
    memcpy(&v, (uint8_t *)mtl_contents(((__bridge id<MTLBuffer>)buffer)) + offset, sizeof v);
    return v;
}

/* ---- draw ---------------------------------------------------------------- */

GLM_HIDDEN bool glm_bind_textures(struct glm_context *ctx, id<MTLRenderCommandEncoder> encoder, const struct glm_ff_key *key,
                                  float (*border_data)[4], float (*lod_bias)[4]);

static void ff_functions(struct glm_backend_context *b, const struct glm_ff_key *key, id<MTLFunction> *vertex,
                         id<MTLFunction> *fragment)
{
    if (b->last_ff_vertex && !memcmp(key, &b->last_ff_key, sizeof *key)) {
        *vertex = b->last_ff_vertex;
        *fragment = b->last_ff_fragment;
        return;
    }
    NSData *data = [NSData dataWithBytes:key length:sizeof *key];
    *vertex = b->ff_vertex[data];
    *fragment = b->ff_fragment[data];
    if (*vertex && *fragment) {
        b->last_ff_key = *key;
        b->last_ff_vertex = *vertex;
        b->last_ff_fragment = *fragment;
        return;
    }
    char *source = glm_ff_generate(key);
    NSString *text = [NSString stringWithUTF8String:source];
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.fastMathEnabled = NO;
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:text options:options error:&error];
    if (!library) {
        glm_log("fixed-function shader failed: %s\n%s", error.localizedDescription.UTF8String, source);
        free(source);
        return;
    }
    free(source);
    *vertex = [library newFunctionWithName:@"ff_vertex"];
    remember_function_source(*vertex, text);
    *fragment = [library newFunctionWithName:@"ff_fragment"];
    remember_function_source(*fragment, text);
    b->ff_vertex[data] = *vertex;
    b->ff_fragment[data] = *fragment;
    b->last_ff_key = *key;
    b->last_ff_vertex = *vertex;
    b->last_ff_fragment = *fragment;
}

/* ---- programs ------------------------------------------------------------ */

GLM_HIDDEN bool glm_bind_program_textures(struct glm_context *ctx, id<MTLRenderCommandEncoder> encoder,
                                          const struct glm_program *program, unsigned stages, uint32_t border_mask,
                                          float (*lod_bias)[4]);
GLM_HIDDEN uint32_t glm_program_border_mask(struct glm_context *ctx, const struct glm_program *program, float (*data)[4]);
GLM_HIDDEN extern int glm_custom_borders;
GLM_HIDDEN void glm_legacy_block_build(struct glm_context *ctx, uint32_t parts, struct glm_legacy_block *b);
GLM_HIDDEN bool glm_legacy_parts_equal(const struct glm_legacy_block *a, const struct glm_legacy_block *b, uint32_t parts);

struct glm_int_variant {
    uint32_t uint_inputs, int_inputs, border_mask;
    uint32_t alpha_func;      /* alpha test variant: GL func - GL_NEVER + 1, 0 = none */
    int refs;                 /* program + running job, under variant_lock */
    bool ready;
    void *function;           /* id<MTLFunction>, NULL if the compile failed */
    void *group;              /* dispatch_group_t, left once ready */
};
static pthread_mutex_t variant_lock = PTHREAD_MUTEX_INITIALIZER;

static void variant_release(struct glm_int_variant *v)
{
    pthread_mutex_lock(&variant_lock);
    bool last = --v->refs == 0;
    pthread_mutex_unlock(&variant_lock);
    if (!last) return;
    if (v->function) CFRelease(v->function);
    if (v->group) CFRelease(v->group);
    free(v);
}

/* Functions of released programs stay alive: pipeline cache keys hold
   function addresses, which must not be reused. */
static NSMutableArray *retired_functions;

/* A Metal function by MSL source text, compiled once per process: done
   (function or error) or still compiling (group entered until then). */
@interface GLMCompiledFunction : NSObject {
@public
    dispatch_group_t group;
    id<MTLFunction> function;
    NSError *error;
}
@end
@implementation GLMCompiledFunction
@end

static NSMutableDictionary<NSString *, GLMCompiledFunction *> *function_cache;
static pthread_mutex_t function_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMapTable<id<MTLFunction>, NSString *> *function_sources;
static void remember_function_source(id<MTLFunction> function, NSString *source)
{
    if (!function || !source) return;
    pthread_mutex_lock(&function_cache_lock);
    if (!function_sources)
        function_sources = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsWeakMemory | NSPointerFunctionsObjectPointerPersonality
                                               valueOptions:NSPointerFunctionsStrongMemory];
    [function_sources setObject:source forKey:function];
    pthread_mutex_unlock(&function_cache_lock);
}

/* The entry for `text`, its compile started if new (Metal's compiler takes
   ~20 ms a source but runs many at once: callers start what they will need
   early and wait late). */
static GLMCompiledFunction *function_entry(NSString *text)
{
    static MTLCompileOptions *options;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        function_cache = [NSMutableDictionary dictionary];
        options = [MTLCompileOptions new];
        options.fastMathEnabled = NO;
        options.languageVersion = MTLLanguageVersion2_3;
    });
    pthread_mutex_lock(&function_cache_lock);
    GLMCompiledFunction *entry = function_cache[text];
    if (entry) {
        pthread_mutex_unlock(&function_cache_lock);
        return entry;
    }
    entry = [GLMCompiledFunction new];
    entry->group = dispatch_group_create();
    dispatch_group_enter(entry->group);
    function_cache[text] = entry;
    pthread_mutex_unlock(&function_cache_lock);
    GLMCompiledFunction *target = entry;
    /* GLMETAL_MSL_SALT=1 (benchmarks): a per-process comment defeats Metal's
       own shader cache, so compiles are cold. */
    static NSString *salt;
    static dispatch_once_t salted;
    dispatch_once(&salted, ^{
        salt = getenv("GLMETAL_MSL_SALT") ? [NSString stringWithFormat:@"// salt %d %u\n", getpid(), arc4random()] : nil;
    });
    [device newLibraryWithSource:salt ? [salt stringByAppendingString:text] : text options:options
               completionHandler:^(id<MTLLibrary> library, NSError *error) {
                   target->function = [library newFunctionWithName:@"main0"];
                   remember_function_source(target->function, text);
                   if (!target->function) {
                       target->error = error;
                       /* A failure is not kept: the next request retries. */
                       pthread_mutex_lock(&function_cache_lock);
                       if (function_cache[text] == target) [function_cache removeObjectForKey:text];
                       pthread_mutex_unlock(&function_cache_lock);
                   }
                   dispatch_group_leave(target->group);
               }];
    return entry;
}

/* Wrap the final selected function, so alpha and border variants retain their behavior. */
static id<MTLFunction> sample_shading_function(struct glm_backend_context *b, id<MTLFunction> function)
{
    static NSMutableDictionary<NSArray<NSString *> *, id> *variants;
    pthread_mutex_lock(&function_cache_lock);
    NSString *source = [function_sources objectForKey:function];
    if (!source) {
        pthread_mutex_unlock(&function_cache_lock);
        return function;
    }
    NSArray<NSString *> *key = @[source, function.name];
    if (!variants) variants = [NSMutableDictionary dictionary];
    id cached = variants[key];
    pthread_mutex_unlock(&function_cache_lock);
    if (!cached) {
        char *patched = glm_sample_shading_msl(source.UTF8String, function.name.UTF8String);
        cached = patched ? function_entry(@(patched)) : (id)[NSNull null];
        free(patched);
        pthread_mutex_lock(&function_cache_lock);
        variants[key] = cached;
        pthread_mutex_unlock(&function_cache_lock);
    }
    if (cached == [NSNull null]) return function;
    GLMCompiledFunction *entry = cached;
    if (dispatch_group_wait(entry->group, DISPATCH_TIME_NOW)) {
        dispatch_group_wait(entry->group, DISPATCH_TIME_FOREVER);
    }
    if (!entry->function) {
        glm_log("sample shading shader failed: %s", entry->error.localizedDescription.UTF8String);
        return nil;
    }
    return entry->function;
}

static id<MTLFunction> depth_clamp_function(struct glm_backend_context *b, id<MTLFunction> function, bool *active)
{
    static NSMutableDictionary<NSArray<NSString *> *, id> *variants;
    pthread_mutex_lock(&function_cache_lock);
    NSString *source = [function_sources objectForKey:function];
    if (!source) {
        pthread_mutex_unlock(&function_cache_lock);
        return function;
    }
    /* Source and entry remain stable when a context releases its FF functions.
       The weak catalog never keeps those original functions alive. */
    NSArray<NSString *> *key = @[source, function.name];
    if (!variants) variants = [NSMutableDictionary dictionary];
    id cached = variants[key];
    pthread_mutex_unlock(&function_cache_lock);
    if (!cached) {
        char *patched = glm_depth_clamp_msl(source.UTF8String, function.name.UTF8String);
        cached = patched ? function_entry(@(patched)) : (id)[NSNull null];
        free(patched);
        pthread_mutex_lock(&function_cache_lock);
        variants[key] = cached;
        pthread_mutex_unlock(&function_cache_lock);
    }
    if (cached == [NSNull null]) return function;
    GLMCompiledFunction *entry = cached;
    if (dispatch_group_wait(entry->group, DISPATCH_TIME_NOW)) {
        dispatch_group_wait(entry->group, DISPATCH_TIME_FOREVER);
    }
    if (!entry->function) {
        glm_log("depth clamp shader failed: %s", entry->error.localizedDescription.UTF8String);
        return nil;
    }
    *active = true;
    return entry->function;
}

/* Clip enables affect rasterization, not captured shader output values. */
static id<MTLFunction> clip_mask_function(struct glm_backend_context *b, id<MTLFunction> function, uint32_t mask)
{
    static NSMutableDictionary<NSArray *, id> *variants;
    static NSMapTable<id<MTLFunction>, NSMutableDictionary<NSNumber *, id> *> *by_function;
    NSNumber *mask_key = @(mask);
    NSString *source = nil;
    NSArray *key = nil;
    pthread_mutex_lock(&function_cache_lock);
    /* A draw usually revisits the same function and mask. Avoid the source
       array key on that path: NSArray hashes collide for equal-length keys,
       making lookup compare many shader sources as the cache grows. Weak
       function keys prevent stale pointer hits after a program is deleted. */
    NSMutableDictionary<NSNumber *, id> *masks = [by_function objectForKey:function];
    id cached = masks[mask_key];
    if (cached) {
        pthread_mutex_unlock(&function_cache_lock);
        goto resolve;
    }
    source = [function_sources objectForKey:function];
    if (!source) {
        pthread_mutex_unlock(&function_cache_lock);
        return function;
    }
    key = @[source, function.name, mask_key];
    if (!variants) variants = [NSMutableDictionary dictionary];
    cached = variants[key];
    pthread_mutex_unlock(&function_cache_lock);
    if (!cached) {
        char *patched = glm_clip_mask_msl(source.UTF8String, function.name.UTF8String, mask);
        cached = patched ? function_entry(@(patched)) : (id)[NSNull null];
        free(patched);
        pthread_mutex_lock(&function_cache_lock);
        variants[key] = cached;
        pthread_mutex_unlock(&function_cache_lock);
    }
    pthread_mutex_lock(&function_cache_lock);
    if (!by_function) by_function = [NSMapTable weakToStrongObjectsMapTable];
    masks = [by_function objectForKey:function];
    if (!masks) {
        masks = [NSMutableDictionary dictionary];
        [by_function setObject:masks forKey:function];
    }
    masks[mask_key] = cached;
    pthread_mutex_unlock(&function_cache_lock);
resolve:
    if (cached == [NSNull null]) return function;
    GLMCompiledFunction *entry = cached;
    if (dispatch_group_wait(entry->group, DISPATCH_TIME_NOW)) {
        dispatch_group_wait(entry->group, DISPATCH_TIME_FOREVER);
    }
    if (!entry->function) {
        glm_log("clip mask shader failed: %s", entry->error.localizedDescription.UTF8String);
        return nil;
    }
    return entry->function;
}

/* Starts compiling the sources' functions without waiting. */
static void metal_functions_prewarm(const char *const *sources, int count)
{
    for (int i = 0; i < count; ++i)
        if (sources[i]) function_entry(@(sources[i]));
}

/* main0 of each MSL source in `sources` (NULL entries skipped) into
   `functions`, compiled concurrently and kept by source text for the
   process. Returns false with `*message` (malloc'd) on the first failure. */
static bool metal_functions(const char *const *sources, int count, __strong id<MTLFunction> *functions, char **message)
{
    GLMCompiledFunction *entries[8] = {nil};
    for (int i = 0; i < count && i < 8; ++i) {
        functions[i] = nil;
        if (sources[i]) entries[i] = function_entry(@(sources[i]));
    }
    for (int i = 0; i < count && i < 8; ++i) {
        if (!entries[i]) continue;
        if (dispatch_group_wait(entries[i]->group, DISPATCH_TIME_NOW)) {
            uint64_t started = glm_now_ns();
            dispatch_group_wait(entries[i]->group, DISPATCH_TIME_FOREVER);
            glm_note_stall("waiting for the Metal compiler", started);
        }
        functions[i] = entries[i]->function;
        if (!functions[i]) {
            const char *reason = entries[i]->error ? entries[i]->error.localizedDescription.UTF8String : "no main0";
            size_t n = strlen(reason) + strlen(sources[i]) + 64;
            *message = malloc(n);
            snprintf(*message, n, "Metal compile failed: %s\n%s", reason, sources[i]);
            return false;
        }
    }
    return true;
}

/* Integer-input variants a vertex MSL source needed (games feed programs
   sharing a vertex shader the same vertex formats): linked programs with
   that source compile the variant along with their own stages. */
static char *int_variant_msl(const char *msl, uint32_t uint_inputs, uint32_t int_inputs);
static NSMutableDictionary<NSString *, NSNumber *> *predicted_variants;
static pthread_mutex_t predicted_lock = PTHREAD_MUTEX_INITIALIZER;

/* Locations any program has had integer data fed to float inputs at. */
static uint32_t seen_uint_inputs, seen_int_inputs;

static void remember_variant(const char *vertex_msl, uint32_t uint_inputs, uint32_t int_inputs)
{
    if (!vertex_msl) return;
    __atomic_or_fetch(&seen_uint_inputs, uint_inputs, __ATOMIC_RELAXED);
    __atomic_or_fetch(&seen_int_inputs, int_inputs, __ATOMIC_RELAXED);
    pthread_mutex_lock(&predicted_lock);
    if (!predicted_variants) predicted_variants = [NSMutableDictionary dictionary];
    predicted_variants[@(vertex_msl)] = @((uint64_t)uint_inputs | (uint64_t)int_inputs << 32);
    pthread_mutex_unlock(&predicted_lock);
}

/* The variant a program will likely need: what its vertex source needed
   before, else the locations other programs were fed integers at (a game
   uses the same vertex formats for many shaders). */
static bool predicted_variant(const char *vertex_msl, uint32_t *uint_inputs, uint32_t *int_inputs);
static bool guess_variant(const struct glm_compile_result *r, uint32_t *uint_inputs, uint32_t *int_inputs)
{
    if (predicted_variant(r->msl[GLM_STAGE_VERTEX], uint_inputs, int_inputs)) return true;
    uint32_t float_inputs = 0;
    for (int i = 0; i < r->attribute_count; ++i)
        if (!r->attributes[i].integer && r->attributes[i].index != GLM_INPUT_BUILTIN && r->attributes[i].location >= 0 &&
            r->attributes[i].location < 32)
            float_inputs |= 1u << r->attributes[i].location;
    *uint_inputs = __atomic_load_n(&seen_uint_inputs, __ATOMIC_RELAXED) & float_inputs;
    *int_inputs = __atomic_load_n(&seen_int_inputs, __ATOMIC_RELAXED) & float_inputs & ~*uint_inputs;
    return *uint_inputs || *int_inputs;
}

static bool predicted_variant(const char *vertex_msl, uint32_t *uint_inputs, uint32_t *int_inputs)
{
    if (!vertex_msl || !predicted_variants) return false;
    pthread_mutex_lock(&predicted_lock);
    NSNumber *masks = predicted_variants[@(vertex_msl)];
    pthread_mutex_unlock(&predicted_lock);
    if (!masks) return false;
    *uint_inputs = (uint32_t)masks.unsignedLongLongValue;
    *int_inputs = (uint32_t)(masks.unsignedLongLongValue >> 32);
    return true;
}

/* Starts compiling a compile result's Metal functions (and its predicted
   integer-input variant) in the background: the later link finds them
   compiled or compiling (any thread). */
void glm_backend_prewarm_program(const struct glm_compile_result *r)
{
    if (!r->ok || r->gs || r->tess) return;
    uint32_t uint_inputs = 0, int_inputs = 0;
    char *variant = guess_variant(r, &uint_inputs, &int_inputs)
                        ? int_variant_msl(r->msl[GLM_STAGE_VERTEX], uint_inputs, int_inputs)
                        : NULL;
    const char *sources[4] = {r->msl[GLM_STAGE_VERTEX], r->msl[GLM_STAGE_FRAGMENT], r->msl_capture, variant};
    metal_functions_prewarm(sources, 4);
    free(variant);
}

bool glm_backend_program_link(struct glm_context *ctx, struct glm_program *p, char **log)
{
    static uint64_t links;
    p->link_serial = __atomic_add_fetch(&links, 1, __ATOMIC_RELAXED); /* memos keyed by it (vertex_inputs) */
    (void)ctx;
    glm_backend_program_release(p);
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.fastMathEnabled = NO;
    options.languageVersion = MTLLanguageVersion2_3;
    for (int stage = 0; stage < GLM_STAGE_COUNT; ++stage) {
        if (!p->result.msl[stage] || stage == GLM_STAGE_VERTEX || stage == GLM_STAGE_FRAGMENT) continue;
        *log = strdup("geometry and tessellation shaders are not supported yet\n");
        return false;
    }
    if (getenv("GLM_DUMP_SHADERS"))
        for (int stage = 0; stage < GLM_STAGE_COUNT; ++stage)
            if (p->result.msl[stage]) glm_log("program stage %d MSL:\n%s", stage, p->result.msl[stage]);
    uint32_t predicted_uint = 0, predicted_int = 0;
    char *variant = guess_variant(&p->result, &predicted_uint, &predicted_int)
                        ? int_variant_msl(p->result.msl[GLM_STAGE_VERTEX], predicted_uint, predicted_int)
                        : NULL;
    const char *sources[4] = {p->result.msl[GLM_STAGE_VERTEX], p->result.msl[GLM_STAGE_FRAGMENT], p->result.msl_capture,
                              variant};
    __strong id<MTLFunction> compiled[4];
    bool compiled_ok = metal_functions(sources, 4, compiled, log);
    free(variant);
    if (!compiled_ok) return false;
    if (compiled[0]) p->functions[GLM_STAGE_VERTEX] = (__bridge_retained void *)compiled[0];
    if (compiled[1]) p->functions[GLM_STAGE_FRAGMENT] = (__bridge_retained void *)compiled[1];
    if (compiled[2]) p->functions_capture = (__bridge_retained void *)compiled[2];
    if (compiled[3] && p->int_variant_count < 16) {
        struct glm_int_variant *v = calloc(1, sizeof *v);
        v->uint_inputs = predicted_uint;
        v->int_inputs = predicted_int;
        v->refs = 1;
        v->ready = true;
        v->function = (__bridge_retained void *)compiled[3];
        p->int_variants[p->int_variant_count++] = v;
    }
    if (p->result.gs && p->result.gs->kernel && p->result.gs->pull) {
        struct glm_gs_result *g = p->result.gs;
        struct glm_gs_state *gs = calloc(1, sizeof *gs);
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@(g->vs_capture) options:options error:&error];
        id<MTLFunction> capture = [library newFunctionWithName:@"main0"];
        id<MTLLibrary> kernel_library = [device newLibraryWithSource:@(g->kernel->msl[GLM_STAGE_COMPUTE]) options:options
                                                               error:&error];
        id<MTLFunction> kernel = [kernel_library newFunctionWithName:@"main0"];
        id<MTLComputePipelineState> pipeline = kernel ? [device newComputePipelineStateWithFunction:kernel error:&error] : nil;
        if (!capture || !pipeline) {
            const char *message = error.localizedDescription.UTF8String;
            size_t n = strlen(message) + 64;
            *log = malloc(n);
            snprintf(*log, n, "geometry emulation: %s\n", message);
            free(gs);
            return false;
        }
        gs->vs_capture = (__bridge_retained void *)capture;
        gs->kernel = (__bridge_retained void *)pipeline;
        gs->kernel_globals = calloc(1, (size_t)(g->kernel->global_size ? g->kernel->global_size : 16));
        struct glm_program *pull = calloc(1, sizeof *pull);
        pull->result = *g->pull;
        pull->borrowed_result = true;
        pull->linked = true;
        pull->globals = calloc(1, (size_t)(g->pull->global_size ? g->pull->global_size : 16));
        if (!glm_backend_program_link(ctx, pull, log)) {
            free(pull->globals);
            free(pull);
            free(gs->kernel_globals);
            free(gs);
            return false;
        }
        gs->pull = pull;
        p->gs = gs;
    }
    if (p->result.tess && p->result.tess->kernel && p->result.tess->eval) {
        const struct glm_tess_result *t = p->result.tess;
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@(t->vs_capture) options:options error:&error];
        id<MTLFunction> capture = [library newFunctionWithName:@"main0"];
        id<MTLLibrary> kernel_library = [device newLibraryWithSource:@(t->kernel->msl[GLM_STAGE_COMPUTE]) options:options
                                                               error:&error];
        id<MTLFunction> kernel = [kernel_library newFunctionWithName:@"main0"];
        id<MTLComputePipelineState> pipeline = kernel ? [device newComputePipelineStateWithFunction:kernel error:&error] : nil;
        if (!capture || !pipeline) {
            const char *message = error.localizedDescription.UTF8String;
            size_t n = strlen(message) + 64;
            *log = malloc(n);
            snprintf(*log, n, "tessellation emulation: %s\n", message);
            return false;
        }
        struct glm_program *eval = calloc(1, sizeof *eval);
        eval->result = *t->eval;
        eval->borrowed_result = true;
        eval->linked = true;
        eval->globals = calloc(1, (size_t)(t->eval->global_size ? t->eval->global_size : 16));
        if (!glm_backend_program_link(ctx, eval, log)) {
            free(eval->globals);
            free(eval);
            return false;
        }
        /* Control points and per-patch data, float4 slots each. */
        MTLVertexDescriptor *layout = [MTLVertexDescriptor vertexDescriptor];
        bool patch_data = false;
        for (int i = 0; i < t->input_count; ++i) {
            const struct glm_tess_input *in = &t->inputs[i];
            layout.attributes[in->attribute].format = !in->integer ? MTLVertexFormatFloat4
                                                      : in->is_unsigned ? MTLVertexFormatUInt4 : MTLVertexFormatInt4;
            layout.attributes[in->attribute].offset = (NSUInteger)in->slot * 16;
            layout.attributes[in->attribute].bufferIndex = in->patch ? GLM_SLOT_TESS_PATCH : GLM_SLOT_TESS_POINTS;
            patch_data |= in->patch;
        }
        layout.layouts[GLM_SLOT_TESS_POINTS].stride = (NSUInteger)t->cp_slots * 16;
        layout.layouts[GLM_SLOT_TESS_POINTS].stepFunction = MTLVertexStepFunctionPerPatchControlPoint;
        if (patch_data) {
            layout.layouts[GLM_SLOT_TESS_PATCH].stride = (NSUInteger)t->patch_slots * 16;
            layout.layouts[GLM_SLOT_TESS_PATCH].stepFunction = MTLVertexStepFunctionPerPatch;
        }
        if (eval->vertex_descriptor) CFRelease(eval->vertex_descriptor);
        eval->vertex_descriptor = (__bridge_retained void *)layout;
        struct glm_tess_state *tess = calloc(1, sizeof *tess);
        tess->vs_capture = (__bridge_retained void *)capture;
        tess->kernel = (__bridge_retained void *)pipeline;
        tess->kernel_globals = calloc(1, (size_t)(t->kernel->global_size ? t->kernel->global_size : 16));
        tess->eval = eval;
        p->tess = tess;
    }
    /* Inputs read the float4 stream; integer inputs read the same slots as
       integers (the fetch stores their values as integers). */
    MTLVertexDescriptor *layout = [MTLVertexDescriptor vertexDescriptor];
    for (int i = 0; i < p->result.attribute_count; ++i) {
        const struct glm_io_info *a = &p->result.attributes[i];
        if (a->index == GLM_INPUT_BUILTIN) continue;
        int columns = 1;
        switch (a->type) {
        case GL_FLOAT_MAT2: case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT2x4: columns = 2; break;
        case GL_FLOAT_MAT3: case GL_FLOAT_MAT3x2: case GL_FLOAT_MAT3x4: columns = 3; break;
        case GL_FLOAT_MAT4: case GL_FLOAT_MAT4x2: case GL_FLOAT_MAT4x3: columns = 4; break;
        }
        for (int c = 0; c < columns * a->array_size; ++c) {
            int location = a->location + c;
            if (location < 0 || location >= GLM_MAX_ATTRIBS) continue;
            bool is_unsigned = a->type == GL_UNSIGNED_INT || (a->type >= GL_UNSIGNED_INT_VEC2 && a->type <= GL_UNSIGNED_INT_VEC4);
            layout.attributes[location].format = !a->integer ? MTLVertexFormatFloat4
                                                 : is_unsigned ? MTLVertexFormatUInt4 : MTLVertexFormatInt4;
            layout.attributes[location].offset = (NSUInteger)location * 16;
            layout.attributes[location].bufferIndex = GLM_SLOT_STREAM;
        }
    }
    layout.layouts[GLM_SLOT_STREAM].stride = GLM_MAX_ATTRIBS * 16;
    layout.layouts[GLM_SLOT_STREAM].stepFunction = MTLVertexStepFunctionPerVertex;
    p->vertex_descriptor = (__bridge_retained void *)layout;
    /* Optional precision path: unsupported generated interfaces keep the
       ordinary vertex pipeline. Its scratch storage is allocated per draw. */
    const struct glm_clip_result *clip = p->result.clip;
    if (clip && clip->vs_capture && clip->kernel && clip->kernel->ok && clip->kernel->msl[GLM_STAGE_COMPUTE] &&
        clip->pull && clip->pull->ok) {
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@(clip->vs_capture) options:options error:&error];
        id<MTLFunction> capture = [library newFunctionWithName:@"main0"];
        id<MTLLibrary> compute_library = [device newLibraryWithSource:@(clip->kernel->msl[GLM_STAGE_COMPUTE])
                                                            options:options error:&error];
        id<MTLFunction> kernel = [compute_library newFunctionWithName:@"main0"];
        id<MTLComputePipelineState> compute = kernel ? [device newComputePipelineStateWithFunction:kernel error:&error] : nil;
        struct glm_program *pull = calloc(1, sizeof *pull);
        pull->result = *clip->pull;
        pull->borrowed_result = true;
        pull->linked = true;
        pull->globals = calloc(1, (size_t)MAX(clip->pull->global_size, 16));
        char *clip_log = NULL;
        if (capture && compute && glm_backend_program_link(ctx, pull, &clip_log)) {
            remember_function_source(capture, @(clip->vs_capture));
            p->clip = calloc(1, sizeof *p->clip);
            p->clip->vs_capture = (__bridge_retained void *)capture;
            p->clip->kernel = (__bridge_retained void *)compute;
            p->clip->pull = pull;
        } else {
            glm_backend_program_release(pull);
            free(pull->globals);
            free(pull);
            const char *message = clip_log ? clip_log : error.localizedDescription.UTF8String;
            glm_log("post-vertex clipping unavailable: %s", message ? message : "unsupported generated interface");
        }
        free(clip_log);
    }
    return true;
}

void glm_backend_program_release(struct glm_program *p)
{
    @synchronized([NSObject class]) {
        if (!retired_functions) retired_functions = [NSMutableArray array];
        for (int stage = 0; stage < GLM_STAGE_COUNT; ++stage) {
            if (!p->functions[stage]) continue;
            [retired_functions addObject:(__bridge_transfer id<MTLFunction>)p->functions[stage]];
            p->functions[stage] = NULL;
        }
        if (p->functions_capture) {
            [retired_functions addObject:(__bridge_transfer id<MTLFunction>)p->functions_capture];
            p->functions_capture = NULL;
        }
        if (p->gs) {
            [retired_functions addObject:(__bridge_transfer id<MTLFunction>)p->gs->vs_capture];
            [retired_functions addObject:(__bridge_transfer id)p->gs->kernel];
            glm_backend_program_release(p->gs->pull);
            free(p->gs->pull->globals);
            free(p->gs->pull);
            free(p->gs->kernel_globals);
            free(p->gs);
            p->gs = NULL;
        }
        /* Pipeline keys hold function addresses: keep ready ones alive. */
        for (int i = 0; i < p->int_variant_count; ++i) {
            struct glm_int_variant *v = p->int_variants[i];
            pthread_mutex_lock(&variant_lock);
            if (v->ready && v->function) [retired_functions addObject:(__bridge id<MTLFunction>)v->function];
            pthread_mutex_unlock(&variant_lock);
            variant_release(v);
        }
        p->int_variant_count = 0;
        if (p->tess) {
            [retired_functions addObject:(__bridge_transfer id<MTLFunction>)p->tess->vs_capture];
            [retired_functions addObject:(__bridge_transfer id)p->tess->kernel];
            glm_backend_program_release(p->tess->eval);
            free(p->tess->eval->globals);
            free(p->tess->eval);
            free(p->tess->kernel_globals);
            free(p->tess);
            p->tess = NULL;
        }
        if (p->clip) {
            [retired_functions addObject:(__bridge_transfer id<MTLFunction>)p->clip->vs_capture];
            glm_backend_release(p->clip->kernel);
            glm_backend_program_release(p->clip->pull);
            free(p->clip->pull->globals);
            free(p->clip->pull);
            free(p->clip);
            p->clip = NULL;
        }
    }
    if (p->vertex_descriptor) CFRelease(p->vertex_descriptor);
    p->vertex_descriptor = NULL;
}

/* A fragment stage for programs without one: writes nothing. */
static id<MTLFunction> empty_fragment(struct glm_backend_context *b)
{
    static id<MTLFunction> function;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        id<MTLLibrary> library = [device newLibraryWithSource:@"fragment void glm_empty_fragment() {}" options:nil error:nil];
        function = [library newFunctionWithName:@"glm_empty_fragment"];
        remember_function_source(function, @"#include <metal_stdlib>\nusing namespace metal;\nfragment void glm_empty_fragment() {}");
    });
    return function;
}

static void set_stages(struct glm_backend_context *b, __unsafe_unretained id<MTLBuffer> buffer, NSUInteger offset, NSUInteger slot,
                       unsigned stages)
{
    enc_buffer(b, stages, buffer, offset, slot);
}

/* GLMARB: env[env_count] then local[local_count], std140 vec4 arrays. */
static void bind_arb_parameters(struct glm_context *ctx, struct glm_arb_program *p, int index)
{
    struct glm_backend_context *b = ctx->backend;
    NSUInteger bytes = (NSUInteger)(p->env_count + p->local_count) * 16, offset;
    id<MTLBuffer> buffer = transient_alloc(b, bytes, &offset);
    uint8_t *dst = (uint8_t *)mtl_contents(buffer) + offset;
    memcpy(dst, ctx->arb_env[index], (size_t)p->env_count * 16);
    memcpy(dst + p->env_count * 16, p->local, (size_t)p->local_count * 16);
    set_stages(b, buffer, offset, GLM_SLOT_ARB, index == 0 ? STAGE_VERTEX : STAGE_FRAGMENT);
}

/* Per draw: uniform data that changes without a state-serial bump. */
static void bind_program_uniforms(struct glm_context *ctx, struct glm_program *p, unsigned stages)
{
    struct glm_backend_context *b = ctx->backend;
    if (p->result.global_size > 0) {
        /* Uploaded again only when glUniform* changed it, or for a new
           command buffer (transient memory lives one command buffer). */
        uint64_t generation = b->submitted_serial;
        if (p->globals_dirty || !p->globals_buffer || p->globals_generation != generation ||
            p->globals_backend != b) {
            NSUInteger offset;
            __unsafe_unretained id<MTLBuffer> globals = transient_alloc(b, (NSUInteger)p->result.global_size, &offset);
            memcpy((uint8_t *)mtl_contents(globals) + offset, p->globals, (size_t)p->result.global_size);
            p->globals_buffer = (__bridge void *)globals; /* the transient list keeps it alive */
            p->globals_offset = offset;
            p->globals_generation = generation;
            p->globals_backend = b;
            p->globals_dirty = false;
        }
        /* Only to the stages reading the block (unknown for older compiles: both). */
        unsigned used = p->result.globals_stages ? p->result.globals_stages & stages : stages;
        if (used) set_stages(b, (__bridge id<MTLBuffer>)p->globals_buffer, p->globals_offset, GLM_SLOT_GLOBALS, used);
    }
    if (p->result.uses_legacy) {
        /* Only the parts the program reads are built and compared. */
        uint32_t parts = p->result.legacy_parts ? p->result.legacy_parts : GLM_LEGACY_ALL;
        glm_legacy_block_build(ctx, parts, &b->legacy_next);
        if (!b->legacy_valid || !glm_legacy_parts_equal(&b->legacy_next, &b->legacy_uploaded, parts)) {
            b->legacy_buffer = transient_alloc(b, sizeof b->legacy_next, &b->legacy_offset);
            memcpy((uint8_t *)mtl_contents(b->legacy_buffer) + b->legacy_offset, &b->legacy_next, sizeof b->legacy_next);
            b->legacy_uploaded = b->legacy_next;
            b->legacy_valid = true;
        }
        unsigned used = p->result.legacy_stages ? p->result.legacy_stages & stages : stages;
        if (used) set_stages(b, b->legacy_buffer, b->legacy_offset, GLM_SLOT_LEGACY, used);
    }
    for (int i = 0; i < p->result.block_count; ++i) {
        GLuint binding = p->block_bindings[i];
        if (binding >= 80) continue;
        const struct glm_indexed_buffer *ib = &ctx->uniform_bindings[binding];
        struct glm_buffer *buffer = ib->buffer ? glm_buffer_get(ctx, ib->buffer) : NULL;
        if (!buffer || !buffer->backend) continue;
        set_stages(b, (__bridge id<MTLBuffer>)buffer->backend, (NSUInteger)ib->offset, (NSUInteger)p->result.blocks[i].slot,
                   stages);
    }
}

/* When draw state changed: point size and textures. */
static void bind_compute_uniform_blocks(struct glm_context *ctx, id<MTLComputeCommandEncoder> encoder,
                                        const struct glm_compile_result *kernel, const struct glm_program *source)
{
    for (int i = 0; i < kernel->block_count; ++i) {
        const struct glm_block_info *block = &kernel->blocks[i];
        for (int j = 0; j < source->result.block_count; ++j) {
            if (strcmp(block->name, source->result.blocks[j].name)) continue;
            GLuint binding = source->block_bindings[j];
            if (binding >= 80) break;
            const struct glm_indexed_buffer *ib = &ctx->uniform_bindings[binding];
            struct glm_buffer *buffer = ib->buffer ? glm_buffer_get(ctx, ib->buffer) : NULL;
            if (buffer && buffer->backend)
                [encoder setBuffer:(__bridge id<MTLBuffer>)buffer->backend offset:(NSUInteger)ib->offset atIndex:(NSUInteger)block->slot];
            break;
        }
    }
}

static void vertex_id_mapping(struct glm_backend_context *b, uint32_t base, uint32_t offset, bool mapped)
{
    if (b->vertex_id_base != base || b->vertex_id_offset != offset || b->vertex_id_mapped != mapped)
        b->fast.valid = false;
    b->vertex_id_base = base;
    b->vertex_id_offset = offset;
    b->vertex_id_mapped = mapped;
}

static void vertex_id_base(struct glm_backend_context *b, uint32_t base)
{
    vertex_id_mapping(b, base, 0, false);
}

static bool bind_program_state(struct glm_context *ctx, struct glm_program *p, unsigned stages)
{
    struct glm_backend_context *b = ctx->backend;
    struct { float size, program_size, upper; uint32_t vertex_id_base, offset, mapped, pad[2]; } point = {
        ctx->state.point_size, ctx->state.program_point_size ? 1.0f : 0.0f,
        ctx->point_sprite_origin == GL_UPPER_LEFT ? 1.0f : 0.0f, b->vertex_id_base,
        b->vertex_id_offset, b->vertex_id_mapped, {0, 0}};
    enc_bytes(b, stages, &point, sizeof point, GLM_SLOT_POINT);
    uint32_t border_mask = (stages & STAGE_FRAGMENT) ? b->border_mask : 0;
    if (border_mask) enc_bytes(b, STAGE_FRAGMENT, b->border_data, sizeof b->border_data, GLM_SLOT_BORDER);
    float lod_bias[GLM_LOD_ROW_COUNT][4] = {{0}};
    bool ok = glm_bind_program_textures(ctx, b->encoder, p, stages, border_mask, lod_bias);
    if (p->result.sampler_count) enc_bytes(b, stages, lod_bias, sizeof lod_bias, GLM_SLOT_LOD_BIAS);
    return ok;
}

static int color_attachment_count(struct glm_backend_context *b)
{
    int n = 0;
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i)
        if (b->color[i]) n = i + 1;
    return n;
}


static bool select_stages(struct glm_context *ctx, struct draw_stages *st)
{
    struct glm_backend_context *b = ctx->backend;
    if (b->fast.valid && b->fast.serial == __atomic_load_n(&ctx->state_serial, __ATOMIC_RELAXED)) {
        *st = b->fast.st;
        return !st->core || st->vertex_program;
    }
    memset(st, 0, sizeof *st);
    st->program = glm_current_program(ctx);
    if (st->program) {
        if (st->program->functions[GLM_STAGE_VERTEX]) st->vertex_program = st->program;
        if (st->program->functions[GLM_STAGE_FRAGMENT]) st->fragment_program = st->program;
    } else {
        if ((st->arb_vertex = glm_arb_current(ctx, 0))) st->vertex_program = &st->arb_vertex->linked;
        if ((st->arb_fragment = glm_arb_current(ctx, 1))) st->fragment_program = &st->arb_fragment->linked;
    }
    st->core = ctx->profile == GLM_PROFILE_CORE;
    if (st->core && !st->vertex_program) return false;
    st->ff_vertex = !st->vertex_program;
    st->ff_fragment = !st->fragment_program && !st->core;
    return true;
}

enum { GLM_SLOT_ALPHA_REF = 23 }; /* alpha test reference of the alpha test variants */
static id<MTLFunction> program_variant(struct glm_program *p, uint32_t uint_inputs,
                                       uint32_t int_inputs, uint32_t border_mask, uint32_t alpha_func);

/* Everything before the draw call: encoder, pipeline for `layout` (nil:
   the stream layout of the CPU path, or the program's), fixed-function
   state and resources. Returns the encoder, or nil to skip the draw. */
static void *prepare_draw_once(struct glm_context *ctx, const struct draw_stages *st, uint32_t metal_primitive,
                               MTLVertexDescriptor *layout, uint32_t layout_id, bool capture);

static void *prepare_draw_raw(struct glm_context *ctx, const struct draw_stages *st,
                                                uint32_t metal_primitive, uint32_t primitive_count,
                                                uint32_t instances, MTLVertexDescriptor *layout, uint32_t layout_id,
                                                bool capture)
{
    glm_query_count_primitives(ctx, metal_primitive, primitive_count, instances);
    void *e = prepare_draw_once(ctx, st, metal_primitive, layout, layout_id, capture);
    /* Binding textures can rebuild a texture's storage, which ends the
       render pass (blits): prepare again on the new one. */
    if (e && e != (__bridge void *)ctx->backend->encoder) {
        ctx->backend->fast.valid = false;
        e = prepare_draw_once(ctx, st, metal_primitive, layout, layout_id, capture);
        if (e != (__bridge void *)ctx->backend->encoder) e = NULL;
    }
    return e;
}

static void *prepare_draw_once(struct glm_context *ctx, const struct draw_stages *st, uint32_t metal_primitive,
                               MTLVertexDescriptor *layout, uint32_t layout_id, bool capture)
{
    struct glm_backend_context *b = ctx->backend;
    const struct glm_state *s = &ctx->state;
    /* Transform feedback still runs the vertex stage with the rasterizer
       discarding everything. */
    bool discard = s->rasterizer_discard || capture;
    if (s->rasterizer_discard && !capture) return NULL;
    bool cull_all = s->cull_face && s->cull_mode == GL_FRONT_AND_BACK;
    if (cull_all && metal_primitive >= 3 && !capture) return NULL;
    if (!ensure_encoder(ctx) || (!discard && scissor_empty(b, s))) return NULL;
    /* May restart the pass, so before any encoder state is set. */
    if (ctx->active_queries[GLM_QUERY_OCCLUSION]) {
        glm_backend_occlusion_draw(ctx, ctx->active_queries[GLM_QUERY_OCCLUSION]);
        if (!b->encoder) return NULL;
    }
    /* The legacy alpha test after a GLSL or ARB fragment program: the
       program's alpha test variant (Source's HDR histogram counts
       alpha-tested fragments). */
    if (b->alpha_override) {
        if (b->fragment_override == b->alpha_override) b->fragment_override = nil;
        b->alpha_override = nil;
    }
    uint32_t fragment_variant = 0;
    if (!st->core && st->fragment_program && !b->fragment_override) {
        if (s->alpha_test && s->alpha_func != GL_ALWAYS) fragment_variant = (uint32_t)(s->alpha_func - GL_NEVER + 1);
        if (s->shade_model == GL_FLAT && st->fragment_program->color_inputs) fragment_variant |= GLM_VARIANT_FLAT_COLORS;
    }
    if (fragment_variant) {
        id<MTLFunction> variant = program_variant(st->fragment_program, 0, 0, 0, fragment_variant);
        if (variant) {
            b->fragment_override = b->alpha_override = variant;
            b->alpha_ref = (fragment_variant & 0xff) ? s->alpha_ref : 0;
        }
    }
    __unsafe_unretained id<MTLRenderCommandEncoder> e = b->encoder;
    uint64_t serial = __atomic_load_n(&ctx->state_serial, __ATOMIC_RELAXED);
    /* Nothing but uniforms, current attributes or matrices changed since
       the last draw on this encoder: only uniform data is re-bound. */
    bool fast = !capture && b->fast.valid && b->fast.serial == serial && b->fast.encoder_serial == b->encoder_serial &&
                b->fast.layout_id == layout_id && b->fast.vertex_override == b->vertex_override &&
                b->fast.fragment_override == b->fragment_override &&
                b->fast.st.vertex_program == st->vertex_program &&
                b->fast.st.fragment_program == st->fragment_program && b->fast.st.ff_vertex == st->ff_vertex &&
                b->fast.st.ff_fragment == st->ff_fragment;
    if (!fast) {
        struct glm_ff_key key;
        id<MTLFunction> vertex = nil, fragment = nil;
        if (st->ff_vertex || st->ff_fragment) {
            glm_ff_key_build(ctx, color_attachment_count(b), st->ff_vertex, st->ff_fragment, &key);
            ff_functions(b, &key, &vertex, &fragment);
            b->fast.key = key;
        }
        if (st->vertex_program) {
            vertex = (__bridge id<MTLFunction>)(capture ? st->vertex_program->functions_capture
                                                        : st->vertex_program->functions[GLM_STAGE_VERTEX]);
            if (!capture && b->vertex_override) vertex = b->vertex_override;
            if (!layout) layout = (__bridge MTLVertexDescriptor *)st->vertex_program->vertex_descriptor;
        }
        if (st->fragment_program)
            fragment = b->fragment_override ? b->fragment_override
                                            : (__bridge id<MTLFunction>)st->fragment_program->functions[GLM_STAGE_FRAGMENT];
        else if (st->core)
            fragment = empty_fragment(b);
        if (vertex && !capture) {
            uint32_t clip_mask = 0;
            for (int i = 0; i < GLM_MAX_CLIP_PLANES; ++i)
                if (s->clip_plane_enabled[i]) clip_mask |= 1u << i;
            vertex = clip_mask_function(b, vertex, clip_mask);
        }
        b->depth_clamp_active = false;
        if (fragment && b->samples > 1 && s->sample_shading && s->min_sample_shading > 0 && !capture)
            fragment = sample_shading_function(b, fragment);
        if (fragment && s->depth_clamp && !capture)
            fragment = depth_clamp_function(b, fragment, &b->depth_clamp_active);
        if (!vertex || !fragment) return NULL;
        id<MTLRenderPipelineState> pipeline = pipeline_for(b, vertex, fragment, s, false, 0, layout, layout_id, capture);
        if (!pipeline) return NULL;
        enc_pipeline(b, pipeline);
        enc_depth(b, depth_stencil_for(b, s, false, 0));
        MTLCullMode cull = MTLCullModeNone;
        if (s->cull_face) cull = s->cull_mode == GL_FRONT ? MTLCullModeFront : s->cull_mode == GL_BACK ? MTLCullModeBack : MTLCullModeNone;
        float offset_units = 0, offset_factor = 0;
        /* Polygon offset applies to polygons only: triangles, or the edges
           and vertices vertex.c draws for line and point polygon modes. */
        bool polygon = metal_primitive >= 3 || ctx->polygon_split;
        bool offset = polygon && ((s->polygon_mode[0] == GL_FILL && s->polygon_offset_fill) ||
                                  (s->polygon_mode[0] == GL_LINE && s->polygon_offset_line) ||
                                  (s->polygon_mode[0] == GL_POINT && s->polygon_offset_point));
        if (offset) {
            offset_factor = s->polygon_offset_factor;
            /* GL's unit is the smallest resolvable depth difference; for a
               32-bit float depth buffer Apple uses 2^-24 like a 24-bit one. */
            offset_units = s->polygon_offset_units;
        }
        /* GL CCW front faces appear clockwise after the Y flip. */
        enc_raster(b, s->front_face == GL_CCW ? MTLWindingClockwise : MTLWindingCounterClockwise, cull,
                   s->polygon_mode[0] == GL_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill, offset_units,
                   offset_factor, s->depth_clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip,
                   (uint32_t)s->stencil[0].ref & 0xff, (uint32_t)glm_stencil_back(s)->ref & 0xff, s->blend_color);
        apply_viewport_scissor(b, s, false);
        if (st->vertex_program == st->fragment_program && st->program) {
            if (!bind_program_state(ctx, st->program, STAGE_BOTH)) return NULL;
        } else {
            if (st->vertex_program && !bind_program_state(ctx, st->vertex_program, STAGE_VERTEX)) return NULL;
            if (st->fragment_program && !bind_program_state(ctx, st->fragment_program, STAGE_FRAGMENT)) return NULL;
        }
        if (st->ff_fragment) {
            float cube_lod[GLM_LOD_ROW_COUNT][4] = {{0}};
            if (!glm_bind_textures(ctx, e, &key, b->border_data, cube_lod)) return NULL;
            bool cube = false;
            for (int u = 0; u < GLM_FF_UNITS; ++u) cube |= key.unit[u].target == 4;
            if (cube) enc_bytes(b, STAGE_FRAGMENT, cube_lod, sizeof cube_lod, GLM_SLOT_FF_CUBE_LOD);
            bool border = false;
            for (int u = 0; u < GLM_FF_UNITS; ++u) border |= key.unit[u].target && key.unit[u].border;
            if (border) enc_bytes(b, STAGE_FRAGMENT, b->border_data, sizeof b->border_data, GLM_SLOT_BORDER);
        }
        b->fast.valid = !capture;
        b->fast.serial = serial;
        b->fast.encoder_serial = b->encoder_serial;
        b->fast.layout_id = layout_id;
        b->fast.vertex_override = b->vertex_override;
        b->fast.fragment_override = b->fragment_override;
        b->fast.st = *st;
    }

    if (st->vertex_program || st->fragment_program) {
        if (st->vertex_program == st->fragment_program) {
            bind_program_uniforms(ctx, st->program, STAGE_BOTH);
        } else {
            if (st->vertex_program) bind_program_uniforms(ctx, st->vertex_program, STAGE_VERTEX);
            if (st->fragment_program) bind_program_uniforms(ctx, st->fragment_program, STAGE_FRAGMENT);
        }
        if (st->arb_vertex) bind_arb_parameters(ctx, st->arb_vertex, 0);
        if (st->arb_fragment) bind_arb_parameters(ctx, st->arb_fragment, 1);
        if (b->alpha_override) enc_bytes(b, STAGE_FRAGMENT, &b->alpha_ref, sizeof b->alpha_ref, GLM_SLOT_ALPHA_REF);
    }
    if (b->depth_clamp_active) {
        float bounds[GLM_MAX_VIEWPORTS][2];
        for (int i = 0; i < GLM_MAX_VIEWPORTS; ++i) {
            bounds[i][0] = fmin(s->depth_ranges[i][0], s->depth_ranges[i][1]);
            bounds[i][1] = fmax(s->depth_ranges[i][0], s->depth_ranges[i][1]);
        }
        enc_bytes(b, STAGE_FRAGMENT, bounds, sizeof bounds, 24);
    }
    if (st->ff_vertex || st->ff_fragment) {
        float packed[sizeof(struct glm_ff_uniforms) / sizeof(float)];
        size_t size = glm_ff_uniforms_pack(ctx, &b->fast.key, packed);
        if (!b->ff_valid || size != b->ff_size || memcmp(packed, b->ff_uploaded, size)) {
            b->ff_buffer = transient_alloc(b, size, &b->ff_offset);
            memcpy((uint8_t *)mtl_contents(b->ff_buffer) + b->ff_offset, packed, size);
            memcpy(b->ff_uploaded, packed, size);
            b->ff_size = size;
            b->ff_valid = true;
        }
        if (st->ff_vertex) enc_buffer(b, STAGE_VERTEX, b->ff_buffer, b->ff_offset, GLM_SLOT_FF);
        if (st->ff_fragment) enc_buffer(b, STAGE_FRAGMENT, b->ff_buffer, b->ff_offset, GLM_SLOT_FF);
    }
    return (__bridge void *)e;
}

static void note_gpu_read(struct glm_context *ctx, struct glm_buffer *buffer, GLintptr low, GLintptr high);

static void draw_once(struct glm_context *ctx, const struct glm_draw *draw);
GLM_HIDDEN bool glm_bind_compute_textures(struct glm_context *ctx, id<MTLComputeCommandEncoder> encoder,
                                          const struct glm_compile_result *kernel, const struct glm_program *source);

/* Bytes a reflected uniform occupies in its block. */
static size_t uniform_bytes(const struct glm_uniform_info *u)
{
    int columns = 0, rows = 0;
    switch (u->type) {
    case GL_FLOAT_MAT2: columns = rows = 2; break;
    case GL_FLOAT_MAT3: columns = rows = 3; break;
    case GL_FLOAT_MAT4: columns = rows = 4; break;
    case GL_FLOAT_MAT2x3: columns = 2; rows = 3; break;
    case GL_FLOAT_MAT2x4: columns = 2; rows = 4; break;
    case GL_FLOAT_MAT3x2: columns = 3; rows = 2; break;
    case GL_FLOAT_MAT3x4: columns = 3; rows = 4; break;
    case GL_FLOAT_MAT4x2: columns = 4; rows = 2; break;
    case GL_FLOAT_MAT4x3: columns = 4; rows = 3; break;
    case GL_DOUBLE_MAT2: columns = rows = 2; break;
    case GL_DOUBLE_MAT3: columns = rows = 3; break;
    case GL_DOUBLE_MAT4: columns = rows = 4; break;
    case GL_DOUBLE_MAT2x3: columns = 2; rows = 3; break;
    case GL_DOUBLE_MAT2x4: columns = 2; rows = 4; break;
    case GL_DOUBLE_MAT3x2: columns = 3; rows = 2; break;
    case GL_DOUBLE_MAT3x4: columns = 3; rows = 4; break;
    case GL_DOUBLE_MAT4x2: columns = 4; rows = 2; break;
    case GL_DOUBLE_MAT4x3: columns = 4; rows = 3; break;
    }
    size_t element = columns ? (size_t)columns * (size_t)(u->matrix_stride ? u->matrix_stride : 16) : 16;
    switch (u->type) {
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: case GL_BOOL: element = 4; break;
    case GL_FLOAT_VEC2: case GL_INT_VEC2: case GL_UNSIGNED_INT_VEC2: case GL_BOOL_VEC2: case GL_DOUBLE: element = 8; break;
    case GL_FLOAT_VEC3: case GL_INT_VEC3: case GL_UNSIGNED_INT_VEC3: case GL_BOOL_VEC3: element = 12; break;
    case GL_DOUBLE_VEC3: element = 24; break;
    case GL_DOUBLE_VEC4: element = 32; break;
    }
    if (u->array_size > 1 && u->array_stride) return (size_t)u->array_size * (size_t)u->array_stride;
    return element;
}

static void remap_uniform_block_bindings(const struct glm_program *source, struct glm_program *target)
{
    for (int i = 0; i < target->result.block_count; ++i)
        for (int j = 0; j < source->result.block_count; ++j)
            if (!strcmp(target->result.blocks[i].name, source->result.blocks[j].name)) {
                target->block_bindings[i] = source->block_bindings[j];
                break;
            }
}

/* Copies uniform values by name from `source` into another compile's block,
   and sampler units likewise. */
static void remap_uniforms(const struct glm_program *source, const struct glm_compile_result *target, uint8_t *globals,
                           int *sampler_units)
{
    for (int i = 0; i < target->uniform_count; ++i) {
        const struct glm_uniform_info *t = &target->uniforms[i];
        for (int k = 0; k < source->result.uniform_count; ++k) {
            const struct glm_uniform_info *u = &source->result.uniforms[k];
            if (strcmp(u->name, t->name)) continue;
            if (t->sampler_slot >= 0 && u->sampler_slot >= 0) {
                if (sampler_units)
                    for (int e = 0; e < t->array_size && t->sampler_slot + e < 64 && u->sampler_slot + e < 64; ++e)
                        sampler_units[t->sampler_slot + e] = source->sampler_units[u->sampler_slot + e];
            } else if (t->offset >= 0 && u->offset >= 0) {
                size_t bytes = uniform_bytes(t), from = uniform_bytes(u);
                if (t->offset >= target->global_size || u->offset >= source->result.global_size) break;
                bytes = MIN(bytes, (size_t)(target->global_size - t->offset));
                from = MIN(from, (size_t)(source->result.global_size - u->offset));
                memcpy(globals + t->offset, source->globals + u->offset, MIN(bytes, from));
            }
            break;
        }
    }
}

/* Geometry stage emulation (shader_compiler.h): capture the vertex stage,
   run the geometry shader as a kernel, draw what it emitted. */
static void draw_geometry(struct glm_context *ctx, const struct glm_draw *draw, struct glm_program *p)
{
    struct glm_backend_context *b = ctx->backend;
    const struct glm_state *s = &ctx->state;
    const struct glm_gs_result *g = p->result.gs;
    struct glm_gs_state *gs = p->gs;
    uint32_t vertices = draw->index_count;
    uint32_t primitives = vertices / (uint32_t)g->in_vertices;
    if (!primitives) return;
    /* 1. Vertex stage outputs, per input vertex. */
    if (!ensure_encoder(ctx)) return;
    id<MTLFunction> capture = (__bridge id<MTLFunction>)gs->vs_capture;
    id<MTLRenderPipelineState> pipeline = pipeline_for(b, capture, empty_fragment(b), s, false, 0,
                                                       (__bridge MTLVertexDescriptor *)p->vertex_descriptor, 0, true);
    if (!pipeline) return;
    enc_pipeline(b, pipeline);
    b->fast.valid = false;
    bind_program_uniforms(ctx, p, STAGE_VERTEX);
    if (!bind_program_state(ctx, p, STAGE_VERTEX)) return;
    NSUInteger stream_offset, captured_offset;
    NSUInteger stream_bytes = (NSUInteger)draw->vertex_count * GLM_MAX_ATTRIBS * 16;
    id<MTLBuffer> stream = transient_alloc(b, stream_bytes, &stream_offset);
    memcpy((uint8_t *)mtl_contents(stream) + stream_offset, draw->vertices, stream_bytes);
    enc_buffer(b, STAGE_VERTEX, stream, stream_offset, GLM_SLOT_STREAM);
    id<MTLBuffer> captured = transient_alloc(b, (NSUInteger)vertices * (NSUInteger)g->vs_stride * 4, &captured_offset);
    enc_buffer(b, STAGE_VERTEX, captured, captured_offset, GLM_SLOT_XFB);
    struct { uint32_t first, pad[3]; uint32_t stride[4][4]; } info;
    memset(&info, 0, sizeof info);
    info.stride[0][0] = (uint32_t)g->vs_stride;
    enc_bytes(b, STAGE_VERTEX, &info, sizeof info, GLM_SLOT_XFB + 4);
    [b->encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:vertices];
    end_encoder(b);
    b->framebuffer_dirty = true;
    /* 2. The geometry shader, one thread per primitive and invocation. */
    uint32_t threads = primitives * (uint32_t)g->invocations;
    bool account = draw->capture || ctx->active_queries[GLM_QUERY_PRIMITIVES];
    for (int stream = 1; stream < 4; ++stream)
        account |= ctx->indexed_queries[GLM_QUERY_PRIMITIVES][stream - 1] != NULL;
    NSUInteger vertices_offset = 0, indices_offset = 0;
    NSUInteger vertex_bytes = ((NSUInteger)threads * (NSUInteger)g->max_vertices + 1) * (NSUInteger)g->out_stride * 4;
    id<MTLBuffer> out_vertices = account ? glm_new_shared_buffer(vertex_bytes, NULL)
                                       : transient_alloc(b, vertex_bytes, &vertices_offset);
    NSUInteger index_count = (NSUInteger)threads * (NSUInteger)g->max_indices;
    id<MTLBuffer> out_indices = account ? glm_new_shared_buffer(index_count * 4, NULL)
                                      : transient_alloc(b, index_count * 4, &indices_offset);
    id<MTLComputeCommandEncoder> compute = [command_buffer(b) computeCommandEncoder];
    id<MTLComputePipelineState> kernel = (__bridge id<MTLComputePipelineState>)gs->kernel;
    [compute setComputePipelineState:kernel];
    [compute setBuffer:captured offset:captured_offset atIndex:GLM_SLOT_GS_INPUT];
    [compute setBuffer:out_vertices offset:vertices_offset atIndex:GLM_SLOT_GS_VERTICES];
    [compute setBuffer:out_indices offset:indices_offset atIndex:GLM_SLOT_GS_INDICES];
    uint32_t gs_info[4] = {primitives, (uint32_t)g->vs_stride, 0, 0};
    [compute setBytes:gs_info length:sizeof gs_info atIndex:GLM_SLOT_GS_INFO];
    if (g->kernel->global_size > 0) {
        int units[64] = {0};
        remap_uniforms(p, g->kernel, gs->kernel_globals, units);
        [compute setBytes:gs->kernel_globals length:(NSUInteger)g->kernel->global_size atIndex:GLM_SLOT_GLOBALS];
    }
    bind_compute_uniform_blocks(ctx, compute, g->kernel, p);
    if (!glm_bind_compute_textures(ctx, compute, g->kernel, p)) {
        [compute endEncoding];
        return;
    }
    NSUInteger width = MIN((NSUInteger)64, kernel.maxTotalThreadsPerThreadgroup);
    [compute dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
    [compute endEncoding];
    /* Read back only when CPU query accounting or feedback compaction needs
       the actual emission count. Fixed output slots also contain unused vertices. */
    if (account) {
        glm_backend_flush(ctx, true);
        const float *values = (const float *)((const uint8_t *)mtl_contents(out_vertices) + vertices_offset);
        const uint32_t *indices = (const uint32_t *)((const uint8_t *)mtl_contents(out_indices) + indices_offset);
        uint32_t per = g->out_primitive == 3 ? 3 : g->out_primitive == 1 ? 2 : 1;
        uint32_t counts[4] = {0};
        uint32_t *slots[4] = {0};
        for (int stream_id = 0; stream_id < 4; ++stream_id)
            slots[stream_id] = malloc((size_t)(index_count + threads * g->max_vertices) * sizeof(uint32_t));
        if (per == 1) {
            for (uint32_t slot = 1; slot <= threads * (uint32_t)g->max_vertices; ++slot) {
                int stream_id = (int)values[(size_t)slot * g->out_stride + g->out_stride - 1];
                if (stream_id >= 0 && stream_id < 4) slots[stream_id][counts[stream_id]++] = slot;
            }
        } else {
            for (NSUInteger i = 0; i < index_count; i += per) {
                if (!indices[i]) continue;
                /* Undo the Metal provoking-vertex rotation for feedback order. */
                for (uint32_t v = 0; v < per; ++v) {
                    uint32_t source = per == 3 && g->flat_outputs ? (v + 1) % 3 : v;
                    slots[0][counts[0]++] = indices[i + source];
                }
            }
        }
        glm_query_count_primitives(ctx, (uint32_t)g->out_primitive, counts[0], 1);
        for (int stream = 1; stream < 4; ++stream) {
            struct glm_query *q = ctx->indexed_queries[GLM_QUERY_PRIMITIVES][stream - 1];
            if (q) q->counter += counts[stream] / per;
        }
        if (draw->capture) {
            uint32_t first[4], kept[4];
            for (int stream_id = 0; stream_id < 4; ++stream_id) {
                first[stream_id] = ctx->xfb.stream_vertices[stream_id];
                uint32_t capacity = UINT32_MAX;
                for (int f = 0; f < p->result.xfb_count && f < 64; ++f) {
                    const struct glm_xfb_varying *varying = &p->result.xfb[f];
                    if (!varying->type || g->feedback_streams[f] != stream_id) continue;
                    const struct glm_indexed_buffer *binding = &ctx->feedback_bindings[varying->buffer];
                    struct glm_buffer *buffer = glm_buffer_get(ctx, binding->buffer);
                    GLsizeiptr bytes = binding->size >= 0 ? binding->size : buffer->size - binding->offset;
                    uint32_t stride = (uint32_t)p->result.xfb_stride[varying->buffer] * sizeof(float);
                    uint32_t fit = stride && bytes > 0 ? (uint32_t)((uint64_t)bytes / stride) : 0;
                    capacity = MIN(capacity, fit);
                }
                uint32_t room = capacity > first[stream_id] ? capacity - first[stream_id] : 0;
                kept[stream_id] = MIN(counts[stream_id], room);
                kept[stream_id] -= kept[stream_id] % per;
                ctx->xfb.stream_vertices[stream_id] += kept[stream_id];
            }
            ctx->xfb.vertices = ctx->xfb.stream_vertices[0];
            struct glm_query *feedback_query = ctx->active_queries[GLM_QUERY_FEEDBACK];
            if (feedback_query) feedback_query->counter += kept[0] / per;
            for (int stream = 1; stream < 4; ++stream) {
                struct glm_query *q = ctx->indexed_queries[GLM_QUERY_FEEDBACK][stream - 1];
                if (q) q->counter += kept[stream] / per;
            }
            for (int f = 0; f < p->result.xfb_count && f < 64; ++f) {
                const struct glm_xfb_varying *varying = &p->result.xfb[f];
                int source_offset = g->feedback_offsets[f], stream_id = g->feedback_streams[f];
                if (!varying->type || source_offset < 0 || stream_id < 0 || stream_id >= 4) continue;
                const struct glm_indexed_buffer *binding = &ctx->feedback_bindings[varying->buffer];
                struct glm_buffer *buffer = glm_buffer_get(ctx, binding->buffer);
                if (!buffer) continue;
                glm_backend_buffer_prepare_write(ctx, buffer, binding->offset, buffer->size - binding->offset, false, false);
                float *destination = (float *)((uint8_t *)glm_backend_buffer_contents(ctx, buffer, true) + binding->offset);
                for (uint32_t v = 0; v < kept[stream_id]; ++v)
                    memcpy(destination + (size_t)(first[stream_id] + v) * p->result.xfb_stride[varying->buffer] + varying->offset,
                           values + (size_t)slots[stream_id][v] * g->out_stride + source_offset,
                           (size_t)varying->components * sizeof(float));
            }
        }
        for (int stream_id = 0; stream_id < 4; ++stream_id) free(slots[stream_id]);
    }
    if (s->rasterizer_discard) return;
    /* 3. The emitted primitives, with the program's fragment stage. */
    struct glm_program *pull = gs->pull;
    remap_uniform_block_bindings(p, pull);
    remap_uniforms(p, &pull->result, pull->globals, pull->sampler_units);
    pull->globals_dirty = true;
    struct draw_stages st = {pull, pull, pull, NULL, NULL, false, false, ctx->profile == GLM_PROFILE_CORE};
    b->viewport_array = g->writes_viewport;
    b->topology = g->out_primitive == 3 ? MTLPrimitiveTopologyClassTriangle
                  : g->out_primitive == 1 ? MTLPrimitiveTopologyClassLine : MTLPrimitiveTopologyClassPoint;
    b->fast.valid = false;
    id<MTLRenderCommandEncoder> e = prepare_draw(ctx, &st, (uint32_t)g->out_primitive, account ? 0 : (uint32_t)index_count, 1, nil, 0,
                                                 false);
    b->fast.valid = false;
    b->viewport_array = false;
    b->topology = 0;
    if (!e) return;
    enc_buffer(b, STAGE_VERTEX, out_vertices, vertices_offset, GLM_SLOT_GS_VERTICES);
    [e drawIndexedPrimitives:(MTLPrimitiveType)g->out_primitive indexCount:index_count indexType:MTLIndexTypeUInt32
                 indexBuffer:out_indices indexBufferOffset:indices_offset];
}

/* Runs `capture` (a vertex stage writing its outputs) over the draw's
   de-indexed vertices; returns the buffer of `stride`-float vertices. */
static id<MTLBuffer> capture_vertices(struct glm_context *ctx, const struct glm_draw *draw, struct glm_program *p,
                                      id<MTLFunction> capture, int stride, NSUInteger *offset)
{
    struct glm_backend_context *b = ctx->backend;
    if (!ensure_encoder(ctx)) return nil;
    id<MTLRenderPipelineState> pipeline = pipeline_for(b, capture, empty_fragment(b), &ctx->state, false, 0,
                                                       (__bridge MTLVertexDescriptor *)p->vertex_descriptor, 0, true);
    if (!pipeline) return nil;
    enc_pipeline(b, pipeline);
    b->fast.valid = false;
    bind_program_uniforms(ctx, p, STAGE_VERTEX);
    if (!bind_program_state(ctx, p, STAGE_VERTEX)) return nil;
    NSUInteger stream_offset;
    NSUInteger stream_bytes = (NSUInteger)draw->vertex_count * GLM_MAX_ATTRIBS * 16;
    id<MTLBuffer> stream = transient_alloc(b, stream_bytes, &stream_offset);
    memcpy((uint8_t *)mtl_contents(stream) + stream_offset, draw->vertices, stream_bytes);
    enc_buffer(b, STAGE_VERTEX, stream, stream_offset, GLM_SLOT_STREAM);
    id<MTLBuffer> captured = transient_alloc(b, (NSUInteger)draw->index_count * (NSUInteger)stride * 4, offset);
    enc_buffer(b, STAGE_VERTEX, captured, *offset, GLM_SLOT_XFB);
    struct { uint32_t first, pad[3]; uint32_t stride[4][4]; } info;
    memset(&info, 0, sizeof info);
    info.stride[0][0] = (uint32_t)stride;
    enc_bytes(b, STAGE_VERTEX, &info, sizeof info, GLM_SLOT_XFB + 4);
    [b->encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:draw->index_count];
    end_encoder(b);
    b->framebuffer_dirty = true;
    return captured;
}

/* Tessellation emulation (shader_compiler.h): capture the vertex stage,
   run the control shader as a kernel, tessellate with the evaluation
   shader as the post-tessellation vertex function. */
static void draw_tessellation(struct glm_context *ctx, const struct glm_draw *draw, struct glm_program *p)
{
    struct glm_backend_context *b = ctx->backend;
    const struct glm_tess_result *t = p->result.tess;
    struct glm_tess_state *tess = p->tess;
    uint32_t in_vertices = (uint32_t)ctx->state.patch_vertices;
    uint32_t patches = draw->index_count / in_vertices;
    if (!patches) return;
    if (t->point_mode) glm_log("tessellation point_mode is drawn as triangles");
    /* 1. Vertex stage outputs, per patch vertex. */
    NSUInteger captured_offset;
    id<MTLBuffer> captured = capture_vertices(ctx, draw, p, (__bridge id<MTLFunction>)tess->vs_capture, t->vs_stride,
                                              &captured_offset);
    if (!captured) return;
    /* 2. The control shader: a threadgroup per patch. */
    NSUInteger points_offset, patch_offset, factors_offset;
    id<MTLBuffer> points = transient_alloc(b, (NSUInteger)patches * (NSUInteger)t->out_vertices * (NSUInteger)t->cp_slots * 16,
                                           &points_offset);
    id<MTLBuffer> patch = transient_alloc(b, (NSUInteger)patches * (NSUInteger)t->patch_slots * 16, &patch_offset);
    id<MTLBuffer> factors = transient_alloc(b, (NSUInteger)patches * (t->quads ? 12 : 8), &factors_offset);
    id<MTLComputeCommandEncoder> compute = [command_buffer(b) computeCommandEncoder];
    id<MTLComputePipelineState> kernel = (__bridge id<MTLComputePipelineState>)tess->kernel;
    [compute setComputePipelineState:kernel];
    [compute setBuffer:captured offset:captured_offset atIndex:GLM_SLOT_GS_INPUT];
    [compute setBuffer:points offset:points_offset atIndex:GLM_SLOT_GS_VERTICES];
    [compute setBuffer:patch offset:patch_offset atIndex:GLM_SLOT_GS_INDICES];
    [compute setBuffer:factors offset:factors_offset atIndex:GLM_SLOT_TESS_FACTORS];
    uint32_t info[4] = {patches, (uint32_t)t->vs_stride, in_vertices, 0};
    [compute setBytes:info length:sizeof info atIndex:GLM_SLOT_GS_INFO];
    if (t->kernel->global_size > 0) {
        int units[64] = {0};
        remap_uniforms(p, t->kernel, tess->kernel_globals, units);
        [compute setBytes:tess->kernel_globals length:(NSUInteger)t->kernel->global_size atIndex:GLM_SLOT_GLOBALS];
    }
    bind_compute_uniform_blocks(ctx, compute, t->kernel, p);
    if (!glm_bind_compute_textures(ctx, compute, t->kernel, p)) {
        [compute endEncoding];
        return;
    }
    [compute dispatchThreadgroups:MTLSizeMake(patches, 1, 1) threadsPerThreadgroup:MTLSizeMake((NSUInteger)t->out_vertices, 1, 1)];
    [compute endEncoding];
    /* 3. Tessellation, the evaluation shader and the fragment stage. */
    struct glm_program *eval = tess->eval;
    remap_uniform_block_bindings(p, eval);
    remap_uniforms(p, &eval->result, eval->globals, eval->sampler_units);
    eval->globals_dirty = true;
    struct draw_stages st = {eval, eval, eval, NULL, NULL, false, false, ctx->profile == GLM_PROFILE_CORE};
    b->tess_mode = 1u | (uint32_t)t->partition << 1 | (t->cw ? 1u << 4 : 0);
    b->fast.valid = false;
    id<MTLRenderCommandEncoder> e = prepare_draw(ctx, &st, 3, patches, 1, nil, 0, false);
    b->tess_mode = 0;
    b->fast.valid = false;
    if (!e) return;
    enc_buffer(b, STAGE_VERTEX, points, points_offset, GLM_SLOT_TESS_POINTS);
    enc_buffer(b, STAGE_VERTEX, patch, patch_offset, GLM_SLOT_TESS_PATCH);
    [e setTessellationFactorBuffer:factors offset:factors_offset instanceStride:0];
    [e drawPatches:(NSUInteger)t->out_vertices patchStart:0 patchCount:patches patchIndexBuffer:nil patchIndexBufferOffset:0
        instanceCount:1 baseInstance:0];
}

bool glm_backend_clip_stream(struct glm_context *ctx, GLenum mode)
{
    if ((mode != GL_TRIANGLES && mode != GL_TRIANGLE_STRIP && mode != GL_TRIANGLE_FAN) ||
        !ctx->current_program || ctx->polygon_split || ctx->state.rasterizer_discard ||
        ctx->state.polygon_mode[0] != GL_FILL || ctx->state.polygon_mode[1] != GL_FILL) return false;
    struct glm_program *p = glm_current_program(ctx);
    if (!p || !p->clip || !p->result.clip || p->gs || p->tess) return false;
    if (p->result.clip->primitive_id && ctx->state.primitive_restart) return false;
    int count = p->result.clip->clip_count;
    for (int i = 0; i < count && i < GLM_MAX_CLIP_PLANES; ++i)
        if (ctx->state.clip_plane_enabled[i]) return true;
    return false;
}

/* Original attributes are uploaded in bounded batches. Capture and clipped
   geometry live in private GPU scratch reused only by ordered passes of this
   draw. There is no CPU readback or second transform-feedback write. */
static bool draw_user_clipped(struct glm_context *ctx, const struct glm_draw *draw, struct glm_program *p)
{
    if (draw->capture || draw->metal_primitive != MTLPrimitiveTypeTriangle ||
        !glm_backend_clip_stream(ctx, draw->mode) || !draw->vertices || draw->instance_count != 1) return false;
    const struct glm_clip_result *g = p->result.clip;
    enum { BATCH_TRIANGLES = 512, ATTRIBUTE_BYTES = GLM_MAX_ATTRIBS * 16 };
    if (g->vs_stride <= 0 || g->out_stride <= 0 || g->clip_count <= 0 || g->clip_count > GLM_MAX_CLIP_PLANES)
        return false;
    uint32_t triangles = draw->index_count / 3;
    if (!triangles) return false;
    uint32_t capacity = MIN(triangles, BATCH_TRIANGLES);
    uint64_t capture_bytes = (uint64_t)capacity * 3 * (uint32_t)g->vs_stride * 4;
    uint64_t output_bytes = ((uint64_t)capacity * GLM_CLIP_MAX_VERTICES + 1) * (uint32_t)g->out_stride * 4;
    uint64_t index_bytes = (uint64_t)capacity * GLM_CLIP_MAX_INDICES * 4;
    if (capture_bytes > device.maxBufferLength || output_bytes > device.maxBufferLength ||
        index_bytes > device.maxBufferLength || capture_bytes > SIZE_MAX || output_bytes > SIZE_MAX) return false;
    id<MTLBuffer> captured = [device newBufferWithLength:(NSUInteger)capture_bytes options:MTLResourceStorageModePrivate];
    id<MTLBuffer> vertices = [device newBufferWithLength:(NSUInteger)output_bytes options:MTLResourceStorageModePrivate];
    id<MTLBuffer> indices = [device newBufferWithLength:(NSUInteger)index_bytes options:MTLResourceStorageModePrivate];
    if (!captured || !vertices || !indices) return false;
    struct glm_backend_context *b = ctx->backend;
    uint32_t mask = 0;
    vertex_id_base(b, 0); /* capture and pull use deindexed internal IDs */
    for (int i = 0; i < g->clip_count; ++i)
        if (ctx->state.clip_plane_enabled[i]) mask |= 1u << i;
    struct glm_program *pull = p->clip->pull;
    remap_uniforms(p, &pull->result, pull->globals, pull->sampler_units);
    remap_uniform_block_bindings(p, pull);
    memcpy(pull->subroutine_values, p->subroutine_values, sizeof pull->subroutine_values);
    pull->globals_dirty = true;
    struct draw_stages st = {pull, pull, pull, NULL, NULL, false, false, ctx->profile == GLM_PROFILE_CORE};
    for (uint32_t first = 0; first < triangles;) {
        uint32_t count = MIN(capacity, triangles - first);
        uint32_t input_count = count * 3;
        if (!ensure_encoder(ctx)) return true;
        if (!bind_program_state(ctx, p, STAGE_VERTEX)) return true;
        if (!b->encoder) {
            if (!ensure_encoder(ctx) || !bind_program_state(ctx, p, STAGE_VERTEX) || !b->encoder) return true;
        }
        id<MTLRenderPipelineState> capture_pipeline = pipeline_for(b, (__bridge id<MTLFunction>)p->clip->vs_capture,
                    empty_fragment(b), &ctx->state, false, 0, (__bridge MTLVertexDescriptor *)p->vertex_descriptor, 0, true);
        if (!capture_pipeline) return true;
        enc_pipeline(b, capture_pipeline);
        b->fast.valid = false;
        bind_program_uniforms(ctx, p, STAGE_VERTEX);
        NSUInteger stream_offset;
        id<MTLBuffer> stream = transient_alloc(b, (NSUInteger)input_count * ATTRIBUTE_BYTES, &stream_offset);
        uint8_t *destination = (uint8_t *)mtl_contents(stream) + stream_offset;
        for (uint32_t i = 0; i < input_count; ++i) {
            uint32_t ordinal = first * 3 + i;
            uint32_t source = draw->indices ? draw->indices[ordinal] : ordinal;
            if (source >= draw->vertex_count) return true;
            memcpy(destination + (size_t)i * ATTRIBUTE_BYTES,
                   (const uint8_t *)draw->vertices + (size_t)source * ATTRIBUTE_BYTES, ATTRIBUTE_BYTES);
        }
        enc_buffer(b, STAGE_VERTEX, stream, stream_offset, GLM_SLOT_STREAM);
        enc_buffer(b, STAGE_VERTEX, captured, 0, GLM_SLOT_XFB);
        struct { uint32_t first, pad[3]; uint32_t stride[4][4]; } capture_info = {0};
        capture_info.stride[0][0] = (uint32_t)g->vs_stride;
        enc_bytes(b, STAGE_VERTEX, &capture_info, sizeof capture_info, GLM_SLOT_XFB + 4);
        [b->encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:input_count
                    instanceCount:1 baseInstance:draw->base_instance];
        end_encoder(b);
        b->framebuffer_dirty = true;
        id<MTLComputeCommandEncoder> compute = [command_buffer(b) computeCommandEncoder];
        id<MTLComputePipelineState> kernel = (__bridge id<MTLComputePipelineState>)p->clip->kernel;
        [compute setComputePipelineState:kernel];
        [compute setBuffer:captured offset:0 atIndex:GLM_SLOT_GS_INPUT];
        [compute setBuffer:vertices offset:0 atIndex:GLM_SLOT_GS_VERTICES];
        [compute setBuffer:indices offset:0 atIndex:GLM_SLOT_GS_INDICES];
        /* Canonical polygon order is independent of the flat reference at
           captured vertex zero. Fan triangulation starts at vertex one. */
        uint32_t even_shift = draw->mode == GL_TRIANGLE_FAN ? 2 : 0, odd_shift = even_shift;
        bool flat = p->result.flat_inputs || (p->color_inputs && ctx->state.shade_model == GL_FLAT &&
                                             ctx->profile == GLM_PROFILE_LEGACY);
        if (flat && ctx->state.provoking_vertex != GL_FIRST_VERTEX_CONVENTION) even_shift = odd_shift = 1;
        else if (flat && draw->mode == GL_TRIANGLE_STRIP) odd_shift = 2;
        uint32_t info[4] = {count, mask, first, even_shift | (odd_shift << 2)};
        [compute setBytes:info length:sizeof info atIndex:GLM_SLOT_GS_INFO];
        NSUInteger width = MIN((NSUInteger)64, kernel.maxTotalThreadsPerThreadgroup);
        [compute dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [compute endEncoding];
        b->fast.valid = false;
        id<MTLRenderCommandEncoder> encoder = prepare_draw(ctx, &st, MTLPrimitiveTypeTriangle,
                    glm_feedback_capturing(ctx) ? 0 : input_count, 1, nil, 0, false);
        b->fast.valid = false;
        if (!encoder) return true;
        enc_buffer(b, STAGE_VERTEX, vertices, 0, GLM_SLOT_GS_VERTICES);
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:(NSUInteger)count * GLM_CLIP_MAX_INDICES
                            indexType:MTLIndexTypeUInt32 indexBuffer:indices indexBufferOffset:0];
        first += count;
    }
    return true;
}

/* Transform feedback captures in a rasterization-free pass; the draw is
   rasterized separately unless the rasterizer discards everything. */
void glm_backend_draw(struct glm_context *ctx, const struct glm_draw *draw)
{
    vertex_id_base(ctx->backend, draw->vertex_id_base);
    ctx->backend->vertex_override = nil;
    ctx->backend->fragment_override = nil;
    ctx->backend->border_mask = 0;
    struct glm_program *program = glm_current_program(ctx);
    if (program && program->gs) {
        draw_geometry(ctx, draw, program);
        return;
    }
    if (program && program->tess) {
        draw_tessellation(ctx, draw, program);
        return;
    }
    if (!program || !draw_user_clipped(ctx, draw, program)) draw_once(ctx, draw);
    if (draw->capture) {
        /* Later draws may read what the vertex stage wrote: Metal orders
           that across passes, not within one. */
        end_encoder(ctx->backend);
        ctx->backend->framebuffer_dirty = true;
    }
    if (draw->capture && !ctx->state.rasterizer_discard) {
        struct glm_draw normal = *draw;
        normal.capture = false;
        if (draw->raster_indices) normal.indices = draw->raster_indices;
        if (!program || !draw_user_clipped(ctx, &normal, program)) draw_once(ctx, &normal);
    }
}

static void draw_once(struct glm_context *ctx, const struct glm_draw *draw)
{
    struct glm_backend_context *b = ctx->backend;
    uint64_t vertex_bytes64 = (uint64_t)draw->vertex_count * GLM_MAX_ATTRIBS * 16;
    uint64_t id_bytes = draw->original_vertex_ids ? (uint64_t)draw->vertex_count * sizeof(uint32_t) : 0;
    if (vertex_bytes64 + id_bytes > UINT32_MAX * UINT64_C(4) || vertex_bytes64 + id_bytes > SIZE_MAX ||
        vertex_bytes64 + id_bytes > device.maxBufferLength) return glm_error(ctx, GL_OUT_OF_MEMORY);
    vertex_id_mapping(b, draw->vertex_id_base, draw->original_vertex_ids ? (uint32_t)(vertex_bytes64 / 4) : 0,
                      draw->original_vertex_ids != NULL);
    struct draw_stages st;
    if (!select_stages(ctx, &st)) return;
    uint32_t primitive_count = !draw->capture && glm_feedback_capturing(ctx) ? 0 : draw->index_count;
    id<MTLRenderCommandEncoder> e = prepare_draw(ctx, &st, draw->metal_primitive, primitive_count, draw->instance_count,
                                                 nil, 0, draw->capture);
    if (!e) return;
    if (draw->capture) {
        /* Feedback buffers at their binding offsets, and where this draw's
           vertices go. Vertices past the capture's room are not written. */
        struct glm_program *p = st.vertex_program;
        struct { uint32_t first, pad[3]; uint32_t stride[4][4]; } info;
        memset(&info, 0, sizeof info);
        info.first = draw->xfb_first;
        uint64_t serial = glm_backend_pending_serial(ctx);
        for (int i = 0; i < p->result.xfb_buffers && i < 4; ++i) {
            const struct glm_indexed_buffer *binding = &ctx->feedback_bindings[i];
            struct glm_buffer *buffer = binding->buffer ? glm_buffer_get(ctx, binding->buffer) : NULL;
            if (!buffer || !buffer->backend) return;
            info.stride[i][0] = (uint32_t)p->result.xfb_stride[i];
            enc_buffer(b, STAGE_VERTEX, (__bridge id<MTLBuffer>)buffer->backend, (NSUInteger)binding->offset,
                       GLM_SLOT_XFB + (NSUInteger)i);
            buffer->gpu_write_serial = serial;
            buffer->content_generation = next_content_generation();
            buffer->gpu_writer = b;
            note_gpu_read(ctx, buffer, binding->offset, buffer->size);
        }
        enc_bytes(b, STAGE_VERTEX, &info, sizeof info, GLM_SLOT_XFB + 4);
        if (draw->xfb_count < draw->vertex_count) {
            /* Only whole primitives that fit are captured: draw those. */
            if (!draw->xfb_count) return;
        }
    }
    NSUInteger vertex_bytes = (NSUInteger)vertex_bytes64, vertex_offset;
    id<MTLBuffer> vertices = transient_alloc(b, vertex_bytes + (NSUInteger)id_bytes, &vertex_offset);
    if (!vertices) return glm_error(ctx, GL_OUT_OF_MEMORY);
    memcpy((uint8_t *)mtl_contents(vertices) + vertex_offset, draw->vertices, vertex_bytes);
    if (id_bytes) memcpy((uint8_t *)mtl_contents(vertices) + vertex_offset + vertex_bytes,
                         draw->original_vertex_ids, (size_t)id_bytes);
    enc_buffer(b, STAGE_VERTEX, vertices, vertex_offset, GLM_SLOT_STREAM);
    MTLPrimitiveType primitive = (MTLPrimitiveType)draw->metal_primitive;
    if (draw->indices) {
        NSUInteger index_bytes = (NSUInteger)draw->index_count * 4, index_offset;
        id<MTLBuffer> indices = transient_alloc(b, index_bytes, &index_offset);
        memcpy((uint8_t *)mtl_contents(indices) + index_offset, draw->indices, index_bytes);
        [e drawIndexedPrimitives:primitive indexCount:draw->index_count indexType:MTLIndexTypeUInt32
                     indexBuffer:indices indexBufferOffset:index_offset instanceCount:1 baseVertex:0
                    baseInstance:draw->base_instance];
    } else {
        NSUInteger count = draw->capture ? draw->xfb_count : draw->index_count;
        [e drawPrimitives:primitive vertexStart:0 vertexCount:count instanceCount:1 baseInstance:draw->base_instance];
    }
}

/* ---- GPU vertex fetch ---------------------------------------------------- */

GLM_HIDDEN void glm_backend_fence_wait(struct glm_context *ctx, uint64_t serial);

enum { GLM_SLOT_VERTEX0 = 17, GLM_VERTEX_SLOTS = 14 };

static size_t gl_type_size(GLenum type)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return 2;
    default: return 4;
    }
}

/* The Metal format that fetches an array as GL would for a float (or, when
   `integer`, integer) shader input; Invalid when Metal has none. */
static MTLVertexFormat vertex_format(const struct glm_array *array, bool integer_input)
{
    /* Unnormalized integer arrays feeding float inputs reach an integer
       variant of the vertex function (int_variant). */
    struct glm_array converted = *array;
    if (integer_input && !array->integer && !array->normalized && array->type != GL_FLOAT && array->type != GL_HALF_FLOAT)
        converted.integer = true;
    const struct glm_array *a = &converted;
    if (a->integer != integer_input) return MTLVertexFormatInvalid;
    if (a->size == GL_BGRA)
        return a->type == GL_UNSIGNED_BYTE && a->normalized ? MTLVertexFormatUChar4Normalized_BGRA : MTLVertexFormatInvalid;
    if (a->size < 1 || a->size > 4) return MTLVertexFormatInvalid;
    int n = a->size - 1;
#define PICK(x1, x2, x3, x4) do { static const MTLVertexFormat f[] = {x1, x2, x3, x4}; return f[n]; } while (0)
    switch (a->type) {
    case GL_FLOAT:
        if (a->integer) break;
        PICK(MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4);
    case GL_HALF_FLOAT:
        if (a->integer) break;
        PICK(MTLVertexFormatHalf, MTLVertexFormatHalf2, MTLVertexFormatHalf3, MTLVertexFormatHalf4);
    case GL_UNSIGNED_BYTE:
        if (a->integer) PICK(MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4);
        if (a->normalized)
            PICK(MTLVertexFormatUCharNormalized, MTLVertexFormatUChar2Normalized, MTLVertexFormatUChar3Normalized,
                 MTLVertexFormatUChar4Normalized);
        break;
    case GL_BYTE:
        if (a->integer) PICK(MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4);
        if (a->normalized)
            PICK(MTLVertexFormatCharNormalized, MTLVertexFormatChar2Normalized, MTLVertexFormatChar3Normalized,
                 MTLVertexFormatChar4Normalized);
        break;
    case GL_UNSIGNED_SHORT:
        if (a->integer) PICK(MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4);
        if (a->normalized)
            PICK(MTLVertexFormatUShortNormalized, MTLVertexFormatUShort2Normalized, MTLVertexFormatUShort3Normalized,
                 MTLVertexFormatUShort4Normalized);
        break;
    case GL_SHORT:
        if (a->integer) PICK(MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4);
        if (a->normalized)
            PICK(MTLVertexFormatShortNormalized, MTLVertexFormatShort2Normalized, MTLVertexFormatShort3Normalized,
                 MTLVertexFormatShort4Normalized);
        break;
    case GL_INT:
        if (a->integer) PICK(MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4);
        break;
    case GL_UNSIGNED_INT:
        if (a->integer) PICK(MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4);
        break;
    }
#undef PICK
    return MTLVertexFormatInvalid;
}

/* The integer-input variant as a patch of the program's own vertex MSL
   (no second GLSL compile): inputs at the `uint_inputs` / `int_inputs`
   locations become uint/int vectors, converted to float at the top of
   main0, as the GLSL rewrite (shader_compiler.cpp) would produce. NULL when
   the source is not in the expected shape. malloc'd. */
static char *int_variant_msl(const char *msl, uint32_t uint_inputs, uint32_t int_inputs)
{
    const char *in_struct = strstr(msl, "struct main0_in\n{\n");
    const char *entry = strstr(msl, "vertex main0_out main0(");
    if (!in_struct || !entry || entry < in_struct) return NULL;
    const char *struct_end = strstr(in_struct, "\n};\n");
    const char *body = strstr(entry, "\n{\n");
    if (!struct_end || !body) return NULL;
    body += 3;
    const char *out_line = strstr(body, "    main0_out out = {};\n");
    const char *body_start = out_line && out_line == body ? out_line + strlen("    main0_out out = {};\n") : body;
    /* The patched struct and the conversions. */
    size_t cap = strlen(msl) * 2 + 4096;
    char *result = malloc(cap), *conversions = malloc(4096);
    char names[32][64];
    int name_count = 0;
    size_t n = 0, c = 0;
    conversions[0] = 0;
    memcpy(result, msl, (size_t)(in_struct - msl));
    n = (size_t)(in_struct - msl);
    const char *line = in_struct;
    while (line < struct_end + 1) {
        const char *eol = strchr(line, '\n');
        if (!eol) break;
        char type[16], name[64];
        int width = 0, location = -1;
        char buffer[256];
        size_t length = (size_t)(eol - line);
        if (length < sizeof buffer) {
            memcpy(buffer, line, length);
            buffer[length] = 0;
        } else {
            buffer[0] = 0;
        }
        bool patched = false;
        if (sscanf(buffer, " %15[a-z0-9] %63[A-Za-z0-9_] [[attribute(%d)]];", type, name, &location) == 3 && location >= 0 &&
            location < 32 && (((uint_inputs | int_inputs) >> location) & 1) && !strncmp(type, "float", 5) &&
            name_count < 32) {
            width = type[5] ? type[5] - '0' : 1;
            const char *itype = (uint_inputs >> location) & 1 ? "uint" : "int";
            char suffix[2] = {width > 1 ? (char)('0' + width) : 0, 0};
            n += (size_t)snprintf(result + n, cap - n, "    %s%s glm_int_%s [[attribute(%d)]];\n", itype, suffix, name, location);
            c += (size_t)snprintf(conversions + c, 4096 - c, "    %s glm_in_%s = %s(in.glm_int_%s);\n", type, name, type, name);
            snprintf(names[name_count++], 64, "%s", name);
            patched = true;
        }
        if (!patched) {
            memcpy(result + n, line, length + 1);
            n += length + 1;
        }
        line = eol + 1;
    }
    if (!name_count) {
        free(result);
        free(conversions);
        return NULL;
    }
    /* Up to the body, then the conversions, then the body with in.<name>
       reads replaced by the converted locals. */
    size_t middle = (size_t)(body_start - line);
    memcpy(result + n, line, middle);
    n += middle;
    memcpy(result + n, conversions, c);
    n += c;
    free(conversions);
    for (const char *q = body_start; *q;) {
        bool replaced = false;
        if (q[0] == 'i' && q[1] == 'n' && q[2] == '.' && (q == msl || !(isalnum((unsigned char)q[-1]) || q[-1] == '_' || q[-1] == '.'))) {
            for (int k = 0; k < name_count; ++k) {
                size_t len = strlen(names[k]);
                if (!strncmp(q + 3, names[k], len) && !(isalnum((unsigned char)q[3 + len]) || q[3 + len] == '_')) {
                    n += (size_t)snprintf(result + n, cap - n, "glm_in_%s", names[k]);
                    q += 3 + len;
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) {
            if (n + 1 >= cap) {
                free(result);
                return NULL;
            }
            result[n++] = *q++;
        }
    }
    result[n] = 0;
    return result;
}

/* The fragment function with the legacy alpha test applied to colour 0
   (GL 2.1 applies it after a fragment shader as after fixed function):
   `func` is the GL function - GL_NEVER + 1; the reference arrives in
   buffer GLM_SLOT_ALPHA_REF. NULL if the MSL has no colour output. */
/* With GLM_VARIANT_FLAT_COLORS: the colour inputs flat (glShadeModel). */
static char *flat_colors_msl(const char *msl)
{
    size_t cap = strlen(msl) + 512, n = 0;
    char *result = malloc(cap);
    static const char *const names[] = {" glm_Color [[user(", " glm_SecondaryColor [[user(", " glm_BackColor_in [[user(",
                                        " glm_BackSecondaryColor_in [[user("};
    const char *in_struct = strstr(msl, "struct main0_in");
    const char *in_end = in_struct ? strstr(in_struct, "};") : NULL;
    for (const char *q = msl; *q;) {
        bool matched = false;
        for (int k = 0; k < 4 && in_struct && q > in_struct && q < in_end; ++k) {
            size_t len = strlen(names[k]);
            if (strncmp(q, names[k], len)) continue;
            const char *close = strstr(q + len, ")]]");
            if (!close) break;
            n += (size_t)snprintf(result + n, cap - n, "%.*s), flat]]", (int)(close - q), q);
            q = close + 3;
            matched = true;
            break;
        }
        if (matched) continue;
        if (n + 1 >= cap) { free(result); return NULL; }
        result[n++] = *q++;
    }
    result[n] = 0;
    return result;
}

static char *alpha_variant_msl(const char *source_msl, uint32_t variant)
{
    static const char *const tests[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    char *flat = variant & GLM_VARIANT_FLAT_COLORS ? flat_colors_msl(source_msl) : NULL;
    uint32_t func = variant & 0xff;
    if (!func) return flat;
    const char *msl = flat ? flat : source_msl;
    if (func > 8) { free(flat); return NULL; }
    const char *out_struct = strstr(msl, "struct main0_out");
    const char *color = out_struct ? strstr(out_struct, " [[color(0)]]") : NULL;
    if (!color) { free(flat); return NULL; }
    const char *name_end = color, *name = color;
    while (name > out_struct && (isalnum((unsigned char)name[-1]) || name[-1] == '_')) --name;
    if (name == name_end) { free(flat); return NULL; }
    char member[128];
    snprintf(member, sizeof member, "%.*s", (int)(name_end - name), name);
    const char *entry = strstr(msl, "fragment main0_out main0(");
    const char *signature_end = entry ? strstr(entry, ")\n{") : NULL;
    if (!signature_end) { free(flat); return NULL; }
    char test[256];
    if (func == 1) snprintf(test, sizeof test, "{ discard_fragment(); return out; }");
    else if (func == 8) snprintf(test, sizeof test, "return out;");
    else snprintf(test, sizeof test, "{ if (!(out.%s.a %s glm_alpha_ref)) discard_fragment(); return out; }", member, tests[func - 1]);
    size_t cap = strlen(msl) * 2 + 4096, n = 0;
    char *result = malloc(cap);
    const char *param = ", constant float& glm_alpha_ref [[buffer(23)]]";
    for (const char *q = msl; *q;) {
        if (q == signature_end) {
            n += (size_t)snprintf(result + n, cap - n, "%s", param);
        }
        if (q > signature_end && !strncmp(q, "return out;", 11)) {
            n += (size_t)snprintf(result + n, cap - n, "%s", test);
            q += 11;
            continue;
        }
        if (!strncmp(q, "[[early_fragment_tests]]", 24)) { /* the test discards */
            q += 24;
            continue;
        }
        if (n + 1 >= cap) {
            free(result);
            free(flat);
            return NULL;
        }
        result[n++] = *q++;
    }
    result[n] = 0;
    free(flat);
    return result;
}

/* Shader work can start asynchronously, but its first consumer must wait.
   Dropping a draw or clear can leave cached shadow maps corrupted indefinitely. */
static id<MTLFunction> variant_function(struct glm_int_variant *v)
{
    pthread_mutex_lock(&variant_lock);
    bool ready = v->ready;
    pthread_mutex_unlock(&variant_lock);
    if (!ready) {
        uint64_t started = glm_now_ns();
        dispatch_group_wait((__bridge dispatch_group_t)v->group, DISPATCH_TIME_FOREVER);
        glm_note_stall(v->border_mask ? "border emulation variant" : "integer input variant", started);
    }
    pthread_mutex_lock(&variant_lock);
    id<MTLFunction> function = (__bridge id<MTLFunction>)v->function;
    pthread_mutex_unlock(&variant_lock);
    return function;
}

/* A recompiled stage of the program: the vertex function with the float
   inputs in `uint_inputs` / `int_inputs` declared as integers, or (with
   `border_mask`) the fragment function emulating those samplers' borders.
   Compiled in the background and waited for (see variant_function); the
   variant is shared by the program and the job. */
static id<MTLFunction> program_variant(struct glm_program *p, uint32_t uint_inputs,
                                       uint32_t int_inputs, uint32_t border_mask, uint32_t alpha_func)
{
    for (int i = 0; i < p->int_variant_count; ++i) {
        struct glm_int_variant *v = p->int_variants[i];
        if (v->uint_inputs != uint_inputs || v->int_inputs != int_inputs || v->border_mask != border_mask ||
            v->alpha_func != alpha_func)
            continue;
        return variant_function(v);
    }
    int stage = border_mask ? GLM_STAGE_FRAGMENT : GLM_STAGE_VERTEX;
    if (p->int_variant_count == 16) return nil;
    /* Alpha test: patch the fragment MSL. Integer inputs: patch the
       program's own MSL (cheap), else recompile. */
    char *patched = alpha_func ? (p->result.msl[GLM_STAGE_FRAGMENT] ? alpha_variant_msl(p->result.msl[GLM_STAGE_FRAGMENT], alpha_func) : NULL)
                    : !border_mask && p->result.msl[GLM_STAGE_VERTEX] && !getenv("GLMETAL_NO_MSL_VARIANTS")
                        ? int_variant_msl(p->result.msl[GLM_STAGE_VERTEX], uint_inputs, int_inputs)
                        : NULL;
    if (alpha_func && !patched) return nil;
    if (patched) {
        GLMCompiledFunction *entry = function_entry(@(patched));
        free(patched);
        if (!alpha_func) remember_variant(p->result.msl[GLM_STAGE_VERTEX], uint_inputs, int_inputs);
        struct glm_int_variant *v = calloc(1, sizeof *v);
        v->uint_inputs = uint_inputs;
        v->int_inputs = int_inputs;
        v->alpha_func = alpha_func;
        v->refs = 2;
        dispatch_group_t group = dispatch_group_create();
        dispatch_group_enter(group);
        v->group = (__bridge_retained void *)group;
        p->int_variants[p->int_variant_count++] = v;
        dispatch_group_notify(entry->group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            id<MTLFunction> function = entry->function;
            if (!function)
                glm_log("%s variant: %s", alpha_func ? "alpha test" : "integer input",
                        entry->error ? entry->error.localizedDescription.UTF8String : "no main0");
            pthread_mutex_lock(&variant_lock);
            v->function = function ? (__bridge_retained void *)function : NULL;
            v->ready = true;
            pthread_mutex_unlock(&variant_lock);
            __atomic_add_fetch(&glm_object_generation, 1, __ATOMIC_RELEASE);
            dispatch_group_leave(group);
            variant_release(v);
        });
        return variant_function(v);
    }
    if (!p->linked_sources[stage]) return nil;
    struct glm_int_variant *v = calloc(1, sizeof *v);
    v->uint_inputs = uint_inputs;
    v->int_inputs = int_inputs;
    v->border_mask = border_mask;
    char **border_names = calloc(32, sizeof *border_names);
    for (int i = 0; i < p->result.uniform_count; ++i) {
        int slot = p->result.uniforms[i].sampler_slot;
        if (slot >= 0 && slot < 32 && (border_mask >> slot) & 1 && p->result.uniforms[i].offset < 0)
            border_names[slot] = strdup(p->result.uniforms[i].name);
    }
    v->refs = 2;
    dispatch_group_t group = dispatch_group_create();
    dispatch_group_enter(group);
    v->group = (__bridge_retained void *)group;
    p->int_variants[p->int_variant_count++] = v;
    /* The job owns copies of everything it reads. */
    char **sources = calloc(GLM_STAGE_COUNT, sizeof *sources);
    for (int stage = 0; stage < GLM_STAGE_COUNT; ++stage)
        if (p->linked_sources[stage]) sources[stage] = strdup(p->linked_sources[stage]);
    int attribute_count = p->attribute_binding_count, output_count = p->output_binding_count;
    struct glm_name_location *attributes = calloc((size_t)attribute_count + 1, sizeof *attributes);
    struct glm_name_location *outputs = calloc((size_t)output_count + 1, sizeof *outputs);
    for (int i = 0; i < attribute_count; ++i)
        attributes[i] = (struct glm_name_location){strdup(p->attribute_bindings[i].name), p->attribute_bindings[i].location};
    for (int i = 0; i < output_count; ++i)
        outputs[i] = (struct glm_name_location){strdup(p->output_bindings[i].name), p->output_bindings[i].location};
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("GLMetal shader variants", dispatch_queue_attr_make_with_qos_class(
                                                                     DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    });
    dispatch_block_t job = ^{
        struct glm_compile_request request = {0};
        for (int stage = 0; stage < GLM_STAGE_COUNT; ++stage) request.sources[stage] = sources[stage];
        request.attributes = attributes;
        request.attribute_count = attribute_count;
        request.frag_outputs = outputs;
        request.frag_output_count = output_count;
        request.uint_inputs = uint_inputs;
        request.int_inputs = int_inputs;
        for (int k = 0; k < 32; ++k) request.border_samplers[k] = border_names[k];
        struct glm_compile_result result;
        uint64_t started = glm_now_ns();
        glm_program_compile(&request, &result);
        glm_note_stall("program variant compile", started);
        id<MTLFunction> function = nil;
        if (result.ok && result.msl[stage]) {
            MTLCompileOptions *options = [MTLCompileOptions new];
            options.fastMathEnabled = NO;
            options.languageVersion = MTLLanguageVersion2_3;
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:@(result.msl[stage]) options:options error:&error];
            function = [library newFunctionWithName:@"main0"];
            remember_function_source(function, @(result.msl[stage]));
            if (!function) glm_log("program variant: %s", error.localizedDescription.UTF8String);
        } else {
            glm_log("program variant failed: %s", result.log ? result.log : "");
        }
        for (int k = 0; k < 32; ++k) free(border_names[k]);
        free(border_names);
        glm_compile_result_free(&result);
        for (int stage = 0; stage < GLM_STAGE_COUNT; ++stage) free(sources[stage]);
        free(sources);
        for (int i = 0; i < attribute_count; ++i) free((char *)attributes[i].name);
        for (int i = 0; i < output_count; ++i) free((char *)outputs[i].name);
        free(attributes);
        free(outputs);
        pthread_mutex_lock(&variant_lock);
        v->function = function ? (__bridge_retained void *)function : NULL;
        v->ready = true;
        pthread_mutex_unlock(&variant_lock);
        /* Draws with the variant must rebuild their pipeline. */
        __atomic_add_fetch(&glm_object_generation, 1, __ATOMIC_RELEASE);
        dispatch_group_leave(group);
        variant_release(v);
    };
    dispatch_async(queue, job);
    return variant_function(v);
}

/* What the vertex function reads: bit per attribute, and which are integer. */
static void vertex_inputs(const struct draw_stages *st, uint32_t *used, uint32_t *integer, uint32_t *unsigned_mask)
{
    *used = *integer = *unsigned_mask = 0;
    if (!st->vertex_program) {
        /* Fixed function (ffgen.c VIn): position, normal, colours, fog and
           eight texture coordinates. */
        *used = 1u << GLM_ATTR_POSITION | 1u << GLM_ATTR_NORMAL | 1u << GLM_ATTR_COLOR | 1u << GLM_ATTR_SECONDARY_COLOR |
                1u << GLM_ATTR_FOG | 0xffu << GLM_ATTR_TEX0;
        return;
    }
    struct glm_program *p = st->vertex_program;
    if (p->inputs_link && p->inputs_link == p->link_serial) {
        *used = p->inputs_used;
        *integer = p->inputs_integer;
        *unsigned_mask = p->inputs_unsigned;
        return;
    }
    const struct glm_compile_result *r = &p->result;
    for (int i = 0; i < r->attribute_count; ++i) {
        const struct glm_io_info *a = &r->attributes[i];
        if (a->index == GLM_INPUT_BUILTIN) continue;
        int columns = 1;
        switch (a->type) {
        case GL_FLOAT_MAT2: case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT2x4: columns = 2; break;
        case GL_FLOAT_MAT3: case GL_FLOAT_MAT3x2: case GL_FLOAT_MAT3x4: columns = 3; break;
        case GL_FLOAT_MAT4: case GL_FLOAT_MAT4x2: case GL_FLOAT_MAT4x3: columns = 4; break;
        }
        bool is_unsigned = a->type == GL_UNSIGNED_INT || (a->type >= GL_UNSIGNED_INT_VEC2 && a->type <= GL_UNSIGNED_INT_VEC4);
        for (int c = 0; c < columns * a->array_size; ++c) {
            int location = a->location + c;
            if (location < 0 || location >= GLM_MAX_ATTRIBS) continue;
            *used |= 1u << location;
            if (a->integer) *integer |= 1u << location;
            if (is_unsigned) *unsigned_mask |= 1u << location;
        }
    }
    p->inputs_used = *used;
    p->inputs_integer = *integer;
    p->inputs_unsigned = *unsigned_mask;
    p->inputs_link = p->link_serial;
}

/* Interned vertex layouts: pipeline keys name them by id. */
struct gpu_layout_key {
    struct { uint8_t format, slot; uint16_t offset; } attribute[GLM_MAX_ATTRIBS];
    struct { uint32_t stride, rate; uint8_t step, pad[3]; } slot[GLM_VERTEX_SLOTS];
};

static uint32_t intern_layout(struct glm_backend_context *b, const struct gpu_layout_key *key, MTLVertexDescriptor **out)
{
    static NSMutableDictionary<NSData *, NSNumber *> *ids;
    static NSMutableArray<MTLVertexDescriptor *> *layouts;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static __thread struct gpu_layout_key last_key;
    static __thread uint32_t last_id;
    static __thread __unsafe_unretained MTLVertexDescriptor *last_layout;
    (void)b;
    if (last_id && !memcmp(key, &last_key, sizeof *key)) {
        *out = last_layout;
        return last_id;
    }
    NSData *data = [NSData dataWithBytes:key length:sizeof *key];
    pthread_mutex_lock(&lock);
    if (!ids) {
        ids = [NSMutableDictionary dictionary];
        layouts = [NSMutableArray array];
    }
    NSNumber *number = ids[data];
    if (!number) {
        MTLVertexDescriptor *d = [MTLVertexDescriptor vertexDescriptor];
        for (int i = 0; i < GLM_MAX_ATTRIBS; ++i) {
            if (!key->attribute[i].format) continue;
            d.attributes[i].format = (MTLVertexFormat)key->attribute[i].format;
            d.attributes[i].offset = key->attribute[i].offset;
            d.attributes[i].bufferIndex = GLM_SLOT_VERTEX0 + key->attribute[i].slot;
        }
        for (int i = 0; i < GLM_VERTEX_SLOTS; ++i) {
            if (!key->slot[i].stride) continue;
            MTLVertexBufferLayoutDescriptor *l = d.layouts[GLM_SLOT_VERTEX0 + i];
            l.stride = key->slot[i].stride;
            l.stepFunction = (MTLVertexStepFunction)key->slot[i].step;
            l.stepRate = key->slot[i].step == MTLVertexStepFunctionConstant ? 0 : key->slot[i].rate;
        }
        [layouts addObject:d];
        number = @(layouts.count); /* ids start at 1; 0 is the CPU path's layout */
        ids[data] = number;
    }
    *out = layouts[number.unsignedIntValue - 1];
    pthread_mutex_unlock(&lock);
    last_key = *key;
    last_id = number.unsignedIntValue;
    last_layout = *out;
    return last_id;
}

/* A slot's source: a GL buffer, or client memory copied for the draw. */
struct vertex_slot {
    struct glm_buffer *buffer;
    const uint8_t *client;
    uintptr_t offset;          /* of the first attribute's data */
    uint32_t stride, divisor, end; /* end: bytes past `offset` the last vertex reaches */
};

/* Drops reads whose command buffers have completed. */
static void prune_gpu_reads(struct glm_buffer *buffer)
{
    struct glm_backend_context *reader = buffer->gpu_reader;
    uint64_t done = reader ? __atomic_load_n(&reader->completed_serial, __ATOMIC_ACQUIRE) : UINT64_MAX;
    int kept = 0;
    for (int i = 0; i < buffer->gpu_read_count; ++i)
        if (buffer->gpu_reads[i].serial == UINT64_MAX || buffer->gpu_reads[i].serial > done)
            buffer->gpu_reads[kept++] = buffer->gpu_reads[i];
    buffer->gpu_read_count = kept;
    if (!kept) buffer->gpu_reader = NULL;
}

static void note_gpu_read(struct glm_context *ctx, struct glm_buffer *buffer, GLintptr low, GLintptr high)
{
    struct glm_backend_context *b = ctx->backend;
    uint64_t serial = glm_backend_pending_serial(ctx);
    int last = buffer->gpu_read_count - 1;
    if (buffer->gpu_reader == b && last >= 0 && buffer->gpu_reads[last].serial == serial) {
        /* Already read by this command buffer: widen its range. */
        if (low < buffer->gpu_reads[last].low) buffer->gpu_reads[last].low = low;
        if (high > buffer->gpu_reads[last].high) buffer->gpu_reads[last].high = high;
        return;
    }
    if (buffer->gpu_reader && buffer->gpu_reader != b) {
        /* Another context's reads: keep them as if current (conservative). */
        for (int i = 0; i < buffer->gpu_read_count; ++i) buffer->gpu_reads[i].serial = UINT64_MAX;
    } else {
        prune_gpu_reads(buffer);
    }
    buffer->gpu_reader = b;
    buffer->gpu_serial = serial;
    int n = buffer->gpu_read_count;
    if (n && buffer->gpu_reads[n - 1].serial == serial) {
        if (low < buffer->gpu_reads[n - 1].low) buffer->gpu_reads[n - 1].low = low;
        if (high > buffer->gpu_reads[n - 1].high) buffer->gpu_reads[n - 1].high = high;
        return;
    }
    if (n == 8) {
        /* Preserve unknown completion when merging an orphaned range. */
        if (buffer->gpu_reads[0].serial > buffer->gpu_reads[1].serial)
            buffer->gpu_reads[1].serial = buffer->gpu_reads[0].serial;
        if (buffer->gpu_reads[0].low < buffer->gpu_reads[1].low) buffer->gpu_reads[1].low = buffer->gpu_reads[0].low;
        if (buffer->gpu_reads[0].high > buffer->gpu_reads[1].high) buffer->gpu_reads[1].high = buffer->gpu_reads[0].high;
        memmove(buffer->gpu_reads, buffer->gpu_reads + 1, 7 * sizeof buffer->gpu_reads[0]);
        n = 7;
    }
    buffer->gpu_reads[n].serial = serial;
    buffer->gpu_reads[n].low = low;
    buffer->gpu_reads[n].high = high;
    buffer->gpu_read_count = n + 1;
}

/* The GPU vertex layout for the bound arrays and the vertex function's
   inputs; memoized, since array state rarely changes between draws. */
struct gpu_layout {
    bool valid;
    const char *fallback;       /* non-NULL: the CPU path must draw */
    struct glm_vertex_array *vao;
    uint64_t vao_serial;
    struct glm_array arrays[GLM_MAX_ATTRIBS];
    uint32_t used, integer, unsigned_mask, constant_mask;
    uint64_t buffer_generation;
    struct vertex_slot slots[GLM_VERTEX_SLOTS];
    int slot_count, constant_slot;
    bool any_client;
    MTLVertexDescriptor *__unsafe_unretained layout; /* interned, never freed */
    uint32_t layout_id;
};

static bool fail_layout(struct gpu_layout *L, const char *reason)
{
    L->fallback = reason;
    return false;
}

static bool build_layout(struct glm_context *ctx, struct gpu_layout *L, uint32_t used, uint32_t integer,
                         uint32_t unsigned_mask)
{
    struct glm_backend_context *b = ctx->backend;
    L->fallback = NULL;
    struct gpu_layout_key key;
    memset(&key, 0, sizeof key);
    struct vertex_slot *slots = L->slots;
    int slot_count = 0;
    bool any_client = false;
    int constant_slot = -1;
    L->constant_mask = 0;
    for (int i = 0; i < GLM_MAX_ATTRIBS; ++i) {
        if (!(used & (1u << i))) continue;
        const struct glm_array *a = &ctx->vao->arrays[i];
        bool int_input = (integer >> i) & 1;
        if (!a->enabled) {
            if (constant_slot < 0) {
                if (slot_count == GLM_VERTEX_SLOTS) return fail_layout(L, "slot_count == GLM_VERTEX_SLOTS");
                constant_slot = slot_count++;
                memset(&slots[constant_slot], 0, sizeof slots[constant_slot]);
                key.slot[constant_slot].stride = GLM_MAX_ATTRIBS * 16;
                key.slot[constant_slot].step = MTLVertexStepFunctionConstant;
            }
            L->constant_mask |= 1u << i;
            key.attribute[i].format = (uint8_t)(int_input ? ((unsigned_mask >> i) & 1 ? MTLVertexFormatUInt4 : MTLVertexFormatInt4)
                                                          : MTLVertexFormatFloat4);
            key.attribute[i].slot = (uint8_t)constant_slot;
            key.attribute[i].offset = (uint16_t)(i * 16);
            continue;
        }
        MTLVertexFormat format = vertex_format(a, int_input);
        if (format == MTLVertexFormatInvalid) {
            static uint32_t logged[32];
            uint32_t key = (uint32_t)a->type << 16 ^ (uint32_t)a->size << 8 ^ (uint32_t)a->normalized << 2 ^
                           (uint32_t)a->integer << 1 ^ (uint32_t)int_input;
            bool seen = false;
            for (int k = 0; k < 32 && logged[k]; ++k) seen |= logged[k] == key;
            if (!seen) {
                for (int k = 0; k < 32; ++k)
                    if (!logged[k]) { logged[k] = key; break; }
                glm_log("no Metal vertex format: attribute %d type 0x%04x size %d normalized %d integer array %d input %d", i,
                        a->type, a->size, a->normalized, a->integer, int_input);
            }
            return fail_layout(L, "format == MTLVertexFormatInvalid");
        }
        size_t element = gl_type_size(a->type) * (size_t)(a->size == GL_BGRA ? 4 : a->size);
        uint32_t stride = a->stride ? (uint32_t)a->stride : (uint32_t)element;
        if (stride % 4 || stride > 2048) return fail_layout(L, "stride % 4 || stride > 2048");
        struct glm_buffer *buffer = a->buffer ? glm_buffer_get(ctx, a->buffer) : NULL;
        if (a->buffer && (!buffer || !buffer->backend)) return fail_layout(L, "a->buffer && (!buffer || !buffer->backend)");
        const uint8_t *client = a->buffer ? NULL : a->pointer;
        if (!a->buffer && !client) return fail_layout(L, "!a->buffer && !client");
        if (a->divisor && client) return fail_layout(L, "a->divisor && client");
        uintptr_t offset = a->buffer ? (uintptr_t)a->pointer : 0;
        /* Share a slot with an interleaved neighbour. */
        int slot = -1;
        for (int k = 0; k < slot_count; ++k) {
            struct vertex_slot *v = &slots[k];
            if (k == constant_slot || v->stride != stride || v->divisor != a->divisor || v->buffer != buffer) continue;
            if (buffer) {
                if (offset >= v->offset && offset + element <= v->offset + stride) { slot = k; break; }
            } else if (client >= v->client && client + element <= v->client + stride) {
                slot = k;
                break;
            }
        }
        if (slot < 0) {
            if (slot_count == GLM_VERTEX_SLOTS) return fail_layout(L, "slot_count == GLM_VERTEX_SLOTS");
            slot = slot_count++;
            slots[slot] = (struct vertex_slot){buffer, client, offset, stride, a->divisor, 0};
            key.slot[slot].stride = stride;
            key.slot[slot].step = a->divisor ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            key.slot[slot].rate = a->divisor ? a->divisor : 1;
        }
        uintptr_t relative = buffer ? offset - slots[slot].offset : (uintptr_t)(client - slots[slot].client);
        if (relative % 4 && format != MTLVertexFormatUChar4Normalized_BGRA && gl_type_size(a->type) != 1) return fail_layout(L, "relative % 4 && format != MTLVertexFormatUChar4Normalized_BG");
        if (relative % 4) return fail_layout(L, "relative % 4");
        if (relative + element > slots[slot].end) slots[slot].end = (uint32_t)(relative + element);
        key.attribute[i].format = (uint8_t)format;
        key.attribute[i].slot = (uint8_t)slot;
        key.attribute[i].offset = (uint16_t)relative;
        if (client) any_client = true;
    }
    /* Buffer bindings must be 4-byte aligned. */
    for (int k = 0; k < slot_count; ++k)
        if (k != constant_slot && slots[k].buffer && slots[k].offset % 4) return fail_layout(L, "k != constant_slot && slots[k].buffer && slots[k].offset % 4");

    L->slot_count = slot_count;
    L->constant_slot = constant_slot;
    L->any_client = any_client;
    MTLVertexDescriptor *layout = nil;
    L->layout_id = intern_layout(b, &key, &layout);
    L->layout = layout;
    return true;
}

static uint32_t read_gl_index(const uint8_t *p, GLenum type, uint32_t i)
{
    switch (type) {
    case GL_UNSIGNED_BYTE: return p[i];
    case GL_UNSIGNED_SHORT: { uint16_t v; memcpy(&v, p + 2 * i, 2); return v; }
    default: { uint32_t v; memcpy(&v, p + 4 * i, 4); return v; }
    }
}

/* Logs (once each, with GLMETAL_DEBUG) why draws take the CPU path. */
static bool gpu_fallback(const char *reason)
{
    static const char *seen[64];
    static unsigned hits[64], total;
    static int count;
    static int stats = -1;
    if (stats < 0) stats = getenv("GLMETAL_STREAM_STATS") != NULL;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    int at = -1;
    for (int i = 0; i < count; ++i)
        if (seen[i] == reason) at = i;
    bool known = at >= 0;
    if (!known && count < 64) {
        at = count;
        seen[count++] = reason;
    }
    if (at >= 0) ++hits[at];
    /* GLMETAL_STREAM_STATS: which reasons send draws to the CPU path. */
    if (stats && ++total % 20000 == 0)
        for (int i = 0; i < count; ++i)
            if (hits[i]) fprintf(stderr, "glmetal: CPU vertex path %u draws: %s\n", hits[i], seen[i]);
    pthread_mutex_unlock(&lock);
    if (!known) glm_log("CPU vertex path: %s", reason);
    return false;
}

/* An indexed draw with primitive restart as a list of the runs between
   restart indices (each converted like an unrestarted draw); malloc'd. */
static struct glm_index_list restart_list(GLenum mode, const uint8_t *data, GLenum type, GLsizei count, uint32_t skip,
                                          uint32_t *metal_primitive)
{
    struct glm_index_list list = {0};
    uint32_t *run = malloc((size_t)count * sizeof(uint32_t) + 4);
    if (!run) return list;
    GLsizei start = 0;
    while (start < count) {
        uint32_t n = 0;
        GLsizei i = start;
        for (; i < count; ++i) {
            uint32_t v = read_gl_index(data, type, (uint32_t)i);
            if (v == skip) break;
            run[n++] = v;
        }
        if (n) glm_triangulate(mode, run, n, &list, metal_primitive, false);
        start = i + 1;
    }
    free(run);
    return list;
}

/* GLMETAL_TRACE_FRAME=N: one line per draw of presented frame N (sRGB debugging). */
static bool tracing(void)
{
    static long frame = -2;
    if (frame == -2) frame = getenv("GLMETAL_TRACE_FRAME") ? atol(getenv("GLMETAL_TRACE_FRAME")) : -1;
    return frame >= 0 && glm_trace_frames >= (unsigned long)frame && glm_trace_frames % 1000 == 0;
}

static void trace_draw(struct glm_context *ctx, const char *what)
{
    struct glm_backend_context *b = ctx->backend;
    struct glm_program *p = glm_current_program(ctx);
    char shader[64] = "-";
    const char *fs = p ? p->linked_sources[GLM_STAGE_FRAGMENT] : NULL;
    if (fs) {
        const char *line = strchr(fs, '\n');
        if (line && line[1] == '/' ) sscanf(line + 1, "// %63s", shader);
    }
    char textures[512] = "";
    for (int u = 0; u < 16; ++u) {
        GLuint name = ctx->state.units[u].bound[GLM_TEX_2D];
        struct glm_texture *t = name ? glm_texture_get(ctx, name) : NULL;
        if (!t || !t->levels[0][0].defined || t->levels[0][0].width < 256) continue;
        size_t n = strlen(textures);
        snprintf(textures + n, sizeof textures - n, " u%d:%u/%04x%s%s", u, name, t->levels[0][0].internal_format,
                 t->sampler.skip_srgb_decode ? "/skip" : "", ctx->state.units[u].sampler ? "/sampler" : "");
    }
    fprintf(stderr, "TRACE ctx %p tex %p %s fb %u srgb %d fmt %d parent %d %dx%d blend %d prog %u %s |%s\n",
            (void *)ctx, (__bridge void *)b->color[0], what, ctx->draw_framebuffer, ctx->state.framebuffer_srgb, b->color[0] ? (int)b->color[0].pixelFormat : -1,
            b->color[0].parentTexture ? (int)b->color[0].parentTexture.pixelFormat : -1,
            b->color[0] ? (int)b->color[0].width : 0, b->color[0] ? (int)b->color[0].height : 0,
            ctx->state.blend[0].enabled, p ? p->name : 0, shader, textures);
}

bool glm_backend_draw_gpu(struct glm_context *ctx, GLenum mode, GLint first, GLsizei count, GLenum index_type,
                          const void *indices, GLsizei instances, GLint base_vertex)
{
    if (tracing() && ensure_encoder(ctx)) trace_draw(ctx, "draw");
    static int disabled = -1;
    if (disabled < 0) disabled = getenv("GLMETAL_CPU_VERTICES") != NULL;
    if (disabled) return gpu_fallback("disabled");
    struct glm_backend_context *b = ctx->backend;
    const struct glm_state *s = &ctx->state;
    /* A bound program pipeline copies its stage programs' uniforms now. */
    if (!ctx->current_program && ctx->bound_pipeline) glm_pipeline_program(ctx);
    struct draw_stages st;
    if (!select_stages(ctx, &st)) return true; /* nothing would be drawn */
    bool indexed = index_type != 0;
    if (mode == GL_PATCHES || (st.program && (st.program->gs || st.program->tess))) return gpu_fallback("geometry or tessellation stage");
    if (mode >= GL_TRIANGLES && (s->polygon_mode[0] != GL_FILL || s->polygon_mode[1] != GL_FILL))
        return gpu_fallback("polygon mode (vertex.c draws polygon outlines and points)");

    /* Primitive: native, or indices generated on the CPU. */
    MTLPrimitiveType primitive;
    bool native = true;
    switch (mode) {
    case GL_POINTS: primitive = MTLPrimitiveTypePoint; break;
    case GL_LINES: primitive = MTLPrimitiveTypeLine; break;
    case GL_LINE_STRIP: primitive = MTLPrimitiveTypeLineStrip; break;
    case GL_TRIANGLES: primitive = MTLPrimitiveTypeTriangle; break;
    case GL_TRIANGLE_STRIP: primitive = MTLPrimitiveTypeTriangleStrip; break;
    case GL_LINE_LOOP: case GL_TRIANGLE_FAN: case GL_QUADS: case GL_QUAD_STRIP: case GL_POLYGON:
        native = false;
        primitive = MTLPrimitiveTypeTriangle;
        break;
    default: return gpu_fallback("patches");
    }
    /* Flat shading needs GL's provoking vertex: the CPU path handles it. */
    if (mode != GL_POINTS) {
        if (st.ff_fragment && !st.vertex_program && s->shade_model == GL_FLAT && ctx->profile == GLM_PROFILE_LEGACY) return gpu_fallback("st.ff_fragment && !st.vertex_program && s->shade_model == GL");
        if (st.fragment_program && st.fragment_program->result.flat_inputs) return gpu_fallback("st.fragment_program && st.fragment_program->result.flat_inpu");
        if (st.fragment_program && st.fragment_program->color_inputs && s->shade_model == GL_FLAT && ctx->profile == GLM_PROFILE_LEGACY)
            return gpu_fallback("flat colour inputs");
        if (st.ff_fragment && s->shade_model == GL_FLAT) return gpu_fallback("st.ff_fragment && s->shade_model == GL_FLAT");
    }
    bool restart = indexed && s->primitive_restart;
    /* Metal restarts strips at 0xffff / 0xffffffff only; other restart
       indices, and restart in lists or emulated modes, draw a list of the
       runs between restarts (restart_list). */
    bool rewrite = false;
    if (restart) {
        uint32_t max = index_type == GL_UNSIGNED_SHORT ? 0xffff : index_type == GL_UNSIGNED_INT ? 0xffffffffu : 0;
        rewrite = !native || (primitive != MTLPrimitiveTypeLineStrip && primitive != MTLPrimitiveTypeTriangleStrip) ||
                  s->primitive_restart_index != max;
    }

    /* Index source and the vertex range the draw reads. */
    const uint8_t *index_data = NULL;
    struct glm_buffer *index_buffer = NULL;
    if (indexed) {
        if (ctx->vao->element_buffer) {
            index_buffer = glm_buffer_get(ctx, ctx->vao->element_buffer);
            const uint8_t *contents = index_buffer ? glm_backend_buffer_contents(ctx, index_buffer, false) : NULL;
            if (!contents) return gpu_fallback("!contents");
            if ((uintptr_t)indices + (size_t)count * gl_type_size(index_type) > (uintptr_t)index_buffer->size) return gpu_fallback("(uintptr_t)indices + (size_t)count * gl_type_size(index_type");
            index_data = contents + (uintptr_t)indices;
        } else {
            index_data = indices;
        }
        if (!index_data) return true;
    }
    int64_t low, high;
    if (indexed) {
        uint32_t lo = UINT32_MAX, hi = 0;
        uint32_t skip = restart ? s->primitive_restart_index : UINT32_MAX;
        /* Static index buffers: the range of a (buffer, offset, count)
           is remembered until the buffer's contents change. */
        struct index_range_memo *memo = NULL;
        if (index_buffer && !index_buffer->mapped) {
            uintptr_t hash = ((uintptr_t)index_buffer->backend >> 4) ^ (uintptr_t)indices * 31 ^ (uintptr_t)count * 17;
            memo = &b->index_ranges[hash % (sizeof b->index_ranges / sizeof b->index_ranges[0])];
            if (memo->storage == index_buffer->backend && memo->generation == index_buffer->content_generation &&
                memo->offset == (uintptr_t)indices && memo->count == count && memo->type == index_type &&
                memo->restart == (restart ? skip : 0)) {
                lo = memo->low;
                hi = memo->high;
            } else {
                memo->storage = NULL;
            }
        }
        if (!memo || !memo->storage) {
            for (GLsizei i = 0; i < count; ++i) {
                uint32_t v = read_gl_index(index_data, index_type, (uint32_t)i);
                if (restart && v == skip) continue;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            if (memo) {
                if (memo->list) CFRelease(memo->list);
                *memo = (struct index_range_memo){index_buffer->backend, index_buffer->content_generation,
                                                  (uintptr_t)indices, count, index_type, restart ? skip : 0, lo, hi};
            }
        }
        if (lo > hi) return true;
        low = (int64_t)lo + base_vertex;
        high = (int64_t)hi + base_vertex;
        if (low < 0) return gpu_fallback("low < 0");
    } else {
        low = first;
        high = (int64_t)first + count - 1;
    }

    /* Vertex layout. */
    uint32_t used, integer, unsigned_mask;
    vertex_inputs(&st, &used, &integer, &unsigned_mask);
    /* Unnormalized integer arrays for float inputs: Metal fetches them only
       into integer inputs, so the program's integer variant draws. */
    b->vertex_override = nil;
    uint32_t convert_uint = 0, convert_int = 0;
    for (uint32_t bits = used & ~integer; bits; bits &= bits - 1) {
        int i = __builtin_ctz(bits);
        const struct glm_array *a = &ctx->vao->arrays[i];
        if (!a->enabled || a->normalized || a->integer) continue;
        if (a->type == GL_UNSIGNED_BYTE || a->type == GL_UNSIGNED_SHORT || a->type == GL_UNSIGNED_INT) convert_uint |= 1u << i;
        else if (a->type == GL_BYTE || a->type == GL_SHORT || a->type == GL_INT) convert_int |= 1u << i;
    }
    static int no_variants = -1;
    if (no_variants < 0) no_variants = getenv("GLMETAL_NO_INT_VARIANTS") != NULL;
    if ((convert_uint | convert_int) && no_variants) return gpu_fallback("integer data for float inputs (variants off)");
    if (convert_uint | convert_int) {
        if (!st.vertex_program || st.vertex_program != st.program) return gpu_fallback("integer data for float inputs");
        id<MTLFunction> variant = program_variant(st.vertex_program, convert_uint, convert_int, 0, 0);
        if (!variant) return gpu_fallback("integer input variant failed");
        b->vertex_override = variant;
        integer |= convert_uint | convert_int;
        unsigned_mask |= convert_uint;
    }
    /* Border colours / GL_CLAMP Metal's samplers cannot give: the program's
       border emulation variant draws once compiled. */
    b->fragment_override = nil;
    b->border_mask = 0;
    if (__atomic_load_n(&glm_custom_borders, __ATOMIC_RELAXED) && st.program && st.fragment_program == st.program) {
        uint32_t mask = glm_program_border_mask(ctx, st.program, b->border_data);
        id<MTLFunction> variant = mask ? program_variant(st.program, 0, 0, mask, 0) : nil;
        if (variant) {
            b->fragment_override = variant;
            b->border_mask = mask;
        }
    }
    static __thread struct gpu_layout cache;
    struct gpu_layout *L = &cache;
    uint64_t buffer_generation = __atomic_load_n(&glm_object_generation, __ATOMIC_ACQUIRE);
    if (!L->valid || L->vao != ctx->vao || L->vao_serial != ctx->vao->serial || L->used != used || L->integer != integer ||
        L->unsigned_mask != unsigned_mask || L->buffer_generation != buffer_generation) {
        L->valid = false;
        L->vao_serial = ctx->vao->serial;
        memcpy(L->arrays, ctx->vao->arrays, sizeof L->arrays);
        L->vao = ctx->vao;
        L->used = used;
        L->integer = integer;
        L->unsigned_mask = unsigned_mask;
        L->buffer_generation = buffer_generation;
        build_layout(ctx, L, used, integer, unsigned_mask);
        L->valid = true;
    }
    if (L->fallback) return gpu_fallback(L->fallback);
    struct vertex_slot *slots = L->slots;
    int slot_count = L->slot_count, constant_slot = L->constant_slot;
    bool any_client = L->any_client;
    __unsafe_unretained MTLVertexDescriptor *layout = L->layout;
    uint32_t layout_id = L->layout_id;
    float constants[GLM_MAX_ATTRIBS][4];
    for (uint32_t bits = L->constant_mask; bits; bits &= bits - 1) {
        int i = __builtin_ctz(bits);
        if ((integer >> i) & 1) {
            int32_t v[4];
            for (int c = 0; c < 4; ++c) v[c] = (int32_t)s->current[i][c];
            memcpy(constants[i], v, sizeof v);
        } else {
            memcpy(constants[i], s->current[i], sizeof constants[i]);
        }
    }

    /* Client arrays are copied for [low, high]; every per-vertex source is
       then addressed relative to `low`. */
    int64_t rebase = any_client ? low : 0;

    /* Index buffer. */
    /* Owned by the index buffer, the memo or this command buffer. */
    __unsafe_unretained id<MTLBuffer> metal_indices = nil;
    NSUInteger metal_index_offset = 0;
    MTLIndexType metal_index_type = MTLIndexTypeUInt32;
    uint32_t draw_count = (uint32_t)count;
    NSInteger index_base = indexed ? base_vertex : 0;
    if (rewrite) {
        struct index_range_memo *memo = NULL;
        if (index_buffer && !index_buffer->mapped) {
            uintptr_t hash = ((uintptr_t)index_buffer->backend >> 4) ^ (uintptr_t)indices * 31 ^ (uintptr_t)count * 17;
            memo = &b->index_ranges[hash % (sizeof b->index_ranges / sizeof b->index_ranges[0])];
            if (memo->storage != index_buffer->backend || memo->generation != index_buffer->content_generation ||
                memo->offset != (uintptr_t)indices || memo->count != count || memo->type != index_type)
                memo = NULL;
        }
        if (memo && memo->list) {
            metal_indices = (__bridge id<MTLBuffer>)memo->list;
            draw_count = memo->list_count;
            primitive = (MTLPrimitiveType)memo->list_primitive;
        } else {
            uint32_t metal_primitive = MTLPrimitiveTypeTriangle;
            struct glm_index_list list = restart_list(mode, index_data, index_type, count, s->primitive_restart_index,
                                                      &metal_primitive);
            primitive = (MTLPrimitiveType)metal_primitive;
            draw_count = list.count;
            if (!list.count) {
                free(list.data);
                return true;
            }
            /* Buffer objects' lists are kept (static index buffers repeat);
               client indices use this command buffer's memory. */
            if (memo) {
                id<MTLBuffer> kept = glm_new_shared_buffer((NSUInteger)list.count * 4, list.data);
                memo->list = (__bridge_retained void *)kept;
                metal_indices = (__bridge id<MTLBuffer>)memo->list;
                metal_index_offset = 0;
                memo->list_count = list.count;
                memo->list_primitive = metal_primitive;
            } else {
                metal_indices = transient_alloc(b, (NSUInteger)list.count * 4, &metal_index_offset);
                memcpy((uint8_t *)mtl_contents(metal_indices) + metal_index_offset, list.data, (size_t)list.count * 4);
            }
            free(list.data);
        }
        if (memo && memo->list) metal_index_offset = 0;
        metal_index_type = MTLIndexTypeUInt32;
    } else if (!native) {
        uint32_t *seq = malloc((size_t)count * sizeof(uint32_t));
        if (!seq) return gpu_fallback("!seq");
        for (GLsizei i = 0; i < count; ++i)
            seq[i] = indexed ? read_gl_index(index_data, index_type, (uint32_t)i) : (uint32_t)(first + i);
        struct glm_index_list list = {0};
        uint32_t metal_primitive;
        glm_triangulate(mode, seq, (uint32_t)count, &list, &metal_primitive, false);
        free(seq);
        primitive = (MTLPrimitiveType)metal_primitive;
        draw_count = list.count;
        if (draw_count) {
            metal_indices = transient_alloc(b, (NSUInteger)draw_count * 4, &metal_index_offset);
            memcpy((uint8_t *)mtl_contents(metal_indices) + metal_index_offset, list.data, (size_t)draw_count * 4);
        }
        free(list.data);
        if (!draw_count) return true;
    } else if (indexed) {
        size_t size = gl_type_size(index_type);
        if (index_type == GL_UNSIGNED_BYTE) {
            metal_indices = transient_alloc(b, (NSUInteger)count * 2, &metal_index_offset);
            uint16_t *out = (uint16_t *)((uint8_t *)mtl_contents(metal_indices) + metal_index_offset);
            for (GLsizei i = 0; i < count; ++i) out[i] = index_data[i];
            metal_index_type = MTLIndexTypeUInt16;
        } else if (index_buffer && (uintptr_t)indices % 4 == 0) {
            metal_indices = (__bridge id<MTLBuffer>)index_buffer->backend;
            metal_index_offset = (uintptr_t)indices;
            metal_index_type = size == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
            note_gpu_read(ctx, index_buffer, (GLintptr)(uintptr_t)indices,
                          (GLintptr)((uintptr_t)indices + (size_t)count * size));
        } else {
            metal_indices = transient_alloc(b, (NSUInteger)count * size, &metal_index_offset);
            memcpy((uint8_t *)mtl_contents(metal_indices) + metal_index_offset, index_data, (size_t)count * size);
            metal_index_type = size == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
        }
    }

    vertex_id_base(b, (uint32_t)rebase);
    __unsafe_unretained id<MTLRenderCommandEncoder> e = prepare_draw(ctx, &st, (uint32_t)primitive, draw_count, (uint32_t)instances, layout,
                                                 layout_id, false);
    if (!e) return true;

    /* The hidden original-ID resource shares stream slot30. In the ordinary
       GPU path mapping is disabled, but Metal still requires a bound argument.
       A real attribute at slot30 replaces this harmless placeholder below. */
    if (slot_count < GLM_VERTEX_SLOTS) {
        static const uint32_t zero_ids[4] = {0};
        enc_bytes(b, STAGE_VERTEX, zero_ids, sizeof zero_ids, GLM_SLOT_STREAM);
    }
    /* Vertex buffers. */
    for (int k = 0; k < slot_count; ++k) {
        NSUInteger slot = GLM_SLOT_VERTEX0 + (NSUInteger)k;
        if (k == constant_slot) {
            enc_bytes(b, STAGE_VERTEX, constants, sizeof constants, slot);
            continue;
        }
        struct vertex_slot *v = &slots[k];
        int64_t base = v->divisor ? 0 : rebase;
        if (v->buffer) {
            __unsafe_unretained id<MTLBuffer> storage = (__bridge id<MTLBuffer>)v->buffer->backend;
            uint64_t binding = v->offset + (uint64_t)base * v->stride;
            enc_buffer(b, STAGE_VERTEX, storage, binding, slot);
            /* Reads through the highest vertex (or instance) used. */
            int64_t last = v->divisor ? (instances - 1) / v->divisor : high - base;
            note_gpu_read(ctx, v->buffer, (GLintptr)binding, (GLintptr)(binding + (uint64_t)last * v->stride + v->end));
        } else {
            size_t bytes = (size_t)(high - low) * v->stride + v->end;
            NSUInteger offset;
            __unsafe_unretained id<MTLBuffer> copy = transient_alloc(b, bytes, &offset);
            memcpy((uint8_t *)mtl_contents(copy) + offset, v->client + (size_t)low * v->stride, bytes);
            enc_buffer(b, STAGE_VERTEX, copy, offset, slot);
        }
    }

    if (metal_indices) {
        [e drawIndexedPrimitives:primitive indexCount:draw_count indexType:metal_index_type indexBuffer:metal_indices
               indexBufferOffset:metal_index_offset instanceCount:(NSUInteger)instances
                      baseVertex:index_base - (NSInteger)rebase baseInstance:0];
    } else {
        [e drawPrimitives:primitive vertexStart:(NSUInteger)(first - rebase) vertexCount:(NSUInteger)count
            instanceCount:(NSUInteger)instances baseInstance:0];
    }
    return true;
}

/* ---- CPU writes to buffers the GPU reads -------------------------------- */

void glm_backend_buffer_prepare_write(struct glm_context *ctx, struct glm_buffer *buffer, GLintptr offset,
                                      GLsizeiptr size, bool discard, bool wait)
{
    buffer->content_generation = next_content_generation();
    if (!buffer->backend || !buffer->gpu_read_count) return;
    struct glm_backend_context *reader = buffer->gpu_reader;
    if (reader == ctx->backend) prune_gpu_reads(buffer);
    uint64_t conflict = 0;
    for (int i = 0; i < buffer->gpu_read_count; ++i)
        if (offset < buffer->gpu_reads[i].high && offset + size > buffer->gpu_reads[i].low &&
            buffer->gpu_reads[i].serial > conflict)
            conflict = buffer->gpu_reads[i].serial;
    if (!conflict) return;
    if (wait && reader == ctx->backend && conflict != UINT64_MAX) {
        if (glm_backend_pending_serial(ctx) == conflict && ctx->backend->commands) glm_backend_flush(ctx, false);
        glm_backend_fence_wait(ctx, conflict);
        prune_gpu_reads(buffer);
        return;
    }
    /* Rename: new storage for the CPU; queued GPU work keeps the old. */
    id<MTLBuffer> old = (__bridge id<MTLBuffer>)buffer->backend;
    uint64_t started = glm_now_ns();
    id<MTLBuffer> fresh = pooled_storage(old.length);
    if (!discard) memcpy(mtl_contents(fresh), mtl_contents(old), MIN(old.length, fresh.length));
    glm_note_stall("buffer rename copy", started);
    glm_backend_buffer_retire(ctx, buffer);
    buffer->backend = (__bridge_retained void *)fresh;
    buffer->gpu_reader = NULL;
    buffer->gpu_read_count = 0;
    static uint64_t renames, renamed_bytes;
    renamed_bytes += old.length;
    ++renames;
    if ((renames & (renames - 1)) == 0)
        glm_log("buffer renames: %llu (%llu MB copied)", (unsigned long long)renames,
                (unsigned long long)(renamed_bytes >> 20));
}

/* Replaced or deleted storage: reused once this context's next command
   buffer completes (read only by it; another context's reads are unknown,
   so its storage is just released). */
void glm_backend_buffer_retire(struct glm_context *ctx, struct glm_buffer *buffer)
{
    if (!buffer->backend) return;
    id<MTLBuffer> storage = (__bridge_transfer id<MTLBuffer>)buffer->backend;
    buffer->backend = NULL;
    if (buffer->gpu_reader && buffer->gpu_reader != ctx->backend) return;
    for (int i = 0; i < buffer->gpu_read_count; ++i)
        if (buffer->gpu_reads[i].serial == UINT64_MAX) return;
    [ctx->backend->retiring addObject:storage];
}

/* ---- buffers ------------------------------------------------------------- */

void glm_backend_buffer_data(struct glm_context *ctx, struct glm_buffer *buffer, const void *data)
{
    (void)ctx;
    buffer->content_generation = next_content_generation();
    /* New storage each time: the old one stays alive for any command
       buffer still using it (GL orphaning semantics). */
    glm_backend_buffer_retire(ctx, buffer);
    NSUInteger length = buffer->size ? (NSUInteger)buffer->size : 1;
    id<MTLBuffer> storage = pooled_storage(length);
    if (data) memcpy(mtl_contents(storage), data, (size_t)buffer->size);
    buffer->backend = (__bridge_retained void *)storage;
    buffer->gpu_reader = NULL;
    buffer->gpu_read_count = 0;
}

/* Storage for glBufferData made on any thread (retained). */
void *glm_backend_new_buffer_storage(size_t size, const void *data)
{
    NSUInteger length = size ? (NSUInteger)size : 1;
    id<MTLBuffer> storage = pooled_storage(length);
    if (data) memcpy(mtl_contents(storage), data, size);
    return (__bridge_retained void *)storage;
}

void *glm_backend_storage_contents(void *storage) { return mtl_contents(((__bridge id<MTLBuffer>)storage)); }

void glm_backend_buffer_adopt(struct glm_context *ctx, struct glm_buffer *buffer, void *storage)
{
    buffer->content_generation = next_content_generation();
    glm_backend_buffer_retire(ctx, buffer);
    buffer->backend = storage;
    buffer->gpu_reader = NULL;
    buffer->gpu_read_count = 0;
}

void glm_backend_buffer_sub_data(struct glm_context *ctx, struct glm_buffer *buffer, GLintptr offset, GLsizeiptr size,
                                 const void *data)
{
    /* GPU work may still read the range: never block, rename instead. */
    glm_backend_buffer_prepare_write(ctx, buffer, offset, size, offset == 0 && size == buffer->size, false);
    id<MTLBuffer> storage = (__bridge id<MTLBuffer>)buffer->backend;
    if (!storage) return;
    memcpy((uint8_t *)mtl_contents(storage) + offset, data, (size_t)size);
}

void *glm_backend_buffer_contents(struct glm_context *ctx, struct glm_buffer *buffer, bool for_write)
{
    if (for_write) buffer->content_generation = next_content_generation();
    /* Written by transform feedback: the CPU sees it once the GPU is done. */
    struct glm_backend_context *writer = buffer->gpu_writer;
    if (writer && buffer->gpu_write_serial > __atomic_load_n(&writer->completed_serial, __ATOMIC_ACQUIRE)) {
        if (writer == ctx->backend) {
            if (glm_backend_pending_serial(ctx) == buffer->gpu_write_serial && ctx->backend->commands)
                glm_backend_flush(ctx, false);
            glm_backend_fence_wait(ctx, buffer->gpu_write_serial);
        }
    }
    __unsafe_unretained id<MTLBuffer> storage = (__bridge id<MTLBuffer>)buffer->backend;
    return storage ? mtl_contents(storage) : NULL;
}

/* ---- readback ------------------------------------------------------------ */

static id<MTLTexture> read_texture(struct glm_context *ctx, NSUInteger *slice, NSUInteger *level)
{
    *slice = *level = 0;
    struct glm_backend_context *b = ctx->backend;
    if (ctx->read_framebuffer == 0) return b->default_color;
    struct glm_framebuffer *fb = glm_framebuffer_get(ctx, ctx->read_framebuffer);
    GLenum buffer = fb->read_buffer;
    if (buffer < GL_COLOR_ATTACHMENT0 || buffer >= GL_COLOR_ATTACHMENT0 + GLM_MAX_DRAW_BUFFERS) return nil;
    return attachment_texture(ctx, &fb->color[buffer - GL_COLOR_ATTACHMENT0], slice, level);
}

/* Copies a region of `source` into a shared buffer as RGBA float (or the
   raw bytes of an RGBA8 texture when `raw8` is set). */
static id<MTLBuffer> download(struct glm_backend_context *b, id<MTLTexture> source, NSUInteger slice, NSUInteger level,
                              MTLRegion region, NSUInteger bytes_per_row)
{
    NSUInteger length = bytes_per_row * region.size.height;
    id<MTLBuffer> staging = glm_new_shared_buffer(length, NULL);
    id<MTLCommandBuffer> commands = command_buffer(b);
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:source sourceSlice:slice sourceLevel:level sourceOrigin:region.origin sourceSize:region.size
                 toBuffer:staging destinationOffset:0 destinationBytesPerRow:bytes_per_row
    destinationBytesPerImage:length];
    [blit endEncoding];
    return staging;
}

GLM_HIDDEN void glm_unpack_texels(uint32_t metal_format, const uint8_t *src, size_t count, float *rgba);
GLM_HIDDEN bool glm_unpack_texels_int(uint32_t metal_format, const uint8_t *src, size_t count, int64_t *rgba);
GLM_HIDDEN bool glm_decode_blocks(uint32_t f, const uint8_t *src, size_t row_bytes, size_t width, size_t height, float *rgba);
GLM_HIDDEN bool glm_convert_pack_int(struct glm_context *ctx, const int64_t *rgba, GLsizei width, GLsizei height,
                                     GLenum format, GLenum type, void *out);
GLM_HIDDEN size_t glm_metal_format_bytes(uint32_t metal_format);
GLM_HIDDEN size_t glm_pixel_bytes(GLenum format, GLenum type);

/* The read framebuffer's depth (or stencil) image; nil without one. */
static id<MTLTexture> read_depth_texture(struct glm_context *ctx, bool stencil, NSUInteger *slice, NSUInteger *level)
{
    *slice = *level = 0;
    if (ctx->read_framebuffer == 0) {
        if (ctx->default_bits_known && !(stencil ? ctx->default_stencil_bits : ctx->default_depth_bits)) return nil;
        return ctx->backend->default_depth;
    }
    struct glm_framebuffer *fb = glm_framebuffer_get(ctx, ctx->read_framebuffer);
    return fb ? attachment_texture(ctx, stencil ? &fb->stencil : &fb->depth, slice, level) : nil;
}

/* Copies a region of the depth or stencil aspect of `source` into a shared
   buffer of floats (depth) or bytes (stencil). */
static id<MTLBuffer> download_aspect(struct glm_backend_context *b, id<MTLTexture> source, NSUInteger slice,
                                     NSUInteger level, MTLRegion region, bool stencil, NSUInteger *bytes_per_row)
{
    MTLPixelFormat f = source.pixelFormat;
    bool combined = f == MTLPixelFormatDepth32Float_Stencil8;
    NSUInteger texel = stencil ? 1 : f == MTLPixelFormatDepth16Unorm ? 2 : 4;
    /* Rows 256-byte aligned: depth and stencil copies to buffers lose texels
       past the first few with tight rows (measured with Stencil8). */
    *bytes_per_row = (region.size.width * texel + 255) & ~(NSUInteger)255;
    NSUInteger length = *bytes_per_row * region.size.height;
    id<MTLBuffer> staging = glm_new_shared_buffer(length, NULL);
    id<MTLBlitCommandEncoder> blit = [command_buffer(b) blitCommandEncoder];
    MTLBlitOption options = !combined ? MTLBlitOptionNone
                            : stencil ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil;
    [blit copyFromTexture:source sourceSlice:slice sourceLevel:level sourceOrigin:region.origin sourceSize:region.size
                 toBuffer:staging destinationOffset:0 destinationBytesPerRow:*bytes_per_row
    destinationBytesPerImage:length options:options];
    [blit endEncoding];
    return staging;
}

/* glReadPixels of GL_DEPTH_COMPONENT, GL_STENCIL_INDEX or GL_DEPTH_STENCIL:
   depth goes through the pack as the red channel, stencil as green. */
static void read_depth_stencil(struct glm_context *ctx, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                               GLenum type, void *pixels)
{
    struct glm_backend_context *b = ctx->backend;
    bool want_depth = format != GL_STENCIL_INDEX, want_stencil = format != GL_DEPTH_COMPONENT;
    NSUInteger dslice, dlevel, sslice, slevel;
    id<MTLTexture> depth = want_depth ? read_depth_texture(ctx, false, &dslice, &dlevel) : nil;
    id<MTLTexture> stencil = want_stencil ? read_depth_texture(ctx, true, &sslice, &slevel) : nil;
    if ((want_depth && !depth) || (want_stencil && !stencil)) return glm_error(ctx, GL_INVALID_OPERATION);
    if ((depth && depth.sampleCount > 1) || (stencil && stencil.sampleCount > 1)) return glm_error(ctx, GL_INVALID_OPERATION);
    id<MTLTexture> any = depth ? depth : stencil;
    NSUInteger level = depth ? dlevel : slevel;
    NSInteger tw = (NSInteger)MAX(any.width >> level, 1u), th = (NSInteger)MAX(any.height >> level, 1u);
    NSInteger x0 = MAX(x, 0), y0 = MAX(y, 0), x1 = MIN((NSInteger)x + width, tw), y1 = MIN((NSInteger)y + height, th);
    if (x1 <= x0 || y1 <= y0) return;
    end_encoder(b);
    MTLRegion region = MTLRegionMake2D((NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0));
    NSUInteger depth_row = 0, stencil_row = 0;
    id<MTLBuffer> depth_data = depth ? download_aspect(b, depth, dslice, dlevel, region, false, &depth_row) : nil;
    id<MTLBuffer> stencil_data = stencil ? download_aspect(b, stencil, sslice, slevel, region, true, &stencil_row) : nil;
    glm_backend_flush(ctx, true);
    /* Depth as Apple reads it back: 24 bits for its 16- and 24-bit
       formats (GLMetal stores 32-bit floats). */
    int depth_bits = 0;
    if (depth) {
        struct glm_framebuffer *fb = ctx->read_framebuffer ? glm_framebuffer_get(ctx, ctx->read_framebuffer) : NULL;
        if (!fb) {
            depth_bits = ctx->default_bits_known ? ctx->default_depth_bits : 32;
        } else if (fb->depth.type == GL_RENDERBUFFER) {
            struct glm_renderbuffer *rb = glm_renderbuffer_get(ctx, fb->depth.name);
            depth_bits = rb ? rb->bits[4] : 0;
        } else if (fb->depth.type == GL_TEXTURE) {
            struct glm_texture *t = glm_texture_get(ctx, fb->depth.name);
            const struct glm_apple_texture *ref =
                t && fb->depth.level >= 0 && fb->depth.level < GLM_MAX_LEVELS
                    ? glm_apple_texture_format(ctx, t->levels[0][fb->depth.level].internal_format) : NULL;
            depth_bits = ref ? ref->size[6] : 0;
        }
        /* 32-bit formats keep their floats. */
        depth_bits = depth_bits == 16 || depth_bits == 24 ? 24 : 0;
    }
    float *values = calloc((size_t)width * height * 4, sizeof(float));
    for (NSUInteger row = 0; row < region.size.height; ++row)
        for (NSUInteger col = 0; col < region.size.width; ++col) {
            float *dst = values + (((size_t)(y0 - y + (NSInteger)row) * (size_t)width) + (size_t)(x0 - x) + col) * 4;
            if (depth_data) {
                const uint8_t *src = (const uint8_t *)mtl_contents(depth_data) + row * depth_row;
                if (depth.pixelFormat == MTLPixelFormatDepth16Unorm) {
                    uint16_t v;
                    memcpy(&v, src + col * 2, 2);
                    dst[0] = v / 65535.0f;
                } else {
                    memcpy(&dst[0], src + col * 4, 4);
                    if (depth_bits > 0 && depth_bits < 32) {
                        double scale = (double)((1u << depth_bits) - 1);
                        double d = dst[0] < 0 ? 0 : dst[0] > 1 ? 1 : dst[0];
                        dst[0] = (float)(floor(d * scale + 0.5) / scale);
                    }
                }
            }
            if (stencil_data) dst[1] = ((const uint8_t *)mtl_contents(stencil_data))[row * stencil_row + col];
        }
    if (!glm_convert_pack(ctx, values, width, height, format, type, pixels)) glm_error(ctx, GL_INVALID_ENUM);
    free(values);
}

void glm_backend_read_pixels(struct glm_context *ctx, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                             GLenum type, void *pixels)
{
    struct glm_backend_context *b = ctx->backend;
    if (format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX || format == GL_DEPTH_STENCIL)
        return read_depth_stencil(ctx, x, y, width, height, format, type, pixels);
    NSUInteger slice, level;
    id<MTLTexture> source = read_texture(ctx, &slice, &level);
    if (!source) return glm_error(ctx, GL_INVALID_OPERATION);
    NSInteger tw = (NSInteger)MAX(source.width >> level, 1u), th = (NSInteger)MAX(source.height >> level, 1u);
    /* Pixels outside the framebuffer are undefined in GL; read the
       intersection and leave the rest untouched. */
    NSInteger x0 = MAX(x, 0), y0 = MAX(y, 0), x1 = MIN((NSInteger)x + width, tw), y1 = MIN((NSInteger)y + height, th);
    if (x1 <= x0 || y1 <= y0) return;
    end_encoder(b);
    id<MTLTexture> readable = source;
    if (source.sampleCount > 1) return glm_error(ctx, GL_INVALID_OPERATION);
    size_t texel = glm_metal_format_bytes((uint32_t)readable.pixelFormat);
    MTLRegion region = MTLRegionMake2D((NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0));
    NSUInteger bytes_per_row = region.size.width * texel;
    id<MTLBuffer> staging = download(b, readable, slice, level, region, bytes_per_row);
    glm_backend_flush(ctx, true);
    if (glm_unpack_texels_int((uint32_t)readable.pixelFormat, NULL, 0, NULL)) {
        /* Integer buffers: exact through 64-bit integers. */
        int64_t *values = calloc((size_t)width * height * 4, sizeof(int64_t));
        for (NSUInteger row = 0; row < region.size.height; ++row)
            glm_unpack_texels_int((uint32_t)readable.pixelFormat, (const uint8_t *)mtl_contents(staging) + row * bytes_per_row,
                                  region.size.width, values + (((size_t)(y0 - y + (NSInteger)row) * (size_t)width) + (size_t)(x0 - x)) * 4);
        if (!glm_convert_pack_int(ctx, values, width, height, format, type, pixels)) glm_error(ctx, GL_INVALID_ENUM);
        free(values);
        return;
    }
    float *rgba = malloc((size_t)width * height * 4 * sizeof(float));
    memset(rgba, 0, (size_t)width * height * 4 * sizeof(float));
    for (NSUInteger row = 0; row < region.size.height; ++row) {
        float *dst = rgba + (((size_t)(y0 - y + (NSInteger)row) * (size_t)width) + (size_t)(x0 - x)) * 4;
        glm_unpack_texels((uint32_t)readable.pixelFormat, (const uint8_t *)mtl_contents(staging) + row * bytes_per_row,
                          region.size.width, dst);
    }
    if (!glm_convert_pack(ctx, rgba, width, height, format, type, pixels)) glm_error(ctx, GL_INVALID_ENUM);
    free(rgba);
}

/* ---- framebuffer blits --------------------------------------------------- */

/* Shader blits: a triangle over the destination rectangle sampling the
   source rectangle (scaled, flipped, converting, or resolving). */
static NSString *const blit_source =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct BlitOut { float4 position [[position]]; float2 uv; };\n"
     "struct BlitArgs { float4 source; };\n" /* x0, y0, x1, y1 in normalized source coordinates */
     "vertex BlitOut blit_vertex(uint vid [[vertex_id]], constant BlitArgs &a [[buffer(0)]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
     "  BlitOut o; o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
     /* Viewport row 0 (NDC y = +1) is the rectangle's first GL row. */
     "  o.uv = float2(mix(a.source.x, a.source.z, p.x), mix(a.source.y, a.source.w, 1.0 - p.y)); return o; }\n"
     "fragment float4 blit_color(BlitOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "  return t.sample(s, in.uv); }\n"
     "fragment float4 blit_color_ms(BlitOut in [[stage_in]], texture2d_ms<float> t [[texture(0)]]) {\n"
     "  uint2 c = uint2(clamp(in.uv * float2(t.get_width(), t.get_height()), float2(0.0),\n"
     "                        float2(t.get_width() - 1, t.get_height() - 1)));\n"
     "  float4 sum = float4(0.0); uint n = t.get_num_samples();\n"
     "  for (uint i = 0; i < n; ++i) sum += t.read(c, i);\n"
     "  return sum / float(n); }\n"
     "struct DepthOut { float depth [[depth(any)]]; };\n"
     "fragment DepthOut blit_depth(BlitOut in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "  DepthOut o; o.depth = t.sample(s, in.uv); return o; }\n"
     "fragment DepthOut blit_depth_ms(BlitOut in [[stage_in]], depth2d_ms<float> t [[texture(0)]],\n"
     "                                uint sample_id [[sample_id]]) {\n"
     "  uint2 c = uint2(clamp(in.uv * float2(t.get_width(), t.get_height()), float2(0.0),\n"
     "                        float2(t.get_width() - 1, t.get_height() - 1)));\n"
     /* Single-sample targets select sample zero. Equal-sample targets keep
        corresponding depth samples instead of averaging depth values. */
     "  DepthOut o; o.depth = t.read(c, sample_id); return o; }\n"
     "struct StencilOut { uint stencil [[stencil]]; };\n"
     "struct DepthStencilOut { float depth [[depth(any)]]; uint stencil [[stencil]]; };\n"
     "fragment StencilOut blit_stencil(BlitOut in [[stage_in]], texture2d<uint> t [[texture(1)]]) {\n"
     "  uint2 c = uint2(clamp(in.uv * float2(t.get_width(), t.get_height()), float2(0),\n"
     "                        float2(t.get_width()-1, t.get_height()-1)));\n"
     "  StencilOut o; o.stencil = t.read(c).r; return o; }\n"
     "fragment StencilOut blit_stencil_ms(BlitOut in [[stage_in]], texture2d_ms<uint> t [[texture(1)]],\n"
     "                                  uint sample_id [[sample_id]]) {\n"
     "  uint2 c = uint2(clamp(in.uv * float2(t.get_width(), t.get_height()), float2(0),\n"
     "                        float2(t.get_width()-1, t.get_height()-1)));\n"
     "  StencilOut o; o.stencil = t.read(c, sample_id).r; return o; }\n"
     "fragment DepthStencilOut blit_depth_stencil(BlitOut in [[stage_in]], depth2d<float> d [[texture(0)]],\n"
     "                                           texture2d<uint> t [[texture(1)]]) {\n"
     "  uint2 c = uint2(clamp(in.uv * float2(t.get_width(), t.get_height()), float2(0),\n"
     "                        float2(t.get_width()-1, t.get_height()-1)));\n"
     "  DepthStencilOut o; o.depth = d.read(c); o.stencil = t.read(c).r; return o; }\n"
     "fragment DepthStencilOut blit_depth_stencil_ms(BlitOut in [[stage_in]], depth2d_ms<float> d [[texture(0)]],\n"
     "                      texture2d_ms<uint> t [[texture(1)]], uint sample_id [[sample_id]]) {\n"
     "  uint2 c = uint2(clamp(in.uv * float2(t.get_width(), t.get_height()), float2(0),\n"
     "                        float2(t.get_width()-1, t.get_height()-1)));\n"
     "  DepthStencilOut o; o.depth = d.read(c,sample_id); o.stencil = t.read(c,sample_id).r; return o; }\n";

static id<MTLRenderPipelineState> blit_pipeline(struct glm_backend_context *b, NSString *fragment_name,
                                                MTLPixelFormat format, bool depth, NSUInteger samples)
{
    static NSMutableDictionary<NSString *, id<MTLRenderPipelineState>> *cache;
    static id<MTLLibrary> library;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    NSString *key = [NSString stringWithFormat:@"%@/%lu/%d/%lu", fragment_name, (unsigned long)format, depth,
                                               (unsigned long)samples];
    pthread_mutex_lock(&lock);
    if (!cache) {
        cache = [NSMutableDictionary dictionary];
        NSError *error = nil;
        MTLCompileOptions *options = [MTLCompileOptions new];
        /* Apple's byte-UNORM resolve accumulates samples in float32 index
           order. Reassociation changes exact half-byte rounding. */
        options.fastMathEnabled = NO;
        library = [device newLibraryWithSource:blit_source options:options error:&error];
        if (!library) glm_log("blit library: %s", error.localizedDescription.UTF8String);
    }
    id<MTLRenderPipelineState> state = cache[key];
    if (!state && library) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [library newFunctionWithName:@"blit_vertex"];
        d.fragmentFunction = [library newFunctionWithName:fragment_name];
        if (depth) {
            if (format != MTLPixelFormatStencil8) d.depthAttachmentPixelFormat = format;
            if (format == MTLPixelFormatDepth32Float_Stencil8 || format == MTLPixelFormatDepth24Unorm_Stencil8 ||
                format == MTLPixelFormatStencil8)
                d.stencilAttachmentPixelFormat = format;
        } else {
            d.colorAttachments[0].pixelFormat = format;
        }
        d.rasterSampleCount = samples;
        NSError *error = nil;
        state = [device newRenderPipelineStateWithDescriptor:d error:&error];
        if (!state) glm_log("blit pipeline: %s", error.localizedDescription.UTF8String);
        else cache[key] = state;
    }
    pthread_mutex_unlock(&lock);
    (void)b;
    return state;
}

/* Blits one attachment: `src`/`dst` are GL rectangles (x0, y0, x1, y1). */
static void blit_texture(struct glm_context *ctx, id<MTLTexture> source, NSUInteger sslice, NSUInteger slevel,
                         id<MTLTexture> target, NSUInteger dslice, NSUInteger dlevel, const GLint src[4], const GLint dst[4],
                         GLbitfield aspects, GLenum filter, bool obey_scissor)
{
    struct glm_backend_context *b = ctx->backend;
    const struct glm_state *s = &ctx->state;
    bool depth = aspects != 0, write_depth = (aspects & GL_DEPTH_BUFFER_BIT) != 0;
    bool write_stencil = (aspects & GL_STENCIL_BUFFER_BIT) != 0;
    bool scissor = obey_scissor && s->scissor_test;
    bool combined = target.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ||
                    target.pixelFormat == MTLPixelFormatDepth24Unorm_Stencil8;
    /* A raw combined-format copy writes both planes. A one-plane blit must
       preserve the destination's other plane, even for identical rectangles. */
    bool full_copy = !combined || (write_depth && write_stencil);
    /* Normalize the destination to increasing coordinates; flips move to
       the source. */
    GLint sx0 = src[0], sy0 = src[1], sx1 = src[2], sy1 = src[3];
    GLint dx0 = dst[0], dy0 = dst[1], dx1 = dst[2], dy1 = dst[3];
    if (dx1 < dx0) { GLint t = dx0; dx0 = dx1; dx1 = t; t = sx0; sx0 = sx1; sx1 = t; }
    if (dy1 < dy0) { GLint t = dy0; dy0 = dy1; dy1 = t; t = sy0; sy0 = sy1; sy1 = t; }
    if (dx1 <= dx0 || dy1 <= dy0 || sx0 == sx1 || sy0 == sy1) return;
    NSUInteger sw = MAX(source.width >> slevel, 1u), sh = MAX(source.height >> slevel, 1u);
    NSUInteger tw = MAX(target.width >> dlevel, 1u), th = MAX(target.height >> dlevel, 1u);
    /* A flipped rectangle has the same extent, even though its signed
       source span is negative after destination normalization. */
    bool same_size = llabs((long long)sx1 - sx0) == (long long)dx1 - dx0 &&
                     llabs((long long)sy1 - sy0) == (long long)dy1 - dy0;
    bool inside = sx0 >= 0 && sy0 >= 0 && sx1 <= (GLint)sw && sy1 <= (GLint)sh && dx0 >= 0 && dy0 >= 0 &&
                  dx1 <= (GLint)tw && dy1 <= (GLint)th;
    end_encoder(b);
    b->framebuffer_dirty = true;
    if (same_size && sx1 > sx0 && sy1 > sy0 && inside && !scissor && source.pixelFormat == target.pixelFormat &&
        source.sampleCount == target.sampleCount && full_copy) {
        id<MTLBlitCommandEncoder> blit = [command_buffer(b) blitCommandEncoder];
        [blit copyFromTexture:source sourceSlice:sslice sourceLevel:slevel
                 sourceOrigin:MTLOriginMake((NSUInteger)sx0, (NSUInteger)sy0, 0)
                   sourceSize:MTLSizeMake((NSUInteger)(sx1 - sx0), (NSUInteger)(sy1 - sy0), 1) toTexture:target
             destinationSlice:dslice destinationLevel:dlevel destinationOrigin:MTLOriginMake((NSUInteger)dx0, (NSUInteger)dy0, 0)];
        [blit endEncoding];
        return;
    }
    if (!depth && same_size && source.sampleCount > 1 && target.sampleCount == 1 && sx0 == 0 && sy0 == 0 &&
        dx0 == 0 && dy0 == 0 && (NSUInteger)sx1 == sw && (NSUInteger)sy1 == sh && (NSUInteger)dx1 == tw &&
        (NSUInteger)dy1 == th && !scissor && source.pixelFormat == target.pixelFormat &&
        source.pixelFormat != MTLPixelFormatRGBA8Unorm && source.pixelFormat != MTLPixelFormatBGRA8Unorm) {
        /* Linear byte-UNORM uses the ordered shader resolve below. Other
           formats retain their format-specific hardware resolve. */
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = source;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        pass.colorAttachments[0].resolveTexture = target;
        pass.colorAttachments[0].resolveLevel = dlevel;
        pass.colorAttachments[0].resolveSlice = dslice;
        [[command_buffer(b) renderCommandEncoderWithDescriptor:pass] endEncoding];
        return;
    }
    if (target.sampleCount > 1 && source.sampleCount > 1 && !same_size) return; /* GL_INVALID_OPERATION in GL */
    id<MTLTexture> view = source;
    if (source.textureType != MTLTextureType2D && source.textureType != MTLTextureType2DMultisample)
        view = [source newTextureViewWithPixelFormat:source.pixelFormat
                                        textureType:source.sampleCount > 1 ? MTLTextureType2DMultisample : MTLTextureType2D
                                              levels:NSMakeRange(slevel, 1) slices:NSMakeRange(sslice, 1)];
    else if (slevel && source.sampleCount == 1)
        view = [source newTextureViewWithPixelFormat:source.pixelFormat textureType:MTLTextureType2D
                                              levels:NSMakeRange(slevel, 1) slices:NSMakeRange(0, 1)];
    id<MTLTexture> stencil_view = nil;
    if (write_stencil) {
        MTLPixelFormat sf = source.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLPixelFormatX32_Stencil8 :
                            source.pixelFormat == MTLPixelFormatDepth24Unorm_Stencil8 ? MTLPixelFormatX24_Stencil8 :
                            source.pixelFormat;
        stencil_view = [view newTextureViewWithPixelFormat:sf];
        if (!stencil_view) return;
    }
    NSString *name = write_depth && write_stencil ? @"blit_depth_stencil" : write_stencil ? @"blit_stencil" :
                     write_depth ? @"blit_depth" : @"blit_color";
    NSString *fragment = source.sampleCount > 1 ? [name stringByAppendingString:@"_ms"] : name;
    id<MTLRenderPipelineState> pipeline = blit_pipeline(b, fragment, target.pixelFormat, depth, target.sampleCount);
    if (!pipeline) return;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (depth) {
        if (target.pixelFormat != MTLPixelFormatStencil8) {
            pass.depthAttachment.texture = target;
            pass.depthAttachment.level = dlevel;
            pass.depthAttachment.slice = dslice;
            pass.depthAttachment.loadAction = MTLLoadActionLoad;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
        }
        if (combined || target.pixelFormat == MTLPixelFormatStencil8) {
            /* Preserve whichever plane the requested mask does not write. */
            pass.stencilAttachment.texture = target;
            pass.stencilAttachment.level = dlevel;
            pass.stencilAttachment.slice = dslice;
            pass.stencilAttachment.loadAction = MTLLoadActionLoad;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
        }
    } else {
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].level = dlevel;
        pass.colorAttachments[0].slice = dslice;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    id<MTLRenderCommandEncoder> e = [command_buffer(b) renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:pipeline];
    if (depth) {
        MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = MTLCompareFunctionAlways;
        d.depthWriteEnabled = write_depth;
        if (write_stencil) {
            MTLStencilDescriptor *st = [MTLStencilDescriptor new];
            st.stencilCompareFunction = MTLCompareFunctionAlways;
            st.stencilFailureOperation = st.depthFailureOperation = st.depthStencilPassOperation = MTLStencilOperationReplace;
            st.readMask = st.writeMask = 0xff;
            d.frontFaceStencil = d.backFaceStencil = st;
        }
        [e setDepthStencilState:[device newDepthStencilStateWithDescriptor:d]];
    }
    [e setViewport:(MTLViewport){(double)dx0, (double)dy0, (double)(dx1 - dx0), (double)(dy1 - dy0), 0, 1}];
    NSInteger cx0 = MAX(dx0, 0), cy0 = MAX(dy0, 0), cx1 = MIN(dx1, (GLint)tw), cy1 = MIN(dy1, (GLint)th);
    if (scissor) {
        cx0 = MAX(cx0, s->scissor[0]);
        cy0 = MAX(cy0, s->scissor[1]);
        cx1 = MIN(cx1, (NSInteger)s->scissor[0] + s->scissor[2]);
        cy1 = MIN(cy1, (NSInteger)s->scissor[1] + s->scissor[3]);
    }
    if (cx1 <= cx0 || cy1 <= cy0) {
        [e endEncoding];
        return;
    }
    [e setScissorRect:(MTLScissorRect){(NSUInteger)cx0, (NSUInteger)cy0, (NSUInteger)(cx1 - cx0), (NSUInteger)(cy1 - cy0)}];
    float args[4] = {(float)sx0 / (float)sw, (float)sy0 / (float)sh, (float)sx1 / (float)sw, (float)sy1 / (float)sh};
    [e setVertexBytes:args length:sizeof args atIndex:0];
    [e setFragmentTexture:view atIndex:0];
    if (stencil_view) [e setFragmentTexture:stencil_view atIndex:1];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = filter == GL_LINEAR && !depth ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    static id<MTLSamplerState> samplers[2];
    int which = sd.minFilter == MTLSamplerMinMagFilterLinear;
    if (!samplers[which]) samplers[which] = [device newSamplerStateWithDescriptor:sd];
    [e setFragmentSamplerState:samplers[which] atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

void glm_backend_blit_framebuffer(struct glm_context *ctx, GLint src[4], GLint dst[4], GLbitfield mask, GLenum filter)
{
    struct glm_backend_context *b = ctx->backend;
    struct glm_framebuffer *draw = ctx->draw_framebuffer ? glm_framebuffer_get(ctx, ctx->draw_framebuffer) : NULL;
    struct glm_framebuffer *read = ctx->read_framebuffer ? glm_framebuffer_get(ctx, ctx->read_framebuffer) : NULL;
    if (mask & GL_COLOR_BUFFER_BIT) {
        NSUInteger sslice, slevel;
        id<MTLTexture> source = read_texture(ctx, &sslice, &slevel);
        if (tracing())
            fprintf(stderr, "TRACE ctx %p blit read fb %u (rb %#x) -> draw fb %u (db0 %#x) src %d,%d,%d,%d dst %d,%d,%d,%d mask %#x filter %#x source %p %lux%lu fmt %d\n",
                    (void *)ctx, ctx->read_framebuffer, read ? read->read_buffer : 0, ctx->draw_framebuffer, draw ? draw->draw_buffers[0] : 0,
                    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], mask, filter, (__bridge void *)source,
                    (unsigned long)source.width, (unsigned long)source.height, (int)source.pixelFormat);
        /* Apple's blits copy sRGB buffers raw (no decode or encode) whether
           or not GL_FRAMEBUFFER_SRGB is enabled. */
        if (source) source = linear_view(b, source);
        if (source) {
            /* Every enabled draw buffer receives the read buffer. */
            for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) {
                id<MTLTexture> target = nil;
                NSUInteger dslice = 0, dlevel = 0;
                if (!draw) {
                    if (i == 0) target = b->default_color;
                } else {
                    GLenum buffer = draw->draw_buffers[i];
                    if (buffer >= GL_COLOR_ATTACHMENT0 && buffer < GL_COLOR_ATTACHMENT0 + GLM_MAX_DRAW_BUFFERS)
                        target = attachment_texture(ctx, &draw->color[buffer - GL_COLOR_ATTACHMENT0], &dslice, &dlevel);
                }
                if (target) blit_texture(ctx, source, sslice, slevel, linear_view(b, target), dslice, dlevel, src, dst, 0, filter, true);
            }
        }
    }
    for (int aspect = 0; aspect < 2; ++aspect) {
        GLbitfield bits = aspect ? GL_STENCIL_BUFFER_BIT : GL_DEPTH_BUFFER_BIT;
        if (!(mask & bits)) continue;
        NSUInteger sslice = 0, slevel = 0, dslice = 0, dlevel = 0;
        id<MTLTexture> source = read ? attachment_texture(ctx, aspect ? &read->stencil : &read->depth, &sslice, &slevel) : b->default_depth;
        id<MTLTexture> target = draw ? attachment_texture(ctx, aspect ? &draw->stencil : &draw->depth, &dslice, &dlevel) : b->default_depth;
        if (!source || !target) continue;
        if (!aspect && (mask & GL_STENCIL_BUFFER_BIT)) {
            NSUInteger ss = 0, sl = 0, ds = 0, dl = 0;
            id<MTLTexture> st = read ? attachment_texture(ctx, &read->stencil, &ss, &sl) : b->default_depth;
            id<MTLTexture> dt = draw ? attachment_texture(ctx, &draw->stencil, &ds, &dl) : b->default_depth;
            if (st == source && dt == target && ss == sslice && sl == slevel && ds == dslice && dl == dlevel) {
                bits |= GL_STENCIL_BUFFER_BIT;
                mask &= ~GL_STENCIL_BUFFER_BIT;
            }
        }
        if (source == target && sslice == dslice && slevel == dlevel) {
            /* A temporary image avoids sampling an attachment being written. */
            GLint w = abs(src[2] - src[0]), h = abs(src[3] - src[1]);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat
                                                                                         width:(NSUInteger)MAX(w, 1)
                                                                                        height:(NSUInteger)MAX(h, 1)
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
            d.storageMode = MTLStorageModePrivate;
            d.sampleCount = source.sampleCount;
            if (d.sampleCount > 1) d.textureType = MTLTextureType2DMultisample;
            id<MTLTexture> copy = [device newTextureWithDescriptor:d];
            GLint whole[4] = {0, 0, w, h};
            GLint from[4] = {MIN(src[0], src[2]), MIN(src[1], src[3]), MAX(src[0], src[2]), MAX(src[1], src[3])};
            GLint flipped[4] = {src[2] < src[0] ? w : 0, src[3] < src[1] ? h : 0, src[2] < src[0] ? 0 : w, src[3] < src[1] ? 0 : h};
            if (copy && w > 0 && h > 0) {
                blit_texture(ctx, source, sslice, slevel, copy, 0, 0, from, whole, bits, GL_NEAREST, false);
                blit_texture(ctx, copy, 0, 0, target, dslice, dlevel, flipped, dst, bits, GL_NEAREST, true);
            }
        } else {
            blit_texture(ctx, source, sslice, slevel, target, dslice, dlevel, src, dst, bits, GL_NEAREST, true);
        }
    }
}

/* ---- copies into textures ------------------------------------------------ */

void glm_backend_copy_framebuffer_to_texture(struct glm_context *ctx, struct glm_texture *texture, int face, GLint level,
                                             GLint dst_x, GLint dst_y, GLint dst_z, GLint src_x, GLint src_y, GLsizei width,
                                             GLsizei height)
{
    struct glm_backend_context *b = ctx->backend;
    NSUInteger slice = 0, source_level = 0;
    struct glm_format_info target_info;
    bool known_format = glm_format_lookup(texture->levels[face][level].internal_format, &target_info);
    GLbitfield aspects = known_format ? (target_info.depth ? GL_DEPTH_BUFFER_BIT : 0) |
                                       (target_info.stencil ? GL_STENCIL_BUFFER_BIT : 0) : 0;
    id<MTLTexture> source = aspects ? read_depth_texture(ctx, !(aspects & GL_DEPTH_BUFFER_BIT), &slice, &source_level)
                                  : read_texture(ctx, &slice, &source_level);
    id<MTLTexture> target = (__bridge id<MTLTexture>)texture->backend;
    if (!source || !target || width <= 0 || height <= 0) return;
    /* Clip to the source; GL leaves texels outside it undefined. */
    NSInteger sw = (NSInteger)MAX(source.width >> source_level, 1u), sh = (NSInteger)MAX(source.height >> source_level, 1u);
    if (src_x < 0) { dst_x -= src_x; width += src_x; src_x = 0; }
    if (src_y < 0) { dst_y -= src_y; height += src_y; src_y = 0; }
    if (src_x + width > sw) width = (GLsizei)(sw - src_x);
    if (src_y + height > sh) height = (GLsizei)(sh - src_y);
    if (width <= 0 || height <= 0) return;
    if (aspects) {
        GLint src[4] = {src_x, src_y, src_x + width, src_y + height};
        GLint dst[4] = {dst_x, dst_y, dst_x + width, dst_y + height};
        NSUInteger dslice = target.textureType == MTLTextureType2DArray || target.textureType == MTLTextureTypeCubeArray
                               ? (NSUInteger)dst_z : (NSUInteger)face;
        /* Separate framebuffer attachments may supply the two planes. */
        for (int aspect = 0; aspect < 2; ++aspect) {
            GLbitfield bit = aspect ? GL_STENCIL_BUFFER_BIT : GL_DEPTH_BUFFER_BIT;
            if (!(aspects & bit)) continue;
            source = read_depth_texture(ctx, aspect != 0, &slice, &source_level);
            if (source) blit_texture(ctx, source, slice, source_level, target, dslice, (NSUInteger)level,
                                     src, dst, bit, GL_NEAREST, false);
        }
        return;
    }
    end_encoder(b);
    b->framebuffer_dirty = true;
    if (source.pixelFormat == target.pixelFormat && source.sampleCount == 1) {
        id<MTLBlitCommandEncoder> blit = [command_buffer(b) blitCommandEncoder];
        /* 3D textures take the layer as a depth; arrays as a slice. */
        bool volume = target.textureType == MTLTextureType3D;
        bool layers = target.textureType == MTLTextureType2DArray || target.textureType == MTLTextureTypeCubeArray;
        [blit copyFromTexture:source sourceSlice:slice sourceLevel:source_level
                 sourceOrigin:MTLOriginMake((NSUInteger)src_x, (NSUInteger)src_y, 0)
                   sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1) toTexture:target
             destinationSlice:layers ? (NSUInteger)dst_z : volume ? 0 : (NSUInteger)face destinationLevel:(NSUInteger)level
            destinationOrigin:MTLOriginMake((NSUInteger)dst_x, (NSUInteger)dst_y, volume ? (NSUInteger)dst_z : 0)];
        [blit endEncoding];
        return;
    }
    /* Different formats: convert through the CPU for now. */
    size_t texel = glm_metal_format_bytes((uint32_t)source.pixelFormat);
    MTLRegion region = MTLRegionMake2D((NSUInteger)src_x, (NSUInteger)src_y, (NSUInteger)width, (NSUInteger)height);
    id<MTLBuffer> staging = download(b, source, slice, source_level, region, region.size.width * texel);
    glm_backend_flush(ctx, true);
    float *rgba = malloc((size_t)width * height * 16);
    for (GLsizei row = 0; row < height; ++row)
        glm_unpack_texels((uint32_t)source.pixelFormat, (const uint8_t *)mtl_contents(staging) + (size_t)row * region.size.width * texel,
                          (size_t)width, rgba + (size_t)row * width * 4);
    struct glm_format_info info;
    if (glm_format_lookup(texture->levels[face][level].internal_format, &info)) {
        GLint saved_alignment = ctx->state.unpack_alignment, saved_row = ctx->state.unpack_row_length;
        GLint saved_skip_rows = ctx->state.unpack_skip_rows, saved_skip_pixels = ctx->state.unpack_skip_pixels;
        GLint saved_row_bytes = ctx->state.unpack_row_bytes, saved_image_bytes = ctx->state.unpack_image_bytes;
        GLint saved_image_height = ctx->state.unpack_image_height, saved_skip_images = ctx->state.unpack_skip_images;
        bool saved_swap = ctx->state.unpack_swap_bytes;
        ctx->state.unpack_image_height = ctx->state.unpack_skip_images = 0;
        ctx->state.unpack_swap_bytes = false;
        ctx->state.unpack_row_bytes = ctx->state.unpack_image_bytes = 0;
        ctx->state.unpack_alignment = 1;
        ctx->state.unpack_row_length = ctx->state.unpack_skip_rows = ctx->state.unpack_skip_pixels = 0;
        size_t row_bytes, image_bytes;
        void *converted = glm_convert_upload(ctx, &info, width, height, 1, GL_RGBA, GL_FLOAT, rgba, &row_bytes, &image_bytes);
        ctx->state.unpack_alignment = saved_alignment;
        ctx->state.unpack_row_length = saved_row;
        ctx->state.unpack_skip_rows = saved_skip_rows;
        ctx->state.unpack_skip_pixels = saved_skip_pixels;
        ctx->state.unpack_row_bytes = saved_row_bytes;
        ctx->state.unpack_image_bytes = saved_image_bytes;
        ctx->state.unpack_image_height = saved_image_height;
        ctx->state.unpack_skip_images = saved_skip_images;
        ctx->state.unpack_swap_bytes = saved_swap;
        if (converted)
            glm_backend_texture_image(ctx, texture, face, level, dst_x, dst_y, dst_z, width, height, 1, converted, row_bytes,
                                      image_bytes);
        free(converted);
    }
    free(rgba);
}

/* glGetTexImage's conversion of a level's texels (RGBA, storage channels)
   to what the client receives. */
static void finish_tex_image(struct glm_context *ctx, const struct glm_texture_level *l, const struct glm_format_info *info,
                             float *rgba, NSUInteger width, NSUInteger total_rows, GLenum format, GLenum type, void *pixels)
{
    /* The level as Apple's implementation returns it: sampled through the
       format's swizzle, then alpha, luminance and intensity as (0,0,0,A),
       (L,0,0,1), (L,0,0,A) and (I,0,0,1), absent colour channels 0 and
       absent alpha 1. Which channels the format has is Apple's. */
    const struct glm_apple_texture *ref = glm_apple_texture_format(ctx, l->internal_format);
    if (ref && !ref->size[6] && !ref->size[7]) {
        const uint8_t *sw = info->swizzle;
        bool identity = sw[0] == 0 && sw[1] == 1 && sw[2] == 2 && sw[3] == 3;
        bool intensity = ref->size[5] > 0, luminance = ref->size[4] > 0;
        for (size_t i = 0; i < (size_t)width * total_rows; ++i) {
            float *p = rgba + i * 4, raw[4] = {p[0], p[1], p[2], p[3]}, v[4];
            for (int c = 0; c < 4; ++c) v[c] = identity ? raw[c] : sw[c] < 4 ? raw[sw[c]] : sw[c] == 4 ? 0.0f : 1.0f;
            if (intensity) {
                p[0] = v[0]; p[1] = p[2] = 0; p[3] = 1.0f;
            } else if (luminance) {
                p[0] = v[0]; p[1] = p[2] = 0; p[3] = ref->size[3] ? v[3] : 1.0f;
            } else {
                for (int c = 0; c < 3; ++c) p[c] = ref->size[c] ? v[c] : 0.0f;
                p[3] = ref->size[3] ? v[3] : 1.0f;
            }
        }
    }
    /* Read back as luminance, a texel's L is its red (not R+G+B as for
       glReadPixels). */
    if (format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA)
        for (size_t i = 0; i < (size_t)width * total_rows; ++i) rgba[i * 4 + 1] = rgba[i * 4 + 2] = 0;
    if (!glm_convert_pack(ctx, rgba, (GLsizei)width, (GLsizei)total_rows, format, type, pixels)) glm_error(ctx, GL_INVALID_ENUM);
}

/* glGetTexImage / glGetCompressedTexImage: a level (every layer or depth
   slice) read back, converted like glReadPixels or as raw blocks. */
void glm_backend_get_tex_image(struct glm_context *ctx, struct glm_texture *t, int face, GLint level, GLenum format,
                               GLenum type, void *pixels, bool raw)
{
    struct glm_backend_context *b = ctx->backend;
    if (!glm_backend_texture_ensure_storage(ctx, t)) return glm_error(ctx, GL_INVALID_OPERATION);
    id<MTLTexture> storage = (__bridge id<MTLTexture>)t->backend;
    const struct glm_texture_level *l = &t->levels[face][level];
    if (!l->defined || (NSUInteger)level >= storage.mipmapLevelCount || storage.sampleCount > 1)
        return glm_error(ctx, GL_INVALID_OPERATION);
    if (!raw) {
        /* Depth formats read back as depth only, colour formats as colour. */
        struct glm_format_info kind;
        bool depth_level = glm_format_lookup(l->internal_format, &kind) && (kind.depth || kind.stencil);
        bool depth_format = format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL || format == GL_STENCIL_INDEX;
        if (depth_level != depth_format) return glm_error(ctx, GL_INVALID_OPERATION);
    }
    end_encoder(b);
    bool volume = storage.textureType == MTLTextureType3D;
    bool rows = t->target == GL_TEXTURE_1D_ARRAY;
    NSUInteger width = MAX(storage.width >> level, 1u), height = rows ? 1 : MAX(storage.height >> level, 1u);
    NSUInteger images = volume ? MAX(storage.depth >> level, 1u)
                        : rows ? (NSUInteger)l->height
                        : storage.textureType == MTLTextureType2DArray || storage.textureType == MTLTextureTypeCubeArray
                            ? (NSUInteger)l->depth : 1;
    struct glm_format_info info;
    glm_format_lookup(l->internal_format, &info);
    size_t texel = glm_metal_format_bytes((uint32_t)storage.pixelFormat);
    NSUInteger bytes_per_row = info.compressed ? ((width + 3) / 4) * (NSUInteger)info.bytes_per_pixel : width * texel;
    NSUInteger block_rows = info.compressed ? (height + 3) / 4 : height;
    NSUInteger image_bytes = bytes_per_row * block_rows;
    id<MTLBuffer> staging = glm_new_shared_buffer(image_bytes * images, NULL);
    if (storage.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
        /* The planes separately (Metal copies a combined format one plane at
           a time), interleaved as an 8-byte depth + stencil texel. */
        NSUInteger plane_rows[2] = {(width * 4 + 255) & ~(NSUInteger)255, (width + 255) & ~(NSUInteger)255};
        id<MTLBuffer> planes[2];
        id<MTLBlitCommandEncoder> blit = [command_buffer(b) blitCommandEncoder];
        for (int p = 0; p < 2; ++p) {
            NSUInteger plane_image = plane_rows[p] * height;
            planes[p] = glm_new_shared_buffer(plane_image * images, NULL);
            for (NSUInteger i = 0; i < images; ++i)
                [blit copyFromTexture:storage sourceSlice:rows || images > 1 ? i : (NSUInteger)face sourceLevel:(NSUInteger)level
                         sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1) toBuffer:planes[p]
                    destinationOffset:i * plane_image destinationBytesPerRow:plane_rows[p] destinationBytesPerImage:plane_image
                              options:p ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil];
        }
        [blit endEncoding];
        glm_backend_flush(ctx, true);
        uint8_t *out = mtl_contents(staging);
        const uint8_t *d = mtl_contents(planes[0]), *s = mtl_contents(planes[1]);
        for (NSUInteger r = 0; r < height * images; ++r)
            for (NSUInteger x = 0; x < width; ++x) {
                uint8_t *texel = out + r * bytes_per_row + x * 8;
                memcpy(texel, d + r * plane_rows[0] + x * 4, 4);
                uint32_t stencil = s[r * plane_rows[1] + x];
                memcpy(texel + 4, &stencil, 4);
            }
    } else {
        id<MTLBlitCommandEncoder> blit = [command_buffer(b) blitCommandEncoder];
        for (NSUInteger i = 0; i < images; ++i)
            [blit copyFromTexture:storage sourceSlice:volume ? 0 : rows || images > 1 ? i : (NSUInteger)face sourceLevel:(NSUInteger)level
                     sourceOrigin:MTLOriginMake(0, 0, volume ? i : 0) sourceSize:MTLSizeMake(width, height, 1) toBuffer:staging
                destinationOffset:i * image_bytes destinationBytesPerRow:bytes_per_row destinationBytesPerImage:image_bytes];
        [blit endEncoding];
        glm_backend_flush(ctx, true);
    }
    if (raw) {
        memcpy(pixels, mtl_contents(staging), image_bytes * images);
        return;
    }
    /* 3D and 2D-array texture queries use image packing in addition to
       ordinary row/pixel packing. 1D arrays are a single 2D image. */
    bool pack_images = volume || t->target == GL_TEXTURE_2D_ARRAY || t->target == GL_TEXTURE_CUBE_MAP_ARRAY;
    NSUInteger pack_count = pack_images ? images : 1;
    NSUInteger pack_height = pack_images ? height : height * images;
    size_t pack_stride = 0, pack_offset = 0;
    if (pack_images) {
        const struct glm_state *s = &ctx->state;
        size_t bytes = glm_pixel_bytes(format, type);
        size_t row_pixels = s->pack_row_length > 0 ? (size_t)s->pack_row_length : width;
        size_t alignment = s->pack_alignment > 0 ? (size_t)s->pack_alignment : 4;
        size_t rows = s->pack_image_height > 0 ? (size_t)s->pack_image_height : height;
        if (!bytes) return glm_error(ctx, GL_INVALID_ENUM);
        if (row_pixels > (SIZE_MAX - alignment + 1) / bytes) return glm_error(ctx, GL_INVALID_OPERATION);
        size_t row = s->pack_row_bytes > 0 ? (size_t)s->pack_row_bytes
                                          : (row_pixels * bytes + alignment - 1) / alignment * alignment;
        if (rows && row > SIZE_MAX / rows) return glm_error(ctx, GL_INVALID_OPERATION);
        pack_stride = row * rows;
        size_t skipped = (size_t)s->pack_skip_images;
        if (pack_stride && (skipped > SIZE_MAX / pack_stride ||
            images - 1 > SIZE_MAX / pack_stride - skipped)) return glm_error(ctx, GL_INVALID_OPERATION);
        pack_offset = skipped * pack_stride;
    }
    NSUInteger total_rows = height * images;
    if (info.compressed) {
        float *rgba = calloc((size_t)width * total_rows * 4, sizeof(float));
        bool ok = true;
        for (NSUInteger i = 0; i < images && ok; ++i)
            ok = glm_decode_blocks((uint32_t)storage.pixelFormat, (const uint8_t *)mtl_contents(staging) + i * image_bytes,
                                   bytes_per_row, width, height, rgba + (size_t)i * width * height * 4);
        if (!ok) {
            free(rgba);
            return glm_error(ctx, GL_INVALID_OPERATION);
        }
        for (NSUInteger i = 0; i < pack_count; ++i)
            finish_tex_image(ctx, l, &info, rgba + (size_t)i * width * pack_height * 4,
                             width, pack_height, format, type, (uint8_t *)pixels + pack_offset + i * pack_stride);
        free(rgba);
        return;
    }
    if (glm_unpack_texels_int((uint32_t)storage.pixelFormat, NULL, 0, NULL)) {
        int64_t *values = calloc((size_t)width * total_rows * 4, sizeof(int64_t));
        for (NSUInteger row = 0; row < total_rows; ++row)
            glm_unpack_texels_int((uint32_t)storage.pixelFormat, (const uint8_t *)mtl_contents(staging) + row * bytes_per_row,
                                  width, values + (size_t)row * width * 4);
        for (NSUInteger i = 0; i < pack_count; ++i)
            if (!glm_convert_pack_int(ctx, values + (size_t)i * width * pack_height * 4,
                                      (GLsizei)width, (GLsizei)pack_height, format, type,
                                      (uint8_t *)pixels + pack_offset + i * pack_stride))
                glm_error(ctx, GL_INVALID_ENUM);
        free(values);
        return;
    }
    float *rgba = malloc((size_t)width * total_rows * 16);
    for (NSUInteger row = 0; row < total_rows; ++row)
        glm_unpack_texels((uint32_t)storage.pixelFormat, (const uint8_t *)mtl_contents(staging) + row * bytes_per_row, width,
                          rgba + (size_t)row * width * 4);
    for (NSUInteger i = 0; i < pack_count; ++i)
        finish_tex_image(ctx, l, &info, rgba + (size_t)i * width * pack_height * 4,
                         width, pack_height, format, type, (uint8_t *)pixels + pack_offset + i * pack_stride);
    free(rgba);
}

/* ---- default framebuffer ------------------------------------------------- */

GLM_HIDDEN void glm_backend_default_framebuffer_resize(struct glm_context *ctx, GLsizei width, GLsizei height)
{
    struct glm_backend_context *b = ctx->backend;
    if (b->default_color && b->default_color.width == (NSUInteger)width && b->default_color.height == (NSUInteger)height) return;
    end_encoder(b);
    if (width <= 0 || height <= 0) {
        b->default_color = b->default_depth = nil;
        return;
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:(NSUInteger)width
                                                                                height:(NSUInteger)height
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    d.storageMode = MTLStorageModePrivate;
    b->default_color = [device newTextureWithDescriptor:d];
    d.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    b->default_depth = [device newTextureWithDescriptor:d];
    b->framebuffer_dirty = true;
}

GLM_HIDDEN void *glm_backend_default_color(struct glm_context *ctx) { return (__bridge void *)ctx->backend->default_color; }
GLM_HIDDEN void *glm_backend_command_buffer(struct glm_context *ctx)
{
    end_encoder(ctx->backend);
    return (__bridge void *)command_buffer(ctx->backend);
}

/* ---- fences -------------------------------------------------------------- */

/* Submits pending work and returns a serial that completes with it. */
GLM_HIDDEN uint64_t glm_backend_fence_insert(struct glm_context *ctx)
{
    glm_backend_flush(ctx, false);
    return ctx->backend->submitted_serial;
}

GLM_HIDDEN bool glm_backend_fence_done(struct glm_context *ctx, uint64_t serial)
{
    return __atomic_load_n(&ctx->backend->completed_serial, __ATOMIC_ACQUIRE) >= serial;
}

GLM_HIDDEN void glm_backend_fence_wait(struct glm_context *ctx, uint64_t serial)
{
    while (!glm_backend_fence_done(ctx, serial)) {
        if (ctx->backend->submitted_serial < serial) return;
        usleep(50);
    }
}
