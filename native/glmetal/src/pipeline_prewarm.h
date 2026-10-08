#ifndef GLM_PIPELINE_PREWARM_H
#define GLM_PIPELINE_PREWARM_H
#import <Metal/Metal.h>

/* Descriptors constructed by metal_backend.m's pipeline_for only. Cache keys
   include the complete vertex layout, not a context-local layout number. */
id<MTLRenderPipelineState> glm_pipeline_acquire(id<MTLDevice> device,
    MTLRenderPipelineDescriptor *descriptor, bool learn, NSError **error);
void glm_pipeline_prewarm(id<MTLDevice> device, id<MTLFunction> vertex, id<MTLFunction> fragment);

struct glm_pipeline_stats { unsigned long long predicted, reused, demanded, waited, dropped; };
struct glm_pipeline_stats glm_pipeline_statistics(void);
#endif
