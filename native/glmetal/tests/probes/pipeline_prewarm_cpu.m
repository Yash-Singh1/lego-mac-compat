/* Mock Metal compilation only. Descriptor objects never reach a real device. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include "../../src/pipeline_prewarm.m"

@interface FakeAttribute : NSObject
@property(getter=isActive) BOOL active;
@property NSUInteger attributeIndex;
@property MTLDataType attributeType;
@end
@implementation FakeAttribute
@end
@interface FakeFunction : NSObject
@property(copy) NSArray *vertexAttributes;
@property MTLFunctionType functionType;
@end
@implementation FakeFunction
@end
@interface FakeDevice : NSObject {
@public
    NSCondition *condition;
    unsigned calls, active, peak;
    bool gated, fail_next;
}
@end
@implementation FakeDevice
- (instancetype)init { if ((self=[super init])) condition=[NSCondition new]; return self; }
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
    assert(d.vertexFunction);
    [condition lock];
    ++calls; ++active; if(active>peak)peak=active;
    [condition broadcast];
    while(gated) [condition wait];
    bool fail=fail_next;fail_next=false;--active;
    [condition unlock];
    if(fail) { if(error)*error=[NSError errorWithDomain:@"mock" code:1 userInfo:nil];return nil; }
    return (id<MTLRenderPipelineState>)[NSObject new];
}
@end
static id<MTLFunction> function(void) {
    FakeAttribute *a=[FakeAttribute new];a.active=YES;a.attributeIndex=0;a.attributeType=MTLDataTypeFloat4;
    FakeFunction *f=[FakeFunction new];f.vertexAttributes=@[a];f.functionType=MTLFunctionTypeVertex;return (id<MTLFunction>)f;
}
static id<MTLFunction> fragment(void) { FakeFunction *f=(FakeFunction *)function();f.functionType=MTLFunctionTypeFragment;return (id<MTLFunction>)f; }
static MTLRenderPipelineDescriptor *descriptor(id<MTLFunction> v,id<MTLFunction> f) {
    MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
    d.vertexFunction=v;d.fragmentFunction=f;d.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    d.vertexDescriptor=[MTLVertexDescriptor vertexDescriptor];
    d.vertexDescriptor.attributes[0].format=MTLVertexFormatFloat4;
    d.vertexDescriptor.layouts[0].stride=16;
    d.vertexDescriptor.layouts[0].stepFunction=MTLVertexStepFunctionPerVertex;
    d.vertexDescriptor.layouts[0].stepRate=1;
    return d;
}
static void reset(void) {
    compiler.suspended=NO;[compiler waitUntilAllOperationsAreFinished];
    pthread_mutex_lock(&lock);
    assert(!pending);[cache removeAllObjects];[order removeAllObjects];[recipes removeAllObjects];
    memset(&stats,0,sizeof(stats));pthread_mutex_unlock(&lock);
}
static FakeDevice *train(void) {
    reset();FakeDevice *dev=[FakeDevice new];
    MTLRenderPipelineDescriptor *d=descriptor(function(),fragment());
    assert(glm_pipeline_acquire((id<MTLDevice>)dev,d,true,NULL));
    assert(glm_pipeline_acquire((id<MTLDevice>)dev,d,true,NULL));
    assert(recipes.count==1);return dev;
}
struct Demand { __strong FakeDevice *device; __strong MTLRenderPipelineDescriptor *descriptor; __strong id<MTLRenderPipelineState> state; };
static void *demand_worker(void *raw) {
    @autoreleasepool { struct Demand *d=raw;d->state=glm_pipeline_acquire((id<MTLDevice>)d->device,d->descriptor,false,NULL); }
    return NULL;
}
#define CHANGE(x) ^(MTLRenderPipelineDescriptor *d){ x; }
int main(void) { @autoreleasepool {
    alarm(15);unsetenv("GLMETAL_NO_PIPELINE_PREWARM");initialize();
    FakeDevice *dev=train();id<MTLFunction> v=function(),f=fragment();
    glm_pipeline_prewarm((id<MTLDevice>)dev,v,f);[compiler waitUntilAllOperationsAreFinished];
    assert(dev->calls==2 && stats.predicted==1);
    MTLRenderPipelineDescriptor *base=descriptor(v,f);
    id<MTLRenderPipelineState> hit=glm_pipeline_acquire((id<MTLDevice>)dev,base,false,NULL);
    assert(hit && dev->calls==2 && stats.reused==1);
    NSArray *changes=@[
      CHANGE(d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm),
      CHANGE(d.colorAttachments[7].pixelFormat=MTLPixelFormatRGBA16Float),
      CHANGE(d.colorAttachments[0].writeMask=MTLColorWriteMaskRed),
      CHANGE(d.colorAttachments[0].blendingEnabled=YES),
      CHANGE(d.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha),
      CHANGE(d.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOne),
      CHANGE(d.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorSourceAlpha),
      CHANGE(d.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOne),
      CHANGE(d.colorAttachments[0].rgbBlendOperation=MTLBlendOperationSubtract),
      CHANGE(d.colorAttachments[0].alphaBlendOperation=MTLBlendOperationMax),
      CHANGE(d.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float),
      CHANGE(d.stencilAttachmentPixelFormat=MTLPixelFormatStencil8),
      CHANGE(d.rasterSampleCount=4), CHANGE(d.alphaToCoverageEnabled=YES),
      CHANGE(d.rasterizationEnabled=NO), CHANGE(d.inputPrimitiveTopology=MTLPrimitiveTopologyClassLine),
      CHANGE(d.vertexDescriptor.attributes[0].offset=4),
      CHANGE(d.vertexDescriptor.attributes[0].bufferIndex=1),
      CHANGE(d.vertexDescriptor.attributes[0].format=MTLVertexFormatInt4),
      CHANGE(d.vertexDescriptor.layouts[0].stride=32),
      CHANGE(d.vertexDescriptor.layouts[0].stepFunction=MTLVertexStepFunctionPerInstance),
      CHANGE(d.vertexDescriptor.layouts[0].stepRate=2),
      CHANGE(d.vertexDescriptor.attributes[30].offset=8),
      CHANGE(d.vertexDescriptor.layouts[30].stride=64),
      CHANGE(d.vertexDescriptor=nil),
      CHANGE(d.maxTessellationFactor=base.maxTessellationFactor==64?32:64),
      CHANGE(d.tessellationFactorFormat=(MTLTessellationFactorFormat)(base.tessellationFactorFormat+1)),
      CHANGE(d.tessellationFactorStepFunction=MTLTessellationFactorStepFunctionPerInstance),
      CHANGE(d.tessellationControlPointIndexType=MTLTessellationControlPointIndexTypeUInt16),
      CHANGE(d.tessellationPartitionMode=MTLTessellationPartitionModeFractionalEven),
      CHANGE(d.tessellationOutputWindingOrder=base.tessellationOutputWindingOrder==MTLWindingClockwise?MTLWindingCounterClockwise:MTLWindingClockwise)
    ];
    unsigned variant=0;
    for(void (^change)(MTLRenderPipelineDescriptor *) in changes) {
      MTLRenderPipelineDescriptor *d=[base copy];change(d);
      if([descriptor_key(d) isEqual:descriptor_key(base)]) fprintf(stderr,"unchanged mutation %u\n",variant);
      ++variant;
      assert(![descriptor_key(d) isEqual:descriptor_key(base)]);
      unsigned before=dev->calls;
      assert(glm_pipeline_acquire((id<MTLDevice>)dev,d,false,NULL)!=hit);
      assert(dev->calls==before+1);
    }
    /* Demand claims a prediction while the compiler queue is suspended. */
    dev=train();compiler.suspended=YES;v=function();f=fragment();
    glm_pipeline_prewarm((id<MTLDevice>)dev,v,f);
    assert(pending==1 && dev->calls==1);
    assert(glm_pipeline_acquire((id<MTLDevice>)dev,descriptor(v,f),false,NULL));
    assert(dev->calls==2 && !pending);
    compiler.suspended=NO;[compiler waitUntilAllOperationsAreFinished];assert(dev->calls==2);
    /* A demand waits for an already running prediction and shares its state. */
    dev=train();dev->gated=true;v=function();f=fragment();
    glm_pipeline_prewarm((id<MTLDevice>)dev,v,f);
    [dev->condition lock];
    NSDate *running_deadline=[NSDate dateWithTimeIntervalSinceNow:5];
    while(!dev->active) assert([dev->condition waitUntilDate:running_deadline]);
    [dev->condition unlock];
    struct Demand demand={dev,descriptor(v,f),nil};pthread_t waiter;
    assert(!pthread_create(&waiter,NULL,demand_worker,&demand));
    bool waiting=false;
    for(unsigned i=0;i<5000 && !waiting;++i) { waiting=glm_pipeline_statistics().waited==1;if(!waiting)usleep(100); }
    assert(waiting);
    [dev->condition lock];assert(dev->calls==2);dev->gated=false;
    [dev->condition broadcast];[dev->condition unlock];
    pthread_join(waiter,NULL);[compiler waitUntilAllOperationsAreFinished];
    assert(demand.state && dev->calls==2 && stats.reused==1);
    assert(glm_pipeline_acquire((id<MTLDevice>)dev,demand.descriptor,false,NULL)==demand.state);
    /* A different active input type must not inherit an incompatible recipe. */
    dev=train();FakeFunction *other=(FakeFunction *)function();
    ((FakeAttribute *)other.vertexAttributes[0]).attributeType=MTLDataTypeInt4;
    glm_pipeline_prewarm((id<MTLDevice>)dev,(id<MTLFunction>)other,fragment());
    [compiler waitUntilAllOperationsAreFinished];assert(dev->calls==1 && !stats.predicted);
    /* Failed speculation never poisons a later demand. */
    dev=train();dev->fail_next=true;v=function();f=fragment();
    glm_pipeline_prewarm((id<MTLDevice>)dev,v,f);[compiler waitUntilAllOperationsAreFinished];
    assert(dev->calls==2 && !pending);
    NSError *error=nil;
    assert(glm_pipeline_acquire((id<MTLDevice>)dev,descriptor(v,f),false,&error));
    assert(!error && dev->calls==3);
    /* A new common configuration can replace the initial four recipes. */
    reset();dev=[FakeDevice new];v=function();f=fragment();
    const MTLColorWriteMask masks[]={MTLColorWriteMaskRed,MTLColorWriteMaskGreen,
        MTLColorWriteMaskBlue,MTLColorWriteMaskAlpha,MTLColorWriteMaskAll};
    for(unsigned i=0;i<5;++i) {
        MTLRenderPipelineDescriptor *d=descriptor(v,f);d.colorAttachments[0].writeMask=masks[i];
        assert(glm_pipeline_acquire((id<MTLDevice>)dev,d,true,NULL));
    }
    assert(glm_pipeline_acquire((id<MTLDevice>)dev,descriptor(v,f),true,NULL));
    v=function();f=fragment();glm_pipeline_prewarm((id<MTLDevice>)dev,v,f);
    [compiler waitUntilAllOperationsAreFinished];
    unsigned adapted=dev->calls;
    assert(glm_pipeline_acquire((id<MTLDevice>)dev,descriptor(v,f),false,NULL));
    assert(dev->calls==adapted && stats.reused==1);
    /* Unfamiliar and ambiguous recipes must not start compiler work. */
    reset();dev=[FakeDevice new];v=function();f=fragment();
    MTLRenderPipelineDescriptor *one=descriptor(v,f),*two=[one copy];
    two.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
    assert(glm_pipeline_acquire((id<MTLDevice>)dev,one,true,NULL));
    glm_pipeline_prewarm((id<MTLDevice>)dev,function(),fragment());
    assert(!stats.predicted);
    assert(glm_pipeline_acquire((id<MTLDevice>)dev,two,true,NULL));
    glm_pipeline_prewarm((id<MTLDevice>)dev,function(),fragment());
    assert(!stats.predicted);
    assert(glm_pipeline_acquire((id<MTLDevice>)dev,one,true,NULL));
    glm_pipeline_prewarm((id<MTLDevice>)dev,function(),fragment());
    [compiler waitUntilAllOperationsAreFinished];assert(stats.predicted==1);
    /* Outstanding predictions and background concurrency have hard bounds. */
    dev=train();compiler.suspended=YES;dev->gated=true;
    for(unsigned i=0;i<PENDING_LIMIT+3;++i) glm_pipeline_prewarm((id<MTLDevice>)dev,function(),fragment());
    assert(pending==PENDING_LIMIT && stats.dropped==3);
    compiler.suspended=NO;
    [dev->condition lock];
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:5];
    while(dev->active<2) assert([dev->condition waitUntilDate:deadline]);
    assert(dev->peak==2);dev->gated=false;[dev->condition broadcast];[dev->condition unlock];
    [compiler waitUntilAllOperationsAreFinished];assert(dev->peak==2 && !pending);
    /* Completed entries evict; a retained state remains valid after eviction. */
    reset();dev=[FakeDevice new];
    MTLRenderPipelineDescriptor *first=descriptor(function(),fragment());
    id<MTLRenderPipelineState> kept=glm_pipeline_acquire((id<MTLDevice>)dev,first,false,NULL);
    for(unsigned i=0;i<CACHE_LIMIT+4;++i) assert(glm_pipeline_acquire((id<MTLDevice>)dev,descriptor(function(),fragment()),false,NULL));
    assert(cache.count==CACHE_LIMIT && !cache[descriptor_key(first)] && kept);
    unsigned before=dev->calls;assert(glm_pipeline_acquire((id<MTLDevice>)dev,first,false,NULL));
    assert(dev->calls==before+1);
    reset();puts("pipeline-prewarm CPU PASS (prediction, exact keys/signatures, queued/running demand, failure retry, limits, eviction)");
} return 0; }
