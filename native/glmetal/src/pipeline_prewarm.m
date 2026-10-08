/* Learn the most frequently used pipeline configuration for each vertex
   input signature. New programs can prepare that configuration while the
   application translates subsequent shaders. Predictions never select draw
   state: acquisition compares functions and every descriptor field used by
   pipeline_for. A different target, blend mode or vertex layout is a miss.

   Two background compiler jobs, at most 32 outstanding predictions and 512
   cached states bound speculation. A draw can claim a queued job immediately
   rather than waiting behind predictions it will never use. */
#import "pipeline_prewarm.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { CACHE_LIMIT = 512, PENDING_LIMIT = 32, SIGNATURE_LIMIT = 128, RECIPE_LIMIT = 4 };
@interface GLMPipelineEntry : NSObject {
@public
    dispatch_group_t group;
    MTLRenderPipelineDescriptor *descriptor;
    id<MTLRenderPipelineState> state;
    NSError *error;
    bool started, done, predicted;
}
@end
@implementation GLMPipelineEntry
@end
@interface GLMPipelineRecipe : NSObject {
@public
    MTLRenderPipelineDescriptor *descriptor;
    NSData *key;
    unsigned uses;
}
@end
@implementation GLMPipelineRecipe
@end

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSData *, GLMPipelineEntry *> *cache;
static NSMutableArray<NSData *> *order;
static NSMutableDictionary<NSData *, NSMutableArray<GLMPipelineRecipe *> *> *recipes;
static NSOperationQueue *compiler;
static unsigned pending;
static bool disabled;
static struct glm_pipeline_stats stats;

static void initialize(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        cache = [NSMutableDictionary dictionary];
        order = [NSMutableArray array];
        recipes = [NSMutableDictionary dictionary];
        compiler = [NSOperationQueue new];
        compiler.name = @"GLMetal pipeline prewarm";
        compiler.maxConcurrentOperationCount = 2;
        compiler.qualityOfService = NSQualityOfServiceUtility;
        disabled = getenv("GLMETAL_NO_PIPELINE_PREWARM") != NULL;
    });
}

static NSData *descriptor_key(MTLRenderPipelineDescriptor *d)
{
    /* Fixed-size integers avoid structure padding and native enum widths.
       These are all fields assigned by pipeline_for, plus full vertex fetch
       descriptors. Function addresses remain alive in cached descriptors. */
    uint64_t v[320] = {0}; unsigned n = 0;
#define VALUE(x) v[n++] = (uint64_t)(x)
    VALUE((uintptr_t)(__bridge void *)d.vertexFunction);
    VALUE((uintptr_t)(__bridge void *)d.fragmentFunction);
    VALUE(d.rasterSampleCount); VALUE(d.alphaToCoverageEnabled);
    VALUE(d.rasterizationEnabled); VALUE(d.inputPrimitiveTopology);
    VALUE(d.depthAttachmentPixelFormat); VALUE(d.stencilAttachmentPixelFormat);
    VALUE(d.maxTessellationFactor); VALUE(d.tessellationFactorFormat);
    VALUE(d.tessellationFactorStepFunction); VALUE(d.tessellationControlPointIndexType);
    VALUE(d.tessellationPartitionMode); VALUE(d.tessellationOutputWindingOrder);
    for (unsigned i = 0; i < 8; ++i) {
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        VALUE(c.pixelFormat); VALUE(c.writeMask); VALUE(c.blendingEnabled);
        VALUE(c.sourceRGBBlendFactor); VALUE(c.destinationRGBBlendFactor);
        VALUE(c.sourceAlphaBlendFactor); VALUE(c.destinationAlphaBlendFactor);
        VALUE(c.rgbBlendOperation); VALUE(c.alphaBlendOperation);
    }
    VALUE(d.vertexDescriptor != nil);
    if (d.vertexDescriptor) for (unsigned i = 0; i < 31; ++i) {
        MTLVertexAttributeDescriptor *a = d.vertexDescriptor.attributes[i];
        MTLVertexBufferLayoutDescriptor *b = d.vertexDescriptor.layouts[i];
        VALUE(a.format); VALUE(a.offset); VALUE(a.bufferIndex);
        VALUE(b.stride); VALUE(b.stepFunction); VALUE(b.stepRate);
    }
#undef VALUE
    return [NSData dataWithBytes:v length:n * sizeof(*v)];
}

static NSData *input_signature(id<MTLFunction> vertex)
{
    uint64_t inputs[31] = {0};
    for (MTLVertexAttribute *a in vertex.vertexAttributes)
        if (a.active && a.attributeIndex < 31) inputs[a.attributeIndex] = a.attributeType + 1;
    return [NSData dataWithBytes:inputs length:sizeof inputs];
}

/* All cache/recipe bookkeeping is under lock, never native compilation. */
static void trim_cache(void)
{
    while (cache.count > CACHE_LIMIT) {
        NSUInteger i;
        for (i = 0; i < order.count; ++i) if (cache[order[i]]->done) break;
        if (i == order.count) break;
        [cache removeObjectForKey:order[i]]; [order removeObjectAtIndex:i];
    }
}

