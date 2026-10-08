/* Program pipeline objects (ARB_separate_shader_objects). Metal pipelines
 * take every stage at once, so a bound pipeline draws with one combined
 * program linked from its stage programs' sources. Each stage's loose
 * uniforms are renamed glmsso<stage>_<name> in the combined program (two
 * stage programs may use the same name for different values) and copied
 * from their owners before draws. */
#include "programs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct glm_pipeline {
    GLuint name;
    GLuint programs[GLM_STAGE_COUNT]; /* by glm_stage; 0: none */
    GLuint active;
    bool validated;
    char *log;
    /* The combined program and what it was linked from. */
    struct glm_program *combined;
    bool combined_failed;
    struct glm_program *owners[GLM_STAGE_COUNT];
    uint64_t owner_generation[GLM_STAGE_COUNT];
    uint64_t owner_uniform_serial[GLM_STAGE_COUNT];
};

static const GLbitfield stage_bits[GLM_STAGE_COUNT] = {
    [GLM_STAGE_VERTEX] = GL_VERTEX_SHADER_BIT, [GLM_STAGE_FRAGMENT] = GL_FRAGMENT_SHADER_BIT,
    [GLM_STAGE_GEOMETRY] = GL_GEOMETRY_SHADER_BIT, [GLM_STAGE_TESS_CONTROL] = GL_TESS_CONTROL_SHADER_BIT,
    [GLM_STAGE_TESS_EVALUATION] = GL_TESS_EVALUATION_SHADER_BIT};

static struct glm_pipeline *pipeline_get(struct glm_context *ctx, GLuint name)
{
    return name ? glm_table_get(&ctx->pipelines, name) : NULL;
}

static void release_combined(struct glm_context *ctx, struct glm_pipeline *pl)
{
    struct glm_program *c = pl->combined;
    if (!c) return;
    (void)ctx;
    glm_compile_result_free(&c->result);
    glm_backend_program_release(c);
    free(c->globals);
    free(c->locations);
    free(c->log);
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) free(c->linked_sources[s]);
    free(c);
    pl->combined = NULL;
    __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
}

GLM_EXPORT void glGenProgramPipelines(GLsizei n, GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        struct glm_pipeline *pl = calloc(1, sizeof *pl);
        pl->name = glm_table_reserve(&ctx->pipelines);
        glm_table_set(&ctx->pipelines, pl->name, pl);
        names[i] = pl->name;
    }
}

GLM_EXPORT void glDeleteProgramPipelines(GLsizei n, const GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        struct glm_pipeline *pl = pipeline_get(ctx, names[i]);
        if (!pl) continue;
        if (ctx->bound_pipeline == pl->name) ctx->bound_pipeline = 0;
        release_combined(ctx, pl);
        free(pl->log);
        glm_table_set(&ctx->pipelines, pl->name, NULL);
        free(pl);
    }
}

GLM_EXPORT GLboolean glIsProgramPipeline(GLuint name)
{
    GLM_CONTEXT(ctx, GL_FALSE);
    return pipeline_get(ctx, name) != NULL;
}

GLM_EXPORT void glBindProgramPipeline(GLuint name)
{
    GLM_CONTEXT(ctx);
    if (name && !pipeline_get(ctx, name)) return glm_error(ctx, GL_INVALID_OPERATION);
    ctx->bound_pipeline = name;
}

GLM_EXPORT void glUseProgramStages(GLuint name, GLbitfield stages, GLuint program)
{
    GLM_CONTEXT(ctx);
    struct glm_pipeline *pl = pipeline_get(ctx, name);
    if (!pl) return glm_error(ctx, GL_INVALID_OPERATION);
    if (stages != GL_ALL_SHADER_BITS && (stages & ~(GLbitfield)0x1f)) return glm_error(ctx, GL_INVALID_VALUE);
    if (program) {
        struct glm_program *p = glm_program_get(ctx, program);
        if (!p) return glm_error(ctx, GL_INVALID_VALUE);
        if (!p->separable || !p->linked) return glm_error(ctx, GL_INVALID_OPERATION);
    }
    for (int s = 0; s < GLM_STAGE_COUNT; ++s)
        if (stage_bits[s] & stages) pl->programs[s] = program;
}

