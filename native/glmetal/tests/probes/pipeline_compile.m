/* Compiler-only benchmark: creates libraries and pipeline states, no command
   queues, buffers or GPU submissions. Feed captured compiler-result MSL via
   an index containing vert_msl/frag_msl paths. Run modes in separate processes. */
#import <Metal/Metal.h>
#include "../../src/pipeline_prewarm.h"
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static double now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
static MTLRenderPipelineDescriptor *descriptor(id<MTLFunction> v, id<MTLFunction> f)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = v; d.fragmentFunction = f;
    MTLVertexDescriptor *layout = [MTLVertexDescriptor vertexDescriptor];
    for (MTLVertexAttribute *a in v.vertexAttributes) {
        if (!a.active || a.attributeIndex >= 16) continue;
        MTLVertexAttributeDescriptor *slot = layout.attributes[a.attributeIndex];
        slot.format = a.attributeType >= MTLDataTypeInt && a.attributeType <= MTLDataTypeInt4 ?
            MTLVertexFormatInt4 : a.attributeType >= MTLDataTypeUInt && a.attributeType <= MTLDataTypeUInt4 ?
            MTLVertexFormatUInt4 : MTLVertexFormatFloat4;
        slot.bufferIndex = 30; slot.offset = a.attributeIndex * 16;
    }
    layout.layouts[30].stride = 16 * 16;
    layout.layouts[30].stepFunction = MTLVertexStepFunctionPerVertex;
    d.vertexDescriptor = layout;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    d.rasterSampleCount = 1;
    return d;
}

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: pipeline_compile index.json baseline|prewarm cold|warm gap_us\n"); return 2; }
    bool prewarm = !strcmp(argv[2], "prewarm"), cold = !strcmp(argv[3], "cold");
    unsigned gap = (unsigned)strtoul(argv[4], NULL, 10);
    if (!prewarm) setenv("GLMETAL_NO_PIPELINE_PREWARM", "1", 1);
    @autoreleasepool {
        NSError *error = nil;
        NSArray *index = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:@(argv[1])]
            options:0 error:&error];
        if (!index) { fprintf(stderr, "index: %s\n", error.localizedDescription.UTF8String); return 2; }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return 2;
        NSMutableArray<MTLRenderPipelineDescriptor *> *descriptors = [NSMutableArray array];
        NSOperationQueue *libraries = [NSOperationQueue new]; libraries.maxConcurrentOperationCount = 2;
        double library_start = now_ms();
        for (NSDictionary *record in index) {
            NSMutableArray *functions = [NSMutableArray arrayWithObjects:[NSNull null], [NSNull null], nil];
            for (unsigned stage = 0; stage < 2; ++stage) {
                NSString *source = [NSString stringWithContentsOfFile:record[stage ? @"frag_msl" : @"vert_msl"]
                    encoding:NSUTF8StringEncoding error:&error];
                if (!source) return 2;
                NSString *entry = cold ? [NSString stringWithFormat:@"bench_%d_%s_main0", getpid(), argv[2]] : @"main0";
                if (cold) source = [source stringByReplacingOccurrencesOfString:@"main0" withString:entry];
                [libraries addOperationWithBlock:^{ @autoreleasepool {
                    MTLCompileOptions *options = [MTLCompileOptions new];
                    options.fastMathEnabled = NO; options.languageVersion = MTLLanguageVersion2_3;
                    NSError *failure = nil;
                    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&failure];
                    id<MTLFunction> function = [library newFunctionWithName:entry];
                    if (!function) { fprintf(stderr, "library: %s\n", failure.localizedDescription.UTF8String); exit(2); }
                    @synchronized(functions) { functions[stage] = function; }
                }}];
            }
            [libraries waitUntilAllOperationsAreFinished];
            [descriptors addObject:descriptor(functions[0], functions[1])];
        }
        double library_ms = now_ms() - library_start;
        /* Seed every observed vertex-input signature with the same target
           configuration. This models configurations learned in earlier areas. */
        for (MTLRenderPipelineDescriptor *d in descriptors) {
            if (!glm_pipeline_acquire(device, d, true, &error)) {
                fprintf(stderr, "seed: %s\n", error.localizedDescription.UTF8String); return 2;
            }
        }
        /* Use new function identities/AIR entry names for the measured burst,
           so neither the process cache nor Metal's pipeline cache is seeded. */
        NSMutableArray<MTLRenderPipelineDescriptor *> *burst = [NSMutableArray array];
        for (NSDictionary *record in index) {
            id<MTLFunction> v = nil, f = nil;
            for (unsigned stage = 0; stage < 2; ++stage) {
                NSString *source = [NSString stringWithContentsOfFile:record[stage ? @"frag_msl" : @"vert_msl"]
                    encoding:NSUTF8StringEncoding error:&error];
                NSString *entry = [NSString stringWithFormat:@"burst_%d_%s_main0", getpid(), argv[2]];
                source = [source stringByReplacingOccurrencesOfString:@"main0" withString:entry];
                MTLCompileOptions *options = [MTLCompileOptions new];
                options.fastMathEnabled = NO; options.languageVersion = MTLLanguageVersion2_3;
                id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
                id<MTLFunction> function = [library newFunctionWithName:entry];
                if (!function) { fprintf(stderr, "burst library: %s\n", error.localizedDescription.UTF8String); return 2; }
                if (stage) f = function; else v = function;
            }
            [burst addObject:descriptor(v, f)];
        }
        struct glm_pipeline_stats before = glm_pipeline_statistics();
        double started = now_ms();
        for (MTLRenderPipelineDescriptor *d in burst) {
            if (prewarm) glm_pipeline_prewarm(device, d.vertexFunction, d.fragmentFunction);
            if (gap) usleep(gap);
        }
        double lead_ms = now_ms() - started;
        double wait = 0; unsigned successes = 0;
        for (MTLRenderPipelineDescriptor *d in burst) {
            double began = now_ms();
            id<MTLRenderPipelineState> p = glm_pipeline_acquire(device, d, false, &error);
            wait += now_ms() - began;
            if (!p) { fprintf(stderr, "pipeline: %s\n", error.localizedDescription.UTF8String); return 2; }
            ++successes;
        }
        struct glm_pipeline_stats after = glm_pipeline_statistics();
        printf("{\"mode\":\"%s\",\"programs\":%u,\"library_ms\":%.3f,\"lead_ms\":%.3f,\"first_draw_pipeline_ms\":%.3f,\"burst_wall_ms\":%.3f,\"predicted\":%llu,\"reused\":%llu,\"demanded\":%llu,\"waited\":%llu,\"dropped\":%llu}\n",
            argv[2], successes, library_ms, lead_ms, wait, now_ms()-started,
            after.predicted-before.predicted, after.reused-before.reused,
            after.demanded-before.demanded, after.waited-before.waited, after.dropped-before.dropped);
    }
    return 0;
}