static void compile_entry(id<MTLDevice> device, NSData *key, GLMPipelineEntry *entry)
{
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:entry->descriptor error:&error];
    pthread_mutex_lock(&lock);
    entry->state = state; entry->error = error; entry->done = true;
    if (entry->predicted) --pending;
    if (!state && cache[key] == entry) {
        [cache removeObjectForKey:key]; [order removeObject:key];
    }
    trim_cache();
    pthread_mutex_unlock(&lock);
    dispatch_group_leave(entry->group);
}

static void learn_recipe(MTLRenderPipelineDescriptor *d)
{
    if (!d.vertexFunction || !d.fragmentFunction || !d.rasterizationEnabled) return;
    NSData *signature = input_signature(d.vertexFunction);
    MTLRenderPipelineDescriptor *copy = [d copy];
    copy.vertexFunction = nil; copy.fragmentFunction = nil;
    NSData *key = descriptor_key(copy);
    pthread_mutex_lock(&lock);
    NSMutableArray<GLMPipelineRecipe *> *list = recipes[signature];
    if (!list) {
        if (recipes.count >= SIGNATURE_LIMIT) { pthread_mutex_unlock(&lock); return; }
        recipes[signature] = list = [NSMutableArray array];
    }
    for (GLMPipelineRecipe *r in list) if ([r->key isEqual:key]) {
        ++r->uses; pthread_mutex_unlock(&lock); return;
    }
    if (list.count == RECIPE_LIMIT) {
        /* A bounded frequent-item counter can adapt after an application
           changes render targets; the first configurations cannot pin the
           prediction forever. Only positive counts are prediction candidates. */
        for (GLMPipelineRecipe *r in list) if (r->uses) --r->uses;
        for (NSUInteger i = 0; i < list.count; ++i) if (!list[i]->uses) {
            [list removeObjectAtIndex:i]; break;
        }
    }
    if (list.count < RECIPE_LIMIT) {
        GLMPipelineRecipe *r = [GLMPipelineRecipe new];
        r->descriptor = copy; r->key = key; r->uses = 1; [list addObject:r];
    }
    pthread_mutex_unlock(&lock);
}

id<MTLRenderPipelineState> glm_pipeline_acquire(id<MTLDevice> device,
    MTLRenderPipelineDescriptor *descriptor, bool learn, NSError **error)
{
    initialize();
    if (disabled) return [device newRenderPipelineStateWithDescriptor:descriptor error:error];
    NSData *key = descriptor_key(descriptor);
    pthread_mutex_lock(&lock);
    GLMPipelineEntry *entry = cache[key];
    bool start = !entry || !entry->started;
    if (!entry) {
        entry = [GLMPipelineEntry new]; entry->descriptor = [descriptor copy];
        entry->group = dispatch_group_create(); dispatch_group_enter(entry->group);
        cache[key] = entry; [order addObject:key]; trim_cache();
        ++stats.demanded;
    } else {
        if (entry->predicted) ++stats.reused;
        if (entry->started && !entry->done) ++stats.waited;
    }
    if (start) entry->started = true;
    pthread_mutex_unlock(&lock);
    if (start) compile_entry(device, key, entry);
    else dispatch_group_wait(entry->group, DISPATCH_TIME_FOREVER);
    if (error) *error = entry->error;
    if (learn && entry->state) learn_recipe(descriptor);
    return entry->state;
}

void glm_pipeline_prewarm(id<MTLDevice> device, id<MTLFunction> vertex, id<MTLFunction> fragment)
{
    initialize();
    if (disabled || !vertex || !fragment) return;
    NSData *signature = input_signature(vertex);
    pthread_mutex_lock(&lock);
    GLMPipelineRecipe *best = nil;
    for (GLMPipelineRecipe *r in recipes[signature]) if (r->uses && (!best || r->uses > best->uses)) best = r;
    unsigned second = 0;
    for (GLMPipelineRecipe *r in recipes[signature]) if (r != best && r->uses > second) second = r->uses;
    /* Skip unfamiliar or ambiguous configurations rather than doubling
       compilation work with low-confidence predictions. */
    if (!best || best->uses < 2 || best->uses / 2 < second) { pthread_mutex_unlock(&lock); return; }
    MTLRenderPipelineDescriptor *d = [best->descriptor copy];
    d.vertexFunction = vertex; d.fragmentFunction = fragment;
    NSData *key = descriptor_key(d);
    if (cache[key]) { pthread_mutex_unlock(&lock); return; }
    if (pending >= PENDING_LIMIT) { ++stats.dropped; pthread_mutex_unlock(&lock); return; }
    GLMPipelineEntry *entry = [GLMPipelineEntry new]; entry->descriptor = d; entry->predicted = true;
    entry->group = dispatch_group_create(); dispatch_group_enter(entry->group);
    cache[key] = entry; [order addObject:key]; ++pending; ++stats.predicted; trim_cache();
    pthread_mutex_unlock(&lock);
    [compiler addOperationWithBlock:^{
        @autoreleasepool {
            pthread_mutex_lock(&lock);
            bool start = !entry->started; if (start) entry->started = true;
            pthread_mutex_unlock(&lock);
            if (start) compile_entry(device, key, entry);
        }
    }];
}

struct glm_pipeline_stats glm_pipeline_statistics(void)
{
    pthread_mutex_lock(&lock); struct glm_pipeline_stats result = stats; pthread_mutex_unlock(&lock);
    return result;
}