GLM_EXPORT void glActiveShaderProgram(GLuint name, GLuint program)
{
    GLM_CONTEXT(ctx);
    struct glm_pipeline *pl = pipeline_get(ctx, name);
    if (!pl) return glm_error(ctx, GL_INVALID_OPERATION);
    struct glm_program *p = program ? glm_program_get(ctx, program) : NULL;
    if (program && !p) return glm_error(ctx, GL_INVALID_VALUE);
    if (p && !p->linked) return glm_error(ctx, GL_INVALID_OPERATION);
    pl->active = program;
}

struct glm_program *glm_pipeline_active_program(struct glm_context *ctx)
{
    struct glm_pipeline *pl = pipeline_get(ctx, ctx->bound_pipeline);
    return pl && pl->active ? glm_program_get(ctx, pl->active) : NULL;
}

/* The program providing `stage`, if it has code for it. */
static struct glm_program *stage_owner(struct glm_context *ctx, struct glm_pipeline *pl, int stage)
{
    struct glm_program *p = pl->programs[stage] ? glm_program_get(ctx, pl->programs[stage]) : NULL;
    return p && p->linked && p->linked_sources[stage] ? p : NULL;
}

struct glm_program *glm_pipeline_stage_program(struct glm_context *ctx, int stage)
{
    struct glm_pipeline *pl = pipeline_get(ctx, ctx->bound_pipeline);
    return pl ? stage_owner(ctx, pl, stage) : NULL;
}

/* The owner's uniform base names ("lights" for "lights[0].color"). */
static int uniform_bases(const struct glm_program *p, char ***out)
{
    int count = 0;
    char **names = calloc((size_t)p->result.uniform_count + 1, sizeof *names);
    for (int i = 0; i < p->result.uniform_count; ++i) {
        const char *name = p->result.uniforms[i].name;
        if (!strncmp(name, "glm_", 4) || !strncmp(name, "gl_", 3)) continue;
        size_t n = strcspn(name, "[.");
        bool seen = false;
        for (int k = 0; k < count; ++k)
            if (strlen(names[k]) == n && !strncmp(names[k], name, n)) seen = true;
        if (seen) continue;
        names[count] = malloc(n + 1);
        memcpy(names[count], name, n);
        names[count][n] = 0;
        ++count;
    }
    /* Subroutine uniforms (the compiler makes them glm_sub_<name>). */
    names = realloc(names, ((size_t)count + (size_t)p->result.subroutine_uniform_count + 1) * sizeof *names);
    for (int i = 0; i < p->result.subroutine_uniform_count; ++i) {
        const char *name = p->result.subroutine_uniforms[i].name;
        bool seen = false;
        for (int k = 0; k < count; ++k) seen |= !strcmp(names[k], name);
        if (!seen) names[count++] = strdup(name);
    }
    *out = names;
    return count;
}

static bool link_combined(struct glm_context *ctx, struct glm_pipeline *pl)
{
    char *sources[GLM_STAGE_COUNT] = {0};
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
        struct glm_program *owner = pl->owners[s];
        if (!owner) continue;
        char **from;
        int count = uniform_bases(owner, &from);
        char **to = calloc((size_t)count + 1, sizeof *to);
        for (int i = 0; i < count; ++i) {
            size_t n = strlen(from[i]) + 16;
            to[i] = malloc(n);
            snprintf(to[i], n, "glmsso%d_%s", s, from[i]);
        }
        sources[s] = glm_glsl_rename(owner->linked_sources[s], (const char *const *)from, (const char *const *)to, count);
        for (int i = 0; i < count; ++i) {
            free(from[i]);
            free(to[i]);
        }
        free(from);
        free(to);
    }
    struct glm_program *c = calloc(1, sizeof *c);
    /* Attribute and output bindings of the programs providing those ends. */
    if (pl->owners[GLM_STAGE_VERTEX]) {
        c->attribute_bindings = pl->owners[GLM_STAGE_VERTEX]->attribute_bindings;
        c->attribute_binding_count = pl->owners[GLM_STAGE_VERTEX]->attribute_binding_count;
    }
    if (pl->owners[GLM_STAGE_FRAGMENT]) {
        c->output_bindings = pl->owners[GLM_STAGE_FRAGMENT]->output_bindings;
        c->output_binding_count = pl->owners[GLM_STAGE_FRAGMENT]->output_binding_count;
    }
    bool ok = glm_program_link_sources(ctx, c, (const char *const *)sources);
    c->attribute_bindings = NULL;
    c->output_bindings = NULL;
    c->attribute_binding_count = c->output_binding_count = 0;
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) free(sources[s]);
    free(pl->log);
    pl->log = c->log ? strdup(c->log) : NULL;
    if (!ok) {
        pl->combined = c;
        release_combined(ctx, pl);
        return false;
    }
    pl->combined = c;
    return true;
}

