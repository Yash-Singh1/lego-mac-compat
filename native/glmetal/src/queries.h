/* Query objects (occlusion, time elapsed, primitives) shared by queries.c
 * and the Metal backend. */
#ifndef GLM_QUERIES_H
#define GLM_QUERIES_H

#include "glm_internal.h"

enum glm_query_class { GLM_QUERY_OCCLUSION, GLM_QUERY_TIME, GLM_QUERY_PRIMITIVES, GLM_QUERY_FEEDBACK, GLM_QUERY_CLASSES };

struct glm_query {
    GLuint name;
    GLenum target;
    GLuint index;
    bool active, deleted; /* deleted active objects survive until EndQuery */
    uint64_t serial;       /* command buffer serial that holds the last work */
    uint64_t counter;      /* CPU-side result part (time, primitives) */
    /* Occlusion: one visibility counter per render pass the query spans. */
    int segment_count, segment_capacity;
    void **buffers;        /* id<MTLBuffer>, retained */
    uint32_t *offsets;
    uint64_t pass_id;      /* backend pass of the last segment */
};

GLM_HIDDEN void glm_queries_destroy(struct glm_context *ctx);

GLM_HIDDEN void glm_query_add_segment(struct glm_query *q, void *retained_buffer, uint32_t offset);
/* Backend: called before a draw while an occlusion query is active. */
GLM_HIDDEN void glm_backend_occlusion_draw(struct glm_context *ctx, struct glm_query *q);
GLM_HIDDEN void glm_backend_occlusion_end(struct glm_context *ctx);
GLM_HIDDEN uint64_t glm_backend_pending_serial(struct glm_context *ctx);
GLM_HIDDEN uint64_t glm_backend_buffer_read_u64(void *buffer, uint32_t offset);
/* Counts primitives for active primitive queries. */
GLM_HIDDEN void glm_query_count_primitives(struct glm_context *ctx, uint32_t metal_primitive, uint32_t vertices,
                                           uint32_t instances);

#endif