/* Copies each stage owner's uniform values, sampler units and block
   bindings into the combined program. */
static void sync_uniforms(struct glm_context *ctx, struct glm_pipeline *pl, bool force)
{
    struct glm_program *c = pl->combined;
    bool changed = force;
    for (int s = 0; s < GLM_STAGE_COUNT; ++s)
        if (pl->owners[s] && pl->owners[s]->uniform_serial != pl->owner_uniform_serial[s]) changed = true;
    if (!changed) return;
    bool samplers_changed = false;
    for (int i = 0; i < c->result.uniform_count; ++i) {
        const struct glm_uniform_info *u = &c->result.uniforms[i];
        int stage;
        int prefix = 0;
        /* glmsso<stage>_<name>, or glm_sub_glmsso<stage>_<name> for subroutine uniforms. */
        bool subroutine = !strncmp(u->name, "glm_sub_", 8);
        const char *renamed = subroutine ? u->name + 8 : u->name;
        if (sscanf(renamed, "glmsso%d_%n", &stage, &prefix) != 1 || !prefix || stage < 0 || stage >= GLM_STAGE_COUNT)
            continue;
        const struct glm_program *owner = pl->owners[stage];
        if (!owner) continue;
        char original[300];
        snprintf(original, sizeof original, "%s%s", subroutine ? "glm_sub_" : "", renamed + prefix);
        for (int k = 0; k < owner->result.uniform_count; ++k) {
            const struct glm_uniform_info *o = &owner->result.uniforms[k];
            if (strcmp(o->name, original)) continue;
            if (u->sampler_slot >= 0 && o->sampler_slot >= 0) {
                for (int e = 0; e < u->array_size && u->sampler_slot + e < 64 && o->sampler_slot + e < 64; ++e) {
                    int unit = owner->sampler_units[o->sampler_slot + e];
                    if (c->sampler_units[u->sampler_slot + e] != unit) samplers_changed = true;
                    c->sampler_units[u->sampler_slot + e] = unit;
                }
            } else if (u->offset >= 0 && o->offset >= 0) {
                int stride = u->array_stride ? u->array_stride : 16;
                size_t bytes = (size_t)(u->array_size > 1 ? u->array_size * stride : stride);
                if (u->matrix_stride) bytes = (size_t)(u->array_size > 1 ? u->array_size * stride : 4 * u->matrix_stride);
                size_t limit = (size_t)c->result.global_size - (size_t)u->offset;
                size_t source_limit = (size_t)owner->result.global_size - (size_t)o->offset;
                if (bytes > limit) bytes = limit;
                if (bytes > source_limit) bytes = source_limit;
                memcpy(c->globals + u->offset, owner->globals + o->offset, bytes);
            }
            break;
        }
    }
    /* Uniform blocks: the binding the first stage program using the block set. */
    for (int b = 0; b < c->result.block_count && b < GLM_SLOT_UBO_COUNT; ++b)
        for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
            const struct glm_program *owner = pl->owners[s];
            if (!owner) continue;
            bool found = false;
            for (int k = 0; k < owner->result.block_count && k < GLM_SLOT_UBO_COUNT; ++k)
                if (!strcmp(owner->result.blocks[k].name, c->result.blocks[b].name)) {
                    if (c->block_bindings[b] != owner->block_bindings[k]) samplers_changed = true;
                    c->block_bindings[b] = owner->block_bindings[k];
                    found = true;
                    break;
                }
            if (found) break;
        }
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) pl->owner_uniform_serial[s] = pl->owners[s] ? pl->owners[s]->uniform_serial : 0;
    c->globals_dirty = true;
    if (samplers_changed) __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
}

struct glm_program *glm_pipeline_program(struct glm_context *ctx)
{
    struct glm_pipeline *pl = pipeline_get(ctx, ctx->bound_pipeline);
    if (!pl) return NULL;
    bool stale = false;
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
        struct glm_program *owner = stage_owner(ctx, pl, s);
        if (owner != pl->owners[s] || (owner && owner->generation != pl->owner_generation[s])) stale = true;
        pl->owners[s] = owner;
        pl->owner_generation[s] = owner ? owner->generation : 0;
    }
    if (stale) {
        release_combined(ctx, pl);
        pl->combined_failed = false;
    }
    if (!pl->combined && !pl->combined_failed) {
        bool any = false;
        for (int s = 0; s < GLM_STAGE_COUNT; ++s) any |= pl->owners[s] != NULL;
        if (!any || !link_combined(ctx, pl)) {
            pl->combined_failed = true;
            return NULL;
        }
        sync_uniforms(ctx, pl, true);
        return pl->combined;
    }
    if (pl->combined) sync_uniforms(ctx, pl, false);
    return pl->combined;
}

GLM_EXPORT void glValidateProgramPipeline(GLuint name)
{
    GLM_CONTEXT(ctx);
    struct glm_pipeline *pl = pipeline_get(ctx, name);
    if (!pl) return glm_error(ctx, GL_INVALID_OPERATION);
    GLuint bound = ctx->bound_pipeline;
    ctx->bound_pipeline = name;
    pl->validated = glm_pipeline_program(ctx) != NULL;
    ctx->bound_pipeline = bound;
}

GLM_EXPORT void glGetProgramPipelineiv(GLuint name, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    struct glm_pipeline *pl = pipeline_get(ctx, name);
    if (!pl) return glm_error(ctx, GL_INVALID_OPERATION);
    switch (pname) {
    case GL_ACTIVE_PROGRAM: *params = (GLint)pl->active; break;
    case GL_VERTEX_SHADER: *params = (GLint)pl->programs[GLM_STAGE_VERTEX]; break;
    case GL_FRAGMENT_SHADER: *params = (GLint)pl->programs[GLM_STAGE_FRAGMENT]; break;
    case GL_GEOMETRY_SHADER: *params = (GLint)pl->programs[GLM_STAGE_GEOMETRY]; break;
    case GL_TESS_CONTROL_SHADER: *params = (GLint)pl->programs[GLM_STAGE_TESS_CONTROL]; break;
    case GL_TESS_EVALUATION_SHADER: *params = (GLint)pl->programs[GLM_STAGE_TESS_EVALUATION]; break;
    case GL_VALIDATE_STATUS: *params = pl->validated; break;
    case GL_INFO_LOG_LENGTH: *params = pl->log && *pl->log ? (GLint)strlen(pl->log) + 1 : 0; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}

GLM_EXPORT void glGetProgramPipelineInfoLog(GLuint name, GLsizei size, GLsizei *length, GLchar *log)
{
    GLM_CONTEXT(ctx);
    struct glm_pipeline *pl = pipeline_get(ctx, name);
    if (!pl) return glm_error(ctx, GL_INVALID_VALUE);
    const char *text = pl->log ? pl->log : "";
    GLsizei n = (GLsizei)strlen(text);
    if (size > 0) {
        if (n > size - 1) n = size - 1;
        memcpy(log, text, (size_t)n);
        log[n] = 0;
    } else {
        n = 0;
    }
    if (length) *length = n;
}
