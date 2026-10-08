/* Shader and program objects: GLSL through shader_compiler.cpp, uniforms in
 * a CPU copy of the program's global uniform block, and the GL queries. */
#include "programs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shaders and programs share one name space, as in GL. */
static struct glm_object_table *objects(struct glm_context *ctx) { return &ctx->share->shaders; }

static void *object_get(struct glm_context *ctx, GLuint name, bool program)
{
    void *item = glm_table_get(objects(ctx), name);
    if (!item) return NULL;
    bool is_program = glm_table_get(&ctx->share->programs, name) != NULL;
    return is_program == program ? item : NULL;
}

struct glm_program *glm_program_get(struct glm_context *ctx, GLuint name) { return object_get(ctx, name, true); }
static struct glm_shader *shader_get(struct glm_context *ctx, GLuint name) { return object_get(ctx, name, false); }

struct glm_program *glm_current_program(struct glm_context *ctx)
{
    if (!ctx->current_program) return ctx->bound_pipeline ? glm_pipeline_program(ctx) : NULL;
    uint64_t generation = ctx->share->program_generation;
    if (ctx->program_cache_name != ctx->current_program || ctx->program_cache_generation != generation) {
        ctx->program_cache = glm_program_get(ctx, ctx->current_program);
        ctx->program_cache_name = ctx->current_program;
        ctx->program_cache_generation = generation;
    }
    struct glm_program *p = ctx->program_cache;
    return p && p->linked ? p : NULL;
}

static char *duplicate(const char *s)
{
    size_t n = strlen(s ? s : "") + 1;
    char *out = malloc(n);
    memcpy(out, s ? s : "", n);
    return out;
}

/* ---- shaders ------------------------------------------------------------- */

static enum glm_stage stage_of(GLenum type)
{
    switch (type) {
    case GL_FRAGMENT_SHADER: return GLM_STAGE_FRAGMENT;
    case GL_GEOMETRY_SHADER: return GLM_STAGE_GEOMETRY;
    case GL_TESS_CONTROL_SHADER: return GLM_STAGE_TESS_CONTROL;
    case GL_TESS_EVALUATION_SHADER: return GLM_STAGE_TESS_EVALUATION;
    default: return GLM_STAGE_VERTEX;
    }
}

GLM_EXPORT GLuint glCreateShader(GLenum type)
{
    GLM_CONTEXT(ctx, 0);
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER && type != GL_GEOMETRY_SHADER &&
        type != GL_TESS_CONTROL_SHADER && type != GL_TESS_EVALUATION_SHADER)
        return glm_error(ctx, GL_INVALID_ENUM), 0;
    pthread_mutex_lock(&ctx->share->lock);
    GLuint name = glm_table_reserve(objects(ctx));
    struct glm_shader *shader = calloc(1, sizeof *shader);
    shader->name = name;
    shader->type = type;
    glm_table_set(objects(ctx), name, shader);
    pthread_mutex_unlock(&ctx->share->lock);
    return name;
}

GLM_EXPORT void glShaderSource(GLuint name, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
    GLM_CONTEXT(ctx);
    struct glm_shader *shader = shader_get(ctx, name);
    if (!shader) return glm_error(ctx, GL_INVALID_VALUE);
    size_t total = 0;
    for (GLsizei i = 0; i < count; ++i)
        total += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
    char *source = malloc(total + 1);
    size_t at = 0;
    for (GLsizei i = 0; i < count; ++i) {
        size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(source + at, strings[i], n);
        at += n;
    }
    source[at] = 0;
    free(shader->source);
    shader->source = source;
}

GLM_EXPORT void glCompileShader(GLuint name)
{
    GLM_CONTEXT(ctx);
    struct glm_shader *shader = shader_get(ctx, name);
    if (!shader) return glm_error(ctx, GL_INVALID_VALUE);
    free(shader->log);
    shader->log = NULL;
    char *version_error = glm_shader_version_error(shader->source, ctx->profile == GLM_PROFILE_CORE);
    if (version_error) {
        shader->compiled = false;
        shader->log = version_error;
        return;
    }
    shader->compiled = glm_shader_check(stage_of(shader->type), shader->source, &shader->log);
    if (!shader->compiled) glm_log("shader %u failed to compile:\n%s\n%s", name, shader->log, shader->source ? shader->source : "");
}

static void free_shader(struct glm_context *ctx, struct glm_shader *shader)
{
    __atomic_add_fetch(&ctx->share->program_generation, 1, __ATOMIC_RELEASE);
    glm_table_set(objects(ctx), shader->name, NULL);
    free(shader->source);
    free(shader->log);
    free(shader);
}

GLM_EXPORT void glDeleteShader(GLuint name)
{
    GLM_CONTEXT(ctx);
    struct glm_shader *shader = shader_get(ctx, name);
    if (!shader) return;
    if (shader->attached) shader->delete_pending = true;
    else free_shader(ctx, shader);
}

GLM_EXPORT GLboolean glIsShader(GLuint name) { GLM_CONTEXT(ctx, GL_FALSE); return shader_get(ctx, name) != NULL; }

/* glGetProgramiv's names in this profile (Apple's: build/probes/apierr.c).
   glGetShaderiv answers them with GL_INVALID_OPERATION. */
static bool program_pname(const struct glm_context *ctx, GLenum pname)
{
    bool core = ctx->profile == GLM_PROFILE_CORE;
    switch (pname) {
    case GL_LINK_STATUS: case GL_VALIDATE_STATUS: case GL_ATTACHED_SHADERS: case GL_ACTIVE_UNIFORMS:
    case GL_ACTIVE_UNIFORM_MAX_LENGTH: case GL_ACTIVE_ATTRIBUTES: case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
    case GL_ACTIVE_UNIFORM_BLOCKS: case GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH: case GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH:
    case GL_TRANSFORM_FEEDBACK_BUFFER_MODE: case GL_TRANSFORM_FEEDBACK_VARYINGS: case GL_PROGRAM_BINARY_RETRIEVABLE_HINT:
    case GL_PROGRAM_SEPARABLE: case GL_PROGRAM_BINARY_LENGTH:
        return true;
    case GL_GEOMETRY_SHADER_INVOCATIONS: case GL_GEOMETRY_VERTICES_OUT: case GL_GEOMETRY_INPUT_TYPE: case GL_GEOMETRY_OUTPUT_TYPE:
    case GL_TESS_CONTROL_OUTPUT_VERTICES: case GL_TESS_GEN_MODE: case GL_TESS_GEN_SPACING: case GL_TESS_GEN_VERTEX_ORDER:
    case GL_TESS_GEN_POINT_MODE:
        return core;
    case 0x8DDA: case 0x8DDB: case 0x8DDC: /* GL_GEOMETRY_*_EXT */
        return !core;
    }
    return false;
}

GLM_EXPORT void glGetShaderiv(GLuint name, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    struct glm_shader *shader = shader_get(ctx, name);
    if (!shader) return glm_error(ctx, GL_INVALID_VALUE);
    if (program_pname(ctx, pname)) return glm_error(ctx, GL_INVALID_OPERATION);
    switch (pname) {
    case 0x8B4E: *params = (GLint)shader->type; break; /* GL_OBJECT_TYPE_ARB (Apple: the shader type) */
    case GL_SHADER_TYPE: *params = (GLint)shader->type; break;
    case GL_DELETE_STATUS: *params = shader->delete_pending; break;
    case GL_COMPILE_STATUS: *params = shader->compiled; break;
    case GL_INFO_LOG_LENGTH: *params = shader->log && *shader->log ? (GLint)strlen(shader->log) + 1 : 0; break;
    case GL_SHADER_SOURCE_LENGTH: *params = shader->source ? (GLint)strlen(shader->source) + 1 : 0; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}

static void copy_string(const char *text, GLsizei size, GLsizei *length, GLchar *out)
{
    if (size <= 0) {
        if (length) *length = 0;
        return;
    }
    size_t n = strlen(text ? text : "");
    if (n > (size_t)size - 1) n = (size_t)size - 1;
    memcpy(out, text ? text : "", n);
    out[n] = 0;
    if (length) *length = (GLsizei)n;
}

GLM_EXPORT void glGetShaderInfoLog(GLuint name, GLsizei size, GLsizei *length, GLchar *log)
{
    GLM_CONTEXT(ctx);
    struct glm_shader *shader = shader_get(ctx, name);
    if (!shader) return glm_error(ctx, GL_INVALID_VALUE);
    copy_string(shader->log, size, length, log);
}

GLM_EXPORT void glGetShaderSource(GLuint name, GLsizei size, GLsizei *length, GLchar *source)
{
    GLM_CONTEXT(ctx);
    struct glm_shader *shader = shader_get(ctx, name);
    if (!shader) return glm_error(ctx, GL_INVALID_VALUE);
    copy_string(shader->source, size, length, source);
}

/* ---- programs ------------------------------------------------------------ */

GLM_EXPORT GLuint glCreateProgram(void)
{
    GLM_CONTEXT(ctx, 0);
    pthread_mutex_lock(&ctx->share->lock);
    GLuint name = glm_table_reserve(objects(ctx));
    struct glm_program *program = calloc(1, sizeof *program);
    program->name = name;
    program->feedback_mode = GL_INTERLEAVED_ATTRIBS;
    program->ext_geometry[1] = GL_TRIANGLES;
    program->ext_geometry[2] = GL_TRIANGLE_STRIP;
    glm_table_set(objects(ctx), name, program);
    glm_table_set(&ctx->share->programs, name, program);
    pthread_mutex_unlock(&ctx->share->lock);
    return name;
}

/* The command stream creates shaders and programs under names the
   application's thread reserved (marshal_custom.c), so creation need not
   wait for the stream. */
void glm_create_shader_named(struct glm_context *ctx, GLenum type, GLuint name)
{
    pthread_mutex_lock(&ctx->share->lock);
    struct glm_shader *shader = calloc(1, sizeof *shader);
    shader->name = name;
    shader->type = type;
    glm_table_set(objects(ctx), name, shader);
    pthread_mutex_unlock(&ctx->share->lock);
}

void glm_create_program_named(struct glm_context *ctx, GLuint name)
{
    pthread_mutex_lock(&ctx->share->lock);
    struct glm_program *program = calloc(1, sizeof *program);
    program->name = name;
    program->feedback_mode = GL_INTERLEAVED_ATTRIBS;
    program->ext_geometry[1] = GL_TRIANGLES;
    program->ext_geometry[2] = GL_TRIANGLE_STRIP;
    glm_table_set(objects(ctx), name, program);
    glm_table_set(&ctx->share->programs, name, program);
    pthread_mutex_unlock(&ctx->share->lock);
}

/* glCompileShader whose check already ran on the application's thread. */
void glm_shader_set_compiled(struct glm_context *ctx, GLuint name, bool ok, const char *log)
{
    struct glm_shader *shader = shader_get(ctx, name);
    if (!shader) return glm_error(ctx, GL_INVALID_VALUE);
    free(shader->log);
    shader->log = log ? duplicate(log) : NULL;
    shader->compiled = ok;
}

GLM_EXPORT void glAttachShader(GLuint program_name, GLuint shader_name)
{
    GLM_CONTEXT(ctx);
    struct glm_program *program = glm_program_get(ctx, program_name);
    struct glm_shader *shader = shader_get(ctx, shader_name);
    if (!program || !shader) return glm_error(ctx, GL_INVALID_VALUE);
    for (int i = 0; i < program->shader_count; ++i)
        if (program->shaders[i] == shader_name) return glm_error(ctx, GL_INVALID_OPERATION);
    if (program->shader_count == 8) return glm_error(ctx, GL_INVALID_OPERATION);
    program->shaders[program->shader_count++] = shader_name;
    ++shader->attached;
}

GLM_EXPORT void glDetachShader(GLuint program_name, GLuint shader_name)
{
    GLM_CONTEXT(ctx);
    struct glm_program *program = glm_program_get(ctx, program_name);
    struct glm_shader *shader = shader_get(ctx, shader_name);
    if (!program || !shader) return glm_error(ctx, GL_INVALID_VALUE);
    for (int i = 0; i < program->shader_count; ++i) {
        if (program->shaders[i] != shader_name) continue;
        program->shaders[i] = program->shaders[--program->shader_count];
        if (--shader->attached == 0 && shader->delete_pending) free_shader(ctx, shader);
        return;
    }
    glm_error(ctx, GL_INVALID_OPERATION);
}

static void set_binding(struct glm_name_location **list, int *count, const char *name, int location)
{
    for (int i = 0; i < *count; ++i)
        if (!strcmp((*list)[i].name, name)) {
            (*list)[i].location = location;
            return;
        }
    *list = realloc(*list, (size_t)(*count + 1) * sizeof **list);
    (*list)[*count].name = duplicate(name);
    (*list)[*count].location = location;
    ++*count;
}

GLM_EXPORT void glBindAttribLocation(GLuint name, GLuint index, const GLchar *attribute)
{
    GLM_CONTEXT(ctx);
    struct glm_program *program = glm_program_get(ctx, name);
    if (!program) return glm_error(ctx, GL_INVALID_VALUE);
    if (index >= GLM_MAX_ATTRIBS) return glm_error(ctx, GL_INVALID_VALUE);
    if (!strncmp(attribute, "gl_", 3)) return glm_error(ctx, GL_INVALID_OPERATION);
    set_binding(&program->attribute_bindings, &program->attribute_binding_count, attribute, (int)index);
}

GLM_EXPORT void glBindFragDataLocation(GLuint name, GLuint color, const GLchar *output)
{
    GLM_CONTEXT(ctx);
    struct glm_program *program = glm_program_get(ctx, name);
    if (!program) return glm_error(ctx, GL_INVALID_VALUE);
    set_binding(&program->output_bindings, &program->output_binding_count, output, (int)color);
}
GLM_EXPORT void glBindFragDataLocationEXT(GLuint name, GLuint color, const GLchar *output) { glBindFragDataLocation(name, color, output); }

GLM_EXPORT void glBindFragDataLocationIndexed(GLuint name, GLuint color, GLuint index, const GLchar *output)
{
    GLM_CONTEXT(ctx);
    struct glm_program *program = glm_program_get(ctx, name);
    if (!program) return glm_error(ctx, GL_INVALID_VALUE);
    if (index > 1 || (index == 1 && color > 0) || color >= GLM_MAX_DRAW_BUFFERS) return glm_error(ctx, GL_INVALID_VALUE);
    if (!strncmp(output, "gl_", 3)) return glm_error(ctx, GL_INVALID_OPERATION);
    set_binding(&program->output_bindings, &program->output_binding_count, output, (int)color | (index ? GLM_OUTPUT_INDEX1 : 0));
}

GLM_EXPORT void glTransformFeedbackVaryings(GLuint name, GLsizei count, const GLchar *const *varyings, GLenum mode)
{
    GLM_CONTEXT(ctx);
    struct glm_program *program = glm_program_get(ctx, name);
    if (!program) return glm_error(ctx, GL_INVALID_VALUE);
    if (mode != GL_INTERLEAVED_ATTRIBS && mode != GL_SEPARATE_ATTRIBS) return glm_error(ctx, GL_INVALID_ENUM);
    if (count < 0 || (mode == GL_SEPARATE_ATTRIBS && count > 4)) return glm_error(ctx, GL_INVALID_VALUE);
    for (int i = 0; i < program->feedback_count; ++i) free(program->feedback_varyings[i]);
    free(program->feedback_varyings);
    program->feedback_varyings = calloc((size_t)(count + 1), sizeof(char *));
    for (GLsizei i = 0; i < count; ++i) program->feedback_varyings[i] = duplicate(varyings[i]);
    program->feedback_count = count;
    program->feedback_mode = mode;
}

/* Lays out GL locations: consecutive per array element, internal (glm_*)
   uniforms excluded. */
static void build_locations(struct glm_program *p)
{
    free(p->locations);
    p->locations = NULL;
    p->location_count = 0;
    int total = 0;
    for (int i = 0; i < p->result.uniform_count; ++i)
        if (strncmp(p->result.uniforms[i].name, "glm_", 4)) total += p->result.uniforms[i].array_size;
    p->locations = calloc((size_t)(total + 1), sizeof *p->locations);
    for (int i = 0; i < p->result.uniform_count; ++i) {
        if (!strncmp(p->result.uniforms[i].name, "glm_", 4)) continue;
        for (int e = 0; e < p->result.uniforms[i].array_size; ++e)
            p->locations[p->location_count++] = (struct glm_location){i, e};
    }
}

GLM_EXPORT void glLinkProgram(GLuint name)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p) return glm_error(ctx, GL_INVALID_VALUE);
    const char *sources[GLM_STAGE_COUNT] = {0};
    for (int i = 0; i < p->shader_count; ++i) {
        struct glm_shader *shader = shader_get(ctx, p->shaders[i]);
        if (!shader || !shader->source) continue;
        sources[stage_of(shader->type)] = shader->source;
    }
    glm_program_link_sources(ctx, p, sources);
}

bool glm_program_link_sources(struct glm_context *ctx, struct glm_program *p, const char *const *sources)
{
    GLuint name = p->name;
    struct glm_compile_request request = {0};
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) request.sources[s] = sources[s];
    request.attributes = p->attribute_bindings;
    request.attribute_count = p->attribute_binding_count;
    request.frag_outputs = p->output_bindings;
    request.frag_output_count = p->output_binding_count;
    request.feedback_varyings = (const char *const *)p->feedback_varyings;
    request.feedback_count = p->feedback_count;
    request.feedback_interleaved = p->feedback_mode == GL_INTERLEAVED_ATTRIBS;
    struct glm_compile_result result = {0};
    uint64_t started = glm_now_ns();
    if (request.feedback_count && !sources[GLM_STAGE_VERTEX] &&
        !sources[GLM_STAGE_GEOMETRY] && !sources[GLM_STAGE_TESS_EVALUATION]) {
        /* Fragment-only executables have no outputs available for capture.
           Report a link failure through the normal failed-relink path. */
        result.log = duplicate("transform feedback requires a vertex-processing shader stage\n");
    } else {
        glm_program_compile(&request, &result);
    }
    glm_note_stall("program compile (glslang + SPIRV-Cross)", started);
    char *backend_log = NULL;
    bool ok = result.ok;
    struct glm_compile_result old = p->result;
    p->result = result;
    if (ok) ok = glm_backend_program_link(ctx, p, &backend_log);
    glm_note_stall("program link", started);
    if (!ok) {
        /* A failed link keeps the program's last successful executable in
           use (GL 2.1 2.15.2), but reports failure. */
        glm_log("program %u failed to link:\n%s%s", name, result.log ? result.log : "", backend_log ? backend_log : "");
        free(p->log);
        size_t n = strlen(result.log ? result.log : "") + strlen(backend_log ? backend_log : "") + 1;
        p->log = malloc(n);
        snprintf(p->log, n, "%s%s", result.log ? result.log : "", backend_log ? backend_log : "");
        glm_compile_result_free(&p->result);
        p->result = old;
        p->linked = false;
        free(backend_log);
        return false;
    }
    glm_compile_result_free(&old);
    free(p->log);
    p->log = duplicate(result.log);
    free(p->globals);
    p->globals = calloc(1, (size_t)(result.global_size ? result.global_size : 16));
    if (result.initial_globals && result.global_size) memcpy(p->globals, result.initial_globals, (size_t)result.global_size);
    memset(p->sampler_units, 0, sizeof p->sampler_units);
    for (int i = 0; i < GLM_SLOT_UBO_COUNT; ++i) p->block_bindings[i] = 0;
    build_locations(p);
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
        free(p->linked_sources[s]);
        p->linked_sources[s] = sources[s] ? duplicate(sources[s]) : NULL;
    }
    const char *fragment_msl = p->result.msl[GLM_STAGE_FRAGMENT];
    p->color_inputs = fragment_msl && (strstr(fragment_msl, " glm_Color [[user") || strstr(fragment_msl, " glm_SecondaryColor [[user"));
    p->linked = true;
    ++p->generation;
    ++p->uniform_serial;
    return true;
}

GLM_EXPORT void glUseProgram(GLuint name)
{
    GLM_CONTEXT(ctx);
    if (name && !glm_program_get(ctx, name)) return glm_error(ctx, GL_INVALID_VALUE);
    ctx->current_program = name;
}

void glm_program_free(struct glm_context *ctx, struct glm_program *p)
{
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) free(p->linked_sources[s]);
    __atomic_add_fetch(&ctx->share->program_generation, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < p->shader_count; ++i) {
        struct glm_shader *shader = shader_get(ctx, p->shaders[i]);
        if (shader && --shader->attached == 0 && shader->delete_pending) free_shader(ctx, shader);
    }
    glm_table_set(objects(ctx), p->name, NULL);
    glm_table_set(&ctx->share->programs, p->name, NULL);
    glm_compile_result_free(&p->result);
    glm_backend_program_release(p);
    free(p->globals);
    free(p->locations);
    free(p->log);
    for (int i = 0; i < p->attribute_binding_count; ++i) free((char *)p->attribute_bindings[i].name);
    for (int i = 0; i < p->output_binding_count; ++i) free((char *)p->output_bindings[i].name);
    free(p->attribute_bindings);
    free(p->output_bindings);
    for (int i = 0; i < p->feedback_count; ++i) free(p->feedback_varyings[i]);
    free(p->feedback_varyings);
    free(p);
}

GLM_EXPORT void glDeleteProgram(GLuint name)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p) return;
    if (ctx->current_program == name) p->delete_pending = true;
    else glm_program_free(ctx, p);
}

GLM_EXPORT GLboolean glIsProgram(GLuint name) { GLM_CONTEXT(ctx, GL_FALSE); return glm_program_get(ctx, name) != NULL; }

GLM_EXPORT void glValidateProgram(GLuint name)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p) return glm_error(ctx, GL_INVALID_VALUE);
    p->validated = p->linked;
}

static int max_name_length(struct glm_program *p, bool attributes)
{
    int longest = 0;
    if (attributes) {
        for (int i = 0; i < p->result.attribute_count; ++i)
            if (p->result.attributes[i].index != GLM_INPUT_INACTIVE)
                longest = (int)strlen(p->result.attributes[i].name) + 1 > longest ? (int)strlen(p->result.attributes[i].name) + 1 : longest;
    } else {
        for (int i = 0; i < p->result.uniform_count; ++i) {
            int n = (int)strlen(p->result.uniforms[i].name) + 1 + (p->result.uniforms[i].array_size > 1 || p->result.uniforms[i].is_array ? 3 : 0);
            if (n > longest) longest = n;
        }
        for (int i = 0; i < p->result.block_uniform_count; ++i) {
            int n = (int)strlen(p->result.block_uniforms[i].name) + 1 + (p->result.block_uniforms[i].array_size > 1 || p->result.block_uniforms[i].is_array ? 3 : 0);
            if (n > longest) longest = n;
        }
    }
    return longest;
}

/* Active uniform `index` (GL numbering: default-block uniforms, then
   block members), or NULL. */
static const struct glm_uniform_info *active_uniform(const struct glm_program *p, GLuint index)
{
    GLuint seen = 0;
    for (int i = 0; i < p->result.uniform_count; ++i) {
        if (!strncmp(p->result.uniforms[i].name, "glm_", 4)) continue;
        if (seen++ == index) return &p->result.uniforms[i];
    }
    index -= seen;
    return index < (GLuint)p->result.block_uniform_count ? &p->result.block_uniforms[index] : NULL;
}

static int active_uniform_count(struct glm_program *p)
{
    int n = p->result.block_uniform_count;
    for (int i = 0; i < p->result.uniform_count; ++i)
        if (strncmp(p->result.uniforms[i].name, "glm_", 4)) ++n;
    return n;
}

static int active_attribute_count(const struct glm_program *p);

GLM_EXPORT void glGetProgramiv(GLuint name, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p) return glm_error(ctx, GL_INVALID_VALUE);
    const struct glm_gs_result *gs = p->linked ? p->result.gs : NULL;
    const struct glm_tess_result *tess = p->linked ? p->result.tess : NULL;
    bool core = ctx->profile == GLM_PROFILE_CORE;
    switch (pname) {
    case 0x8B4E: *params = 0x8B40; return; /* GL_OBJECT_TYPE_ARB: GL_PROGRAM_OBJECT_ARB */
    case 0x8B4F: return glm_error(ctx, GL_INVALID_OPERATION); /* GL_OBJECT_SUBTYPE_ARB */
    case GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH: {
        GLint n = 0;
        for (int i = 0; p->linked && i < p->result.block_count; ++i)
            if ((GLint)strlen(p->result.blocks[i].name) + 1 > n) n = (GLint)strlen(p->result.blocks[i].name) + 1;
        *params = n;
        return;
    }
    case GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH: {
        GLint n = 0;
        for (int i = 0; p->linked && i < p->feedback_count; ++i)
            if ((GLint)strlen(p->feedback_varyings[i]) + 1 > n) n = (GLint)strlen(p->feedback_varyings[i]) + 1;
        *params = n;
        return;
    }
    case GL_GEOMETRY_SHADER_INVOCATIONS: case GL_GEOMETRY_VERTICES_OUT: case GL_GEOMETRY_INPUT_TYPE: case GL_GEOMETRY_OUTPUT_TYPE:
        if (!core) break;
        if (!gs) return glm_error(ctx, GL_INVALID_OPERATION);
        switch (pname) {
        case GL_GEOMETRY_SHADER_INVOCATIONS: *params = gs->invocations; break;
        case GL_GEOMETRY_VERTICES_OUT: *params = gs->max_vertices; break;
        case GL_GEOMETRY_INPUT_TYPE: {
            static const GLenum inputs[7] = {0, GL_POINTS, GL_LINES, GL_TRIANGLES, GL_LINES_ADJACENCY, 0, GL_TRIANGLES_ADJACENCY};
            *params = gs->in_vertices > 0 && gs->in_vertices < 7 ? (GLint)inputs[gs->in_vertices] : 0;
            break;
        }
        default: *params = gs->out_primitive == 0 ? GL_POINTS : gs->out_primitive == 1 ? GL_LINE_STRIP : GL_TRIANGLE_STRIP;
        }
        return;
    case GL_TESS_CONTROL_OUTPUT_VERTICES: case GL_TESS_GEN_MODE: case GL_TESS_GEN_SPACING: case GL_TESS_GEN_VERTEX_ORDER:
    case GL_TESS_GEN_POINT_MODE:
        if (!core) break;
        if (!tess) return glm_error(ctx, GL_INVALID_OPERATION);
        switch (pname) {
        case GL_TESS_CONTROL_OUTPUT_VERTICES: *params = tess->out_vertices; break;
        case GL_TESS_GEN_MODE: *params = tess->quads ? GL_QUADS : GL_TRIANGLES; break;
        case GL_TESS_GEN_SPACING: /* MTLTessellationPartitionMode */
            *params = tess->partition == 2 ? GL_FRACTIONAL_ODD : tess->partition == 3 ? GL_FRACTIONAL_EVEN : GL_EQUAL;
            break;
        case GL_TESS_GEN_VERTEX_ORDER: *params = tess->cw ? GL_CW : GL_CCW; break;
        default: *params = tess->point_mode;
        }
        return;
    case 0x8DDA: case 0x8DDB: case 0x8DDC: /* GL_GEOMETRY_*_EXT */
        if (core) break;
        *params = p->ext_geometry[pname - 0x8DDA];
        return;
    }
    if (!glm_program_query_iv(p, pname, params)) glm_error(ctx, GL_INVALID_ENUM);
}

/* glGetProgramiv of `p` (also the application thread's shadow programs,
   marshal_custom.c); false for an unknown pname. */
bool glm_program_query_iv(struct glm_program *p, GLenum pname, GLint *params)
{
    switch (pname) {
    case GL_DELETE_STATUS: *params = p->delete_pending; break;
    case GL_LINK_STATUS: *params = p->linked; break;
    case GL_VALIDATE_STATUS: *params = p->validated; break;
    case GL_INFO_LOG_LENGTH: *params = p->log && *p->log ? (GLint)strlen(p->log) + 1 : 0; break;
    case GL_ATTACHED_SHADERS: *params = p->shader_count; break;
    case GL_ACTIVE_UNIFORMS: *params = p->linked ? active_uniform_count(p) : 0; break;
    case GL_ACTIVE_UNIFORM_MAX_LENGTH: *params = p->linked ? max_name_length(p, false) : 0; break;
    case GL_ACTIVE_ATTRIBUTES: *params = p->linked ? active_attribute_count(p) : 0; break;
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH: *params = p->linked ? max_name_length(p, true) : 0; break;
    case GL_ACTIVE_UNIFORM_BLOCKS: *params = p->linked ? p->result.block_count : 0; break;
    case GL_TRANSFORM_FEEDBACK_VARYINGS: *params = p->feedback_count; break;
    case GL_TRANSFORM_FEEDBACK_BUFFER_MODE: *params = (GLint)p->feedback_mode; break;
    case GL_PROGRAM_BINARY_RETRIEVABLE_HINT: *params = 0; break;
    case GL_PROGRAM_SEPARABLE: *params = p->separable; break;
    case GL_PROGRAM_BINARY_LENGTH: *params = 0; break;
    default: return false;
    }
    return true;
}

GLM_EXPORT void glGetProgramInfoLog(GLuint name, GLsizei size, GLsizei *length, GLchar *log)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p) return glm_error(ctx, GL_INVALID_VALUE);
    copy_string(p->log, size, length, log);
}

GLM_EXPORT void glGetAttachedShaders(GLuint name, GLsizei max, GLsizei *count, GLuint *shaders)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p) return glm_error(ctx, GL_INVALID_VALUE);
    GLsizei n = 0;
    for (int i = 0; i < p->shader_count && n < max; ++i) shaders[n++] = p->shaders[i];
    if (count) *count = n;
}

/* ---- attributes and outputs ---------------------------------------------- */

GLM_EXPORT GLint glGetAttribLocation(GLuint name, const GLchar *attribute)
{
    GLM_CONTEXT(ctx, -1);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p || !p->linked) return glm_error(ctx, GL_INVALID_OPERATION), -1;
    return glm_program_attrib_location(p, attribute);
}

GLint glm_program_attrib_location(const struct glm_program *p, const char *attribute)
{
    for (int i = 0; i < p->result.attribute_count; ++i)
        if (!strcmp(p->result.attributes[i].name, attribute))
            return p->result.attributes[i].index == GLM_INPUT_INACTIVE ? -1 : p->result.attributes[i].location;
    return -1;
}

/* The active attributes GL lists: unused inputs are not active. */
static int active_attribute_count(const struct glm_program *p)
{
    int n = 0;
    for (int i = 0; i < p->result.attribute_count; ++i) n += p->result.attributes[i].index != GLM_INPUT_INACTIVE;
    return n;
}
static const struct glm_io_info *active_attribute(const struct glm_program *p, GLuint index)
{
    for (int i = 0; i < p->result.attribute_count; ++i)
        if (p->result.attributes[i].index != GLM_INPUT_INACTIVE && index-- == 0) return &p->result.attributes[i];
    return NULL;
}

GLM_EXPORT void glGetActiveAttrib(GLuint name, GLuint index, GLsizei size, GLsizei *length, GLint *array_size, GLenum *type,
                                  GLchar *out)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    const struct glm_io_info *a = p ? active_attribute(p, index) : NULL;
    if (!a) return glm_error(ctx, GL_INVALID_VALUE);
    copy_string(a->name, size, length, out);
    if (array_size) *array_size = a->array_size;
    if (type) *type = a->type;
}

GLM_EXPORT GLint glGetFragDataLocation(GLuint name, const GLchar *output)
{
    GLM_CONTEXT(ctx, -1);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p || !p->linked) return glm_error(ctx, GL_INVALID_OPERATION), -1;
    for (int i = 0; i < p->result.output_count; ++i)
        if (!strcmp(p->result.outputs[i].name, output)) return p->result.outputs[i].location;
    return -1;
}
GLM_EXPORT GLint glGetFragDataLocationEXT(GLuint name, const GLchar *output) { return glGetFragDataLocation(name, output); }

GLM_EXPORT GLint glGetFragDataIndex(GLuint name, const GLchar *output)
{
    GLM_CONTEXT(ctx, -1);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p || !p->linked) return glm_error(ctx, GL_INVALID_OPERATION), -1;
    for (int i = 0; i < p->result.output_count; ++i)
        if (!strcmp(p->result.outputs[i].name, output)) return p->result.outputs[i].index;
    return -1;
}

/* ---- uniforms ------------------------------------------------------------ */

/* "name", "name[3]", "s[1].field". Returns -1 when absent. */
GLM_EXPORT GLint glGetUniformLocation(GLuint name, const GLchar *uniform)
{
    GLM_CONTEXT(ctx, -1);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p || !p->linked) return glm_error(ctx, GL_INVALID_OPERATION), -1;
    return glm_program_uniform_location(p, uniform);
}

GLint glm_program_uniform_location(const struct glm_program *p, const char *uniform)
{
    if (!strncmp(uniform, "gl_", 3)) return -1;
    char base[256];
    snprintf(base, sizeof base, "%s", uniform);
    int element = 0;
    size_t n = strlen(base);
    if (n && base[n - 1] == ']') {
        char *open = strrchr(base, '[');
        if (open) {
            element = atoi(open + 1);
            *open = 0;
        }
    }
    for (int loc = 0; loc < p->location_count; ++loc) {
        const struct glm_uniform_info *u = &p->result.uniforms[p->locations[loc].uniform];
        if (p->locations[loc].element == 0 && !strcmp(u->name, base))
            return element < u->array_size ? loc + element : -1;
    }
    /* Struct members were flattened with their array index. */
    for (int loc = 0; loc < p->location_count; ++loc) {
        const struct glm_uniform_info *u = &p->result.uniforms[p->locations[loc].uniform];
        if (p->locations[loc].element == 0 && !strcmp(u->name, uniform)) return loc;
    }
    return -1;
}

GLM_EXPORT void glGetActiveUniform(GLuint name, GLuint index, GLsizei size, GLsizei *length, GLint *array_size,
                                   GLenum *type, GLchar *out)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p) return glm_error(ctx, GL_INVALID_VALUE);
    const struct glm_uniform_info *u = active_uniform(p, index);
    if (!u) return glm_error(ctx, GL_INVALID_VALUE);
    char text[300];
    snprintf(text, sizeof text, u->array_size > 1 || u->is_array ? "%s[0]" : "%s", u->name);
    copy_string(text, size, length, out);
    if (array_size) *array_size = u->array_size;
    if (type) *type = u->type;
}

GLM_EXPORT void glGetActiveUniformName(GLuint name, GLuint index, GLsizei size, GLsizei *length, GLchar *out)
{
    glGetActiveUniform(name, index, size, length, NULL, NULL, out);
}

GLM_EXPORT void glGetUniformIndices(GLuint name, GLsizei count, const GLchar *const *names, GLuint *indices)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p || !p->linked) return glm_error(ctx, p ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
    int total = active_uniform_count(p);
    for (GLsizei i = 0; i < count; ++i) {
        indices[i] = GL_INVALID_INDEX;
        /* "a" and "a[0]" both name array a. */
        char base[300];
        snprintf(base, sizeof base, "%s", names[i]);
        size_t n = strlen(base);
        if (n > 3 && !strcmp(base + n - 3, "[0]")) base[n - 3] = 0;
        for (int k = 0; k < total; ++k)
            if (!strcmp(active_uniform(p, (GLuint)k)->name, base)) {
                indices[i] = (GLuint)k;
                break;
            }
    }
}

GLM_EXPORT void glGetActiveUniformsiv(GLuint name, GLsizei count, const GLuint *indices, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i)
        if (!active_uniform(p, indices[i])) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i) {
        const struct glm_uniform_info *u = active_uniform(p, indices[i]);
        bool member = u >= p->result.block_uniforms && u < p->result.block_uniforms + p->result.block_uniform_count;
        switch (pname) {
        case GL_UNIFORM_TYPE: params[i] = (GLint)u->type; break;
        case GL_UNIFORM_SIZE: params[i] = u->array_size; break;
        case GL_UNIFORM_NAME_LENGTH: params[i] = (GLint)strlen(u->name) + 1 + (u->array_size > 1 || u->is_array ? 3 : 0); break;
        case GL_UNIFORM_BLOCK_INDEX: params[i] = member ? u->block : -1; break;
        case GL_UNIFORM_OFFSET: params[i] = member ? u->offset : -1; break;
        case GL_UNIFORM_ARRAY_STRIDE: params[i] = member ? (u->array_size > 1 || u->is_array ? u->array_stride : 0) : -1; break;
        case GL_UNIFORM_MATRIX_STRIDE: params[i] = member ? u->matrix_stride : -1; break;
        case GL_UNIFORM_IS_ROW_MAJOR: params[i] = member && u->row_major; break;
        default: return glm_error(ctx, GL_INVALID_ENUM);
        }
    }
}

static bool is_sampler(uint32_t type)
{
    /* 0x8DC6-0x8DC8 inside the sampler range are GL_UNSIGNED_INT_VEC2-4. */
    return (type >= GL_SAMPLER_1D && type <= GL_SAMPLER_2D_RECT_SHADOW) ||
           (type >= GL_SAMPLER_1D_ARRAY && type <= GL_UNSIGNED_INT_SAMPLER_BUFFER && !(type >= 0x8DC6 && type <= 0x8DC8)) ||
           (type >= 0x900C && type <= 0x900F) /* cube map arrays */ || (type >= 0x9108 && type <= 0x910D) /* multisample */;
}

static int components(uint32_t type)
{
    switch (type) {
    case GL_DOUBLE: case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: case GL_BOOL: return 1;
    case GL_DOUBLE_VEC2: case GL_FLOAT_VEC2: case GL_INT_VEC2: case GL_UNSIGNED_INT_VEC2: case GL_BOOL_VEC2: return 2;
    case GL_DOUBLE_VEC3: case GL_FLOAT_VEC3: case GL_INT_VEC3: case GL_UNSIGNED_INT_VEC3: case GL_BOOL_VEC3: return 3;
    case GL_DOUBLE_VEC4: case GL_FLOAT_VEC4: case GL_INT_VEC4: case GL_UNSIGNED_INT_VEC4: case GL_BOOL_VEC4: return 4;
    default: return 1;
    }
}

static int matrix_shape(uint32_t type, int *rows)
{
    switch (type) {
    case GL_DOUBLE_MAT2: case GL_FLOAT_MAT2: *rows = 2; return 2;
    case GL_DOUBLE_MAT3: case GL_FLOAT_MAT3: *rows = 3; return 3;
    case GL_DOUBLE_MAT4: case GL_FLOAT_MAT4: *rows = 4; return 4;
    case GL_DOUBLE_MAT2x3: case GL_FLOAT_MAT2x3: *rows = 3; return 2;
    case GL_DOUBLE_MAT2x4: case GL_FLOAT_MAT2x4: *rows = 4; return 2;
    case GL_DOUBLE_MAT3x2: case GL_FLOAT_MAT3x2: *rows = 2; return 3;
    case GL_DOUBLE_MAT3x4: case GL_FLOAT_MAT3x4: *rows = 4; return 3;
    case GL_DOUBLE_MAT4x2: case GL_FLOAT_MAT4x2: *rows = 2; return 4;
    case GL_DOUBLE_MAT4x3: case GL_FLOAT_MAT4x3: *rows = 3; return 4;
    default: *rows = 0; return 0;
    }
}

static bool double_type(uint32_t type)
{
    return type == GL_DOUBLE || (type >= GL_DOUBLE_VEC2 && type <= GL_DOUBLE_VEC4) ||
           (type >= GL_DOUBLE_MAT2 && type <= GL_DOUBLE_MAT4x3);
}

bool glm_program_set_double(struct glm_context *ctx, struct glm_program *p, GLint location,
                            GLsizei count, int columns, int rows, GLboolean transpose, const GLdouble *values)
{
    if (!p || !p->linked || location < 0 || location >= p->location_count) return false;
    const struct glm_location loc = p->locations[location];
    const struct glm_uniform_info *u = &p->result.uniforms[loc.uniform];
    if (!double_type(u->type)) return false;
    int actual_rows, actual_columns = matrix_shape(u->type, &actual_rows);
    if (count < 0) { glm_error(ctx, GL_INVALID_VALUE); return true; }
    if (count > 1 && !u->is_array) { glm_error(ctx, GL_INVALID_OPERATION); return true; }
    if (rows ? actual_columns != columns || actual_rows != rows : actual_columns || components(u->type) != columns) {
        glm_error(ctx, GL_INVALID_OPERATION);
        return true;
    }
    if (!count || u->offset < 0) return true;
    int n = rows ? columns * rows : columns;
    int stride = u->array_stride ? u->array_stride : n * 8;
    int matrix_stride = u->matrix_stride ? u->matrix_stride : (u->row_major ? columns : rows) * 8;
    for (GLsizei e = 0; e < count && loc.element + e < u->array_size; ++e) {
        uint8_t *dst = p->globals + u->offset + (loc.element + e) * stride;
        if (!rows) memcpy(dst, values + (size_t)e * n, (size_t)n * 8);
        else for (int c = 0; c < columns; ++c)
            for (int r = 0; r < rows; ++r) {
                int source = transpose ? r * columns + c : c * rows + r;
                int offset = u->row_major ? r * matrix_stride + c * 8 : c * matrix_stride + r * 8;
                memcpy(dst + offset, values + (size_t)e * n + source, 8);
            }
    }
    p->globals_dirty = true;
    ++p->uniform_serial;
    return true;
}

bool glm_program_get_double(const struct glm_program *p, GLint location, GLdouble *values)
{
    if (!p || !p->linked || location < 0 || location >= p->location_count) return false;
    const struct glm_location loc = p->locations[location];
    const struct glm_uniform_info *u = &p->result.uniforms[loc.uniform];
    if (!double_type(u->type) || u->offset < 0) return false;
    int rows, columns = matrix_shape(u->type, &rows);
    int n = columns ? columns * rows : components(u->type);
    int stride = u->array_stride ? u->array_stride : n * 8;
    const uint8_t *src = p->globals + u->offset + loc.element * stride;
    if (!columns) memcpy(values, src, (size_t)n * 8);
    else for (int c = 0; c < columns; ++c)
        for (int r = 0; r < rows; ++r) {
            int matrix_stride = u->matrix_stride ? u->matrix_stride : (u->row_major ? columns : rows) * 8;
            int offset = u->row_major ? r * matrix_stride + c * 8 : c * matrix_stride + r * 8;
            memcpy(values + c * rows + r, src + offset, 8);
        }
    return true;
}

enum value_kind { VALUE_FLOAT, VALUE_INT, VALUE_UINT };

/* The program glUniform* changes: the current one, else the bound
   pipeline's active program. */
struct glm_program *glm_program_uniform_target(struct glm_context *ctx)
{
    if (ctx->current_program) return glm_current_program(ctx);
    return ctx->bound_pipeline ? glm_pipeline_active_program(ctx) : NULL;
}

static void set_uniform_in(struct glm_context *ctx, struct glm_program *p, GLint location, GLsizei count, int n,
                           enum value_kind kind, const void *values);
static void set_matrix_in(struct glm_context *ctx, struct glm_program *p, GLint location, GLsizei count, int columns,
                          int rows, GLboolean transpose, const GLfloat *values);

/* Writes `count` elements of `n` components starting at `location`. */
static void set_uniform(GLint location, GLsizei count, int n, enum value_kind kind, const void *values)
{
    GLM_CONTEXT(ctx);
    set_uniform_in(ctx, glm_program_uniform_target(ctx), location, count, n, kind, values);
}

static void set_uniform_in(struct glm_context *ctx, struct glm_program *p, GLint location, GLsizei count, int n,
                           enum value_kind kind, const void *values)
{
    if (location == -1) return;
    if (!p || !p->linked || location < 0 || location >= p->location_count || count < 0) return glm_error(ctx, GL_INVALID_OPERATION);
    struct glm_location loc = p->locations[location];
    const struct glm_uniform_info *u = &p->result.uniforms[loc.uniform];
    if (is_sampler(u->type)) {
        if (kind != VALUE_INT || n != 1) return glm_error(ctx, GL_INVALID_OPERATION);
        for (GLsizei i = 0; i < count && loc.element + i < u->array_size; ++i)
            if (u->sampler_slot + loc.element + i < 64) p->sampler_units[u->sampler_slot + loc.element + i] = ((const GLint *)values)[i];
        /* Sampler units are draw state, not uniform data. */
        __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
        ++p->uniform_serial;
        return;
    }
    if (double_type(u->type)) return glm_error(ctx, GL_INVALID_OPERATION);
    if (u->offset < 0) return;
    int per = components(u->type);
    if (per != n) return glm_error(ctx, GL_INVALID_OPERATION);
    /* glUniform*f loads float uniforms, *i int, *ui uint; bools take any. */
    bool is_int = u->type == GL_INT || (u->type >= GL_INT_VEC2 && u->type <= GL_INT_VEC4);
    bool is_uint = u->type == GL_UNSIGNED_INT || (u->type >= GL_UNSIGNED_INT_VEC2 && u->type <= GL_UNSIGNED_INT_VEC4);
    bool is_boolean = u->type == GL_BOOL || (u->type >= GL_BOOL_VEC2 && u->type <= GL_BOOL_VEC4);
    if (!is_boolean && (is_int ? kind != VALUE_INT : is_uint ? kind != VALUE_UINT : kind != VALUE_FLOAT))
        return glm_error(ctx, GL_INVALID_OPERATION);
    p->globals_dirty = true;
    ++p->uniform_serial;
    bool is_bool = u->type == GL_BOOL || u->type == GL_BOOL_VEC2 || u->type == GL_BOOL_VEC3 || u->type == GL_BOOL_VEC4;
    bool target_float = !is_bool && (u->type == GL_FLOAT || (u->type >= GL_FLOAT_VEC2 && u->type <= GL_FLOAT_VEC4));
    int stride = u->array_stride ? u->array_stride : n * 4;
    for (GLsizei e = 0; e < count && loc.element + e < u->array_size; ++e) {
        uint8_t *dst = p->globals + u->offset + (loc.element + e) * stride;
        for (int c = 0; c < n; ++c) {
            const uint8_t *src = (const uint8_t *)values + ((size_t)e * n + c) * 4;
            if (target_float && kind == VALUE_FLOAT) memcpy(dst + c * 4, src, 4);
            else if (is_bool) {
                uint32_t v;
                if (kind == VALUE_FLOAT) { float f; memcpy(&f, src, 4); v = f != 0; }
                else { int32_t i; memcpy(&i, src, 4); v = i != 0; }
                memcpy(dst + c * 4, &v, 4);
            } else memcpy(dst + c * 4, src, 4);
        }
    }
}

static void set_matrix(GLint location, GLsizei count, int columns, int rows, GLboolean transpose, const GLfloat *values)
{
    GLM_CONTEXT(ctx);
    set_matrix_in(ctx, glm_program_uniform_target(ctx), location, count, columns, rows, transpose, values);
}

static void set_matrix_in(struct glm_context *ctx, struct glm_program *p, GLint location, GLsizei count, int columns,
                          int rows, GLboolean transpose, const GLfloat *values)
{
    if (location == -1) return;
    if (!p || !p->linked || location < 0 || location >= p->location_count || count < 0) return glm_error(ctx, GL_INVALID_OPERATION);
    struct glm_location loc = p->locations[location];
    const struct glm_uniform_info *u = &p->result.uniforms[loc.uniform];
    if (double_type(u->type)) return glm_error(ctx, GL_INVALID_OPERATION);
    int urows, ucolumns = matrix_shape(u->type, &urows);
    if (ucolumns != columns || urows != rows || u->offset < 0) return glm_error(ctx, GL_INVALID_OPERATION);
    p->globals_dirty = true;
    ++p->uniform_serial;
    int matrix_stride = u->matrix_stride ? u->matrix_stride : 16;
    int stride = u->array_stride ? u->array_stride : matrix_stride * columns;
    for (GLsizei e = 0; e < count && loc.element + e < u->array_size; ++e) {
        uint8_t *dst = p->globals + u->offset + (loc.element + e) * stride;
        const GLfloat *m = values + (size_t)e * columns * rows;
        for (int c = 0; c < columns; ++c)
            for (int r = 0; r < rows; ++r) {
                float v = transpose ? m[r * columns + c] : m[c * rows + r];
                memcpy(dst + c * matrix_stride + r * 4, &v, 4);
            }
    }
}

#define UNIFORM_F(n, ...) do { GLfloat v[] = {__VA_ARGS__}; set_uniform(location, 1, n, VALUE_FLOAT, v); } while (0)
#define UNIFORM_I(n, ...) do { GLint v[] = {__VA_ARGS__}; set_uniform(location, 1, n, VALUE_INT, v); } while (0)
#define UNIFORM_U(n, ...) do { GLuint v[] = {__VA_ARGS__}; set_uniform(location, 1, n, VALUE_UINT, v); } while (0)

GLM_EXPORT void glUniform1f(GLint location, GLfloat x) { UNIFORM_F(1, x); }
GLM_EXPORT void glUniform2f(GLint location, GLfloat x, GLfloat y) { UNIFORM_F(2, x, y); }
GLM_EXPORT void glUniform3f(GLint location, GLfloat x, GLfloat y, GLfloat z) { UNIFORM_F(3, x, y, z); }
GLM_EXPORT void glUniform4f(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { UNIFORM_F(4, x, y, z, w); }
GLM_EXPORT void glUniform1i(GLint location, GLint x) { UNIFORM_I(1, x); }
GLM_EXPORT void glUniform2i(GLint location, GLint x, GLint y) { UNIFORM_I(2, x, y); }
GLM_EXPORT void glUniform3i(GLint location, GLint x, GLint y, GLint z) { UNIFORM_I(3, x, y, z); }
GLM_EXPORT void glUniform4i(GLint location, GLint x, GLint y, GLint z, GLint w) { UNIFORM_I(4, x, y, z, w); }
GLM_EXPORT void glUniform1ui(GLint location, GLuint x) { UNIFORM_U(1, x); }
GLM_EXPORT void glUniform2ui(GLint location, GLuint x, GLuint y) { UNIFORM_U(2, x, y); }
GLM_EXPORT void glUniform3ui(GLint location, GLuint x, GLuint y, GLuint z) { UNIFORM_U(3, x, y, z); }
GLM_EXPORT void glUniform4ui(GLint location, GLuint x, GLuint y, GLuint z, GLuint w) { UNIFORM_U(4, x, y, z, w); }
GLM_EXPORT void glUniform1fv(GLint location, GLsizei count, const GLfloat *v) { set_uniform(location, count, 1, VALUE_FLOAT, v); }
GLM_EXPORT void glUniform2fv(GLint location, GLsizei count, const GLfloat *v) { set_uniform(location, count, 2, VALUE_FLOAT, v); }
GLM_EXPORT void glUniform3fv(GLint location, GLsizei count, const GLfloat *v) { set_uniform(location, count, 3, VALUE_FLOAT, v); }
GLM_EXPORT void glUniform4fv(GLint location, GLsizei count, const GLfloat *v) { set_uniform(location, count, 4, VALUE_FLOAT, v); }
GLM_EXPORT void glUniform1iv(GLint location, GLsizei count, const GLint *v) { set_uniform(location, count, 1, VALUE_INT, v); }
GLM_EXPORT void glUniform2iv(GLint location, GLsizei count, const GLint *v) { set_uniform(location, count, 2, VALUE_INT, v); }
GLM_EXPORT void glUniform3iv(GLint location, GLsizei count, const GLint *v) { set_uniform(location, count, 3, VALUE_INT, v); }
GLM_EXPORT void glUniform4iv(GLint location, GLsizei count, const GLint *v) { set_uniform(location, count, 4, VALUE_INT, v); }
GLM_EXPORT void glUniform1uiv(GLint location, GLsizei count, const GLuint *v) { set_uniform(location, count, 1, VALUE_UINT, v); }
GLM_EXPORT void glUniform2uiv(GLint location, GLsizei count, const GLuint *v) { set_uniform(location, count, 2, VALUE_UINT, v); }
GLM_EXPORT void glUniform3uiv(GLint location, GLsizei count, const GLuint *v) { set_uniform(location, count, 3, VALUE_UINT, v); }
GLM_EXPORT void glUniform4uiv(GLint location, GLsizei count, const GLuint *v) { set_uniform(location, count, 4, VALUE_UINT, v); }
GLM_EXPORT void glUniformMatrix2fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { set_matrix(l, c, 2, 2, t, v); }
GLM_EXPORT void glUniformMatrix3fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { set_matrix(l, c, 3, 3, t, v); }
GLM_EXPORT void glUniformMatrix4fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { set_matrix(l, c, 4, 4, t, v); }
GLM_EXPORT void glUniformMatrix2x3fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { set_matrix(l, c, 2, 3, t, v); }
GLM_EXPORT void glUniformMatrix2x4fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { set_matrix(l, c, 2, 4, t, v); }
GLM_EXPORT void glUniformMatrix3x2fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { set_matrix(l, c, 3, 2, t, v); }
GLM_EXPORT void glUniformMatrix3x4fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { set_matrix(l, c, 3, 4, t, v); }
GLM_EXPORT void glUniformMatrix4x2fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { set_matrix(l, c, 4, 2, t, v); }
GLM_EXPORT void glUniformMatrix4x3fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { set_matrix(l, c, 4, 3, t, v); }

/* ---- glProgramUniform* (ARB_separate_shader_objects) --------------------- */

static void program_uniform(GLuint program, GLint location, GLsizei count, int n, enum value_kind kind, const void *v)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, program);
    if (!p) return glm_error(ctx, GL_INVALID_VALUE);
    set_uniform_in(ctx, p, location, count, n, kind, v);
}

static void program_matrix(GLuint program, GLint location, GLsizei count, int columns, int rows, GLboolean transpose,
                           const GLfloat *v)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, program);
    if (!p) return glm_error(ctx, GL_INVALID_VALUE);
    set_matrix_in(ctx, p, location, count, columns, rows, transpose, v);
}

#define PROGRAM_UNIFORM(suffix, T, kind, n, ...)                                                         \
    GLM_EXPORT void glProgramUniform##suffix(GLuint program, GLint location, __VA_ARGS__)
#define PU_F(n, ...) do { GLfloat v[] = {__VA_ARGS__}; program_uniform(program, location, 1, n, VALUE_FLOAT, v); } while (0)
#define PU_I(n, ...) do { GLint v[] = {__VA_ARGS__}; program_uniform(program, location, 1, n, VALUE_INT, v); } while (0)
#define PU_U(n, ...) do { GLuint v[] = {__VA_ARGS__}; program_uniform(program, location, 1, n, VALUE_UINT, v); } while (0)
GLM_EXPORT void glProgramUniform1f(GLuint program, GLint location, GLfloat x) { PU_F(1, x); }
GLM_EXPORT void glProgramUniform2f(GLuint program, GLint location, GLfloat x, GLfloat y) { PU_F(2, x, y); }
GLM_EXPORT void glProgramUniform3f(GLuint program, GLint location, GLfloat x, GLfloat y, GLfloat z) { PU_F(3, x, y, z); }
GLM_EXPORT void glProgramUniform4f(GLuint program, GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { PU_F(4, x, y, z, w); }
GLM_EXPORT void glProgramUniform1i(GLuint program, GLint location, GLint x) { PU_I(1, x); }
GLM_EXPORT void glProgramUniform2i(GLuint program, GLint location, GLint x, GLint y) { PU_I(2, x, y); }
GLM_EXPORT void glProgramUniform3i(GLuint program, GLint location, GLint x, GLint y, GLint z) { PU_I(3, x, y, z); }
GLM_EXPORT void glProgramUniform4i(GLuint program, GLint location, GLint x, GLint y, GLint z, GLint w) { PU_I(4, x, y, z, w); }
GLM_EXPORT void glProgramUniform1ui(GLuint program, GLint location, GLuint x) { PU_U(1, x); }
GLM_EXPORT void glProgramUniform2ui(GLuint program, GLint location, GLuint x, GLuint y) { PU_U(2, x, y); }
GLM_EXPORT void glProgramUniform3ui(GLuint program, GLint location, GLuint x, GLuint y, GLuint z) { PU_U(3, x, y, z); }
GLM_EXPORT void glProgramUniform4ui(GLuint program, GLint location, GLuint x, GLuint y, GLuint z, GLuint w) { PU_U(4, x, y, z, w); }
#define PU_V(suffix, T, n, kind)                                                                                     \
    GLM_EXPORT void glProgramUniform##suffix(GLuint program, GLint location, GLsizei count, const T *v)             \
    {                                                                                                               \
        program_uniform(program, location, count, n, kind, v);                                                      \
    }
PU_V(1fv, GLfloat, 1, VALUE_FLOAT) PU_V(2fv, GLfloat, 2, VALUE_FLOAT) PU_V(3fv, GLfloat, 3, VALUE_FLOAT)
PU_V(4fv, GLfloat, 4, VALUE_FLOAT) PU_V(1iv, GLint, 1, VALUE_INT) PU_V(2iv, GLint, 2, VALUE_INT)
PU_V(3iv, GLint, 3, VALUE_INT) PU_V(4iv, GLint, 4, VALUE_INT) PU_V(1uiv, GLuint, 1, VALUE_UINT)
PU_V(2uiv, GLuint, 2, VALUE_UINT) PU_V(3uiv, GLuint, 3, VALUE_UINT) PU_V(4uiv, GLuint, 4, VALUE_UINT)
#define PU_M(suffix, c, r)                                                                                           \
    GLM_EXPORT void glProgramUniformMatrix##suffix(GLuint program, GLint location, GLsizei count, GLboolean t,       \
                                                   const GLfloat *v)                                                \
    {                                                                                                               \
        program_matrix(program, location, count, c, r, t, v);                                                       \
    }
PU_M(2fv, 2, 2) PU_M(3fv, 3, 3) PU_M(4fv, 4, 4) PU_M(2x3fv, 2, 3) PU_M(2x4fv, 2, 4) PU_M(3x2fv, 3, 2)
PU_M(3x4fv, 3, 4) PU_M(4x2fv, 4, 2) PU_M(4x3fv, 4, 3)
#undef PROGRAM_UNIFORM

GLM_EXPORT GLuint glCreateShaderProgramv(GLenum type, GLsizei count, const GLchar *const *strings)
{
    GLM_CONTEXT(ctx, 0);
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, count, strings, NULL);
    glCompileShader(shader);
    GLuint program = glCreateProgram();
    struct glm_program *p = glm_program_get(ctx, program);
    struct glm_shader *s = shader_get(ctx, shader);
    p->separable = true;
    if (s && s->compiled) {
        glAttachShader(program, shader);
        glLinkProgram(program);
        glDetachShader(program, shader);
    } else if (s) {
        free(p->log);
        p->log = duplicate(s->log);
    }
    glDeleteShader(shader);
    return program;
}

GLM_EXPORT void glProgramParameteri(GLuint program, GLenum pname, GLint value)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, program);
    if (!p) return glm_error(ctx, GL_INVALID_VALUE);
    if (pname == GL_PROGRAM_SEPARABLE) p->separable = value != 0;
    else if (pname >= 0x8DDA && pname <= 0x8DDC && ctx->profile != GLM_PROFILE_CORE) p->ext_geometry[pname - 0x8DDA] = value;
    else if (pname != GL_PROGRAM_BINARY_RETRIEVABLE_HINT) glm_error(ctx, GL_INVALID_ENUM);
}

GLM_EXPORT void glGetUniformfv(GLuint name, GLint location, GLfloat *params)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p || !p->linked || location < 0 || location >= p->location_count) return glm_error(ctx, GL_INVALID_OPERATION);
    const struct glm_uniform_info *u = &p->result.uniforms[p->locations[location].uniform];
    if (is_sampler(u->type)) {
        params[0] = (GLfloat)p->sampler_units[u->sampler_slot + p->locations[location].element];
        return;
    }
    GLdouble doubles[16];
    if (glm_program_get_double(p, location, doubles)) {
        int rows, columns = matrix_shape(u->type, &rows);
        int n = columns ? columns * rows : components(u->type);
        for (int c = 0; c < n; ++c) params[c] = (GLfloat)doubles[c];
        return;
    }
    int rows, columns = matrix_shape(u->type, &rows);
    const uint8_t *src = p->globals + u->offset + p->locations[location].element * (u->array_stride ? u->array_stride : 16);
    bool integer = !(u->type == GL_FLOAT || (u->type >= GL_FLOAT_VEC2 && u->type <= GL_FLOAT_VEC4) || columns);
    if (columns) {
        for (int c = 0; c < columns; ++c) memcpy(params + c * rows, src + c * (u->matrix_stride ? u->matrix_stride : 16), (size_t)rows * 4);
        return;
    }
    for (int c = 0; c < components(u->type); ++c) {
        if (integer) { int32_t i; memcpy(&i, src + c * 4, 4); params[c] = (GLfloat)i; }
        else memcpy(&params[c], src + c * 4, 4);
    }
}

GLM_EXPORT void glGetUniformiv(GLuint name, GLint location, GLint *params)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    GLdouble doubles[16];
    if (glm_program_get_double(p, location, doubles)) {
        const struct glm_uniform_info *u = &p->result.uniforms[p->locations[location].uniform];
        int rows, columns = matrix_shape(u->type, &rows);
        int n = columns ? columns * rows : components(u->type);
        for (int c = 0; c < n; ++c) params[c] = (GLint)doubles[c];
        return;
    }
    GLfloat f[16];
    glGetUniformfv(name, location, f);
    if (!p || location < 0 || location >= p->location_count) return;
    const struct glm_uniform_info *u = &p->result.uniforms[p->locations[location].uniform];
    for (int c = 0; c < components(u->type); ++c) params[c] = (GLint)f[c];
}

/* ---- uniform blocks ------------------------------------------------------ */

GLM_EXPORT GLuint glGetUniformBlockIndex(GLuint name, const GLchar *block)
{
    GLM_CONTEXT(ctx, GL_INVALID_INDEX);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p || !p->linked) return GL_INVALID_INDEX;
    for (int i = 0; i < p->result.block_count; ++i)
        if (!strcmp(p->result.blocks[i].name, block)) return (GLuint)i;
    return GL_INVALID_INDEX;
}

GLM_EXPORT void glUniformBlockBinding(GLuint name, GLuint index, GLuint binding)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p || index >= (GLuint)p->result.block_count || index >= GLM_SLOT_UBO_COUNT) return glm_error(ctx, GL_INVALID_VALUE);
    p->block_bindings[index] = binding;
}

GLM_EXPORT void glGetActiveUniformBlockiv(GLuint name, GLuint index, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p || index >= (GLuint)p->result.block_count) return glm_error(ctx, GL_INVALID_VALUE);
    switch (pname) {
    case GL_UNIFORM_BLOCK_BINDING: *params = (GLint)p->block_bindings[index]; break;
    case GL_UNIFORM_BLOCK_DATA_SIZE: *params = p->result.blocks[index].size; break;
    case GL_UNIFORM_BLOCK_NAME_LENGTH: *params = (GLint)strlen(p->result.blocks[index].name) + 1; break;
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS:
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: {
        int n = 0, first = active_uniform_count(p) - p->result.block_uniform_count;
        /* Elements of a block array ("Block[2]") share element 0's members. */
        int members_of = (int)index;
        const char *name = p->result.blocks[index].name, *bracket = strrchr(name, '[');
        if (bracket && strcmp(bracket, "[0]")) {
            for (int b = 0; b < p->result.block_count; ++b)
                if (!strncmp(p->result.blocks[b].name, name, (size_t)(bracket - name)) &&
                    !strcmp(p->result.blocks[b].name + (bracket - name), "[0]"))
                    members_of = b;
        }
        for (int i = 0; i < p->result.block_uniform_count; ++i) {
            if (p->result.block_uniforms[i].block != members_of) continue;
            if (pname == GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES) params[n] = first + i;
            ++n;
        }
        if (pname == GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS) *params = n;
        break;
    }
    case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER:
        *params = (p->result.blocks[index].stages >> GLM_STAGE_VERTEX) & 1;
        break;
    case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER:
        *params = (p->result.blocks[index].stages >> GLM_STAGE_FRAGMENT) & 1;
        break;
    case GL_UNIFORM_BLOCK_REFERENCED_BY_GEOMETRY_SHADER:
        *params = (p->result.blocks[index].stages >> GLM_STAGE_GEOMETRY) & 1;
        break;
    case GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_CONTROL_SHADER:
        *params = (p->result.blocks[index].stages >> GLM_STAGE_TESS_CONTROL) & 1;
        break;
    case GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_EVALUATION_SHADER:
        *params = (p->result.blocks[index].stages >> GLM_STAGE_TESS_EVALUATION) & 1;
        break;
    default: glm_error(ctx, GL_INVALID_ENUM); break;
    }
}

GLM_EXPORT void glGetActiveUniformBlockName(GLuint name, GLuint index, GLsizei size, GLsizei *length, GLchar *out)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p || index >= (GLuint)p->result.block_count) return glm_error(ctx, GL_INVALID_VALUE);
    copy_string(p->result.blocks[index].name, size, length, out);
}

/* ---- program-related glGet values ---------------------------------------- */

/* Values Apple's GL reports on Apple silicon; the core-only ones are
   GL_INVALID_ENUM in the legacy profile, as there. */
int glm_program_query(struct glm_context *ctx, GLenum pname, double *v)
{
    bool core = ctx->profile == GLM_PROFILE_CORE;
    switch (pname) {
    case GL_MAX_VERTEX_UNIFORM_VECTORS: case GL_MAX_FRAGMENT_UNIFORM_VECTORS: v[0] = 1024; return 1;
    /* GL_MAX_VARYING_COMPONENTS aliases GL_MAX_VARYING_FLOATS. Core
       limits follow glslang's 64-component VS output capacity, less Position. */
    case GL_MAX_VARYING_FLOATS: v[0] = core ? 60 : 124; return 1;
    case GL_MAX_VARYING_VECTORS: v[0] = core ? 15 : 31; return 1;
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: v[0] = 16; return 1;
    case GL_MAX_TEXTURE_LOD_BIAS: v[0] = 16; return 1;
    }
    if (!core) return 0;
    switch (pname) {
    case GL_MAX_UNIFORM_BUFFER_BINDINGS: v[0] = 80; return 1;
    case GL_MAX_VERTEX_UNIFORM_BLOCKS: case GL_MAX_FRAGMENT_UNIFORM_BLOCKS: case GL_MAX_GEOMETRY_UNIFORM_BLOCKS: v[0] = 16; return 1;
    case GL_MAX_COMBINED_UNIFORM_BLOCKS: v[0] = 48; return 1;
    case GL_MAX_UNIFORM_BLOCK_SIZE: v[0] = 65536; return 1;
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: v[0] = 256; return 1;
    case GL_MAX_VERTEX_OUTPUT_COMPONENTS: v[0] = 64; return 1;
    case GL_MAX_FRAGMENT_INPUT_COMPONENTS: v[0] = 128; return 1;
    case GL_MAX_GEOMETRY_UNIFORM_COMPONENTS: v[0] = 1024; return 1;
    case GL_SAMPLER_BINDING: v[0] = ctx->state.units[ctx->state.active_texture - GL_TEXTURE0].sampler; return 1;
    default: return 0;
    }
}

/* ---- ARB_shader_objects (GLhandleARB is a pointer on Apple) --------------- */

static GLuint handle_name(GLhandleARB h) { return (GLuint)(uintptr_t)h; }
static GLhandleARB name_handle(GLuint n) { return (GLhandleARB)(uintptr_t)n; }

GLM_EXPORT GLhandleARB glCreateShaderObjectARB(GLenum type) { return name_handle(glCreateShader(type)); }
GLM_EXPORT GLhandleARB glCreateProgramObjectARB(void) { return name_handle(glCreateProgram()); }
GLM_EXPORT void glShaderSourceARB(GLhandleARB s, GLsizei n, const GLcharARB *const *strings, const GLint *lengths)
{ glShaderSource(handle_name(s), n, (const GLchar *const *)strings, lengths); }
GLM_EXPORT void glCompileShaderARB(GLhandleARB s) { glCompileShader(handle_name(s)); }
GLM_EXPORT void glAttachObjectARB(GLhandleARB p, GLhandleARB s) { glAttachShader(handle_name(p), handle_name(s)); }
GLM_EXPORT void glDetachObjectARB(GLhandleARB p, GLhandleARB s) { glDetachShader(handle_name(p), handle_name(s)); }
GLM_EXPORT void glLinkProgramARB(GLhandleARB p) { glLinkProgram(handle_name(p)); }
GLM_EXPORT void glUseProgramObjectARB(GLhandleARB p) { glUseProgram(handle_name(p)); }
GLM_EXPORT void glValidateProgramARB(GLhandleARB p) { glValidateProgram(handle_name(p)); }
GLM_EXPORT void glBindAttribLocationARB(GLhandleARB p, GLuint i, const GLcharARB *n) { glBindAttribLocation(handle_name(p), i, n); }
GLM_EXPORT GLint glGetAttribLocationARB(GLhandleARB p, const GLcharARB *n) { return glGetAttribLocation(handle_name(p), n); }
GLM_EXPORT GLint glGetUniformLocationARB(GLhandleARB p, const GLcharARB *n) { return glGetUniformLocation(handle_name(p), n); }
GLM_EXPORT void glGetActiveUniformARB(GLhandleARB p, GLuint i, GLsizei s, GLsizei *l, GLint *a, GLenum *t, GLcharARB *n)
{ glGetActiveUniform(handle_name(p), i, s, l, a, t, n); }
GLM_EXPORT void glGetActiveAttribARB(GLhandleARB p, GLuint i, GLsizei s, GLsizei *l, GLint *a, GLenum *t, GLcharARB *n)
{ glGetActiveAttrib(handle_name(p), i, s, l, a, t, n); }
GLM_EXPORT void glGetUniformfvARB(GLhandleARB p, GLint l, GLfloat *v) { glGetUniformfv(handle_name(p), l, v); }
GLM_EXPORT void glGetUniformivARB(GLhandleARB p, GLint l, GLint *v) { glGetUniformiv(handle_name(p), l, v); }
GLM_EXPORT GLhandleARB glGetHandleARB(GLenum pname)
{
    GLM_CONTEXT(ctx, NULL);
    return pname == GL_PROGRAM_OBJECT_ARB ? name_handle(ctx->current_program) : NULL;
}
GLM_EXPORT void glDeleteObjectARB(GLhandleARB h)
{
    GLM_CONTEXT(ctx);
    GLuint name = handle_name(h);
    if (glm_program_get(ctx, name)) glDeleteProgram(name);
    else glDeleteShader(name);
}
GLM_EXPORT void glGetObjectParameterivARB(GLhandleARB h, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    GLuint name = handle_name(h);
    bool program = glm_program_get(ctx, name) != NULL;
    switch (pname) {
    case GL_OBJECT_TYPE_ARB: *params = program ? GL_PROGRAM_OBJECT_ARB : GL_SHADER_OBJECT_ARB; return;
    case GL_OBJECT_SUBTYPE_ARB: if (!program) glGetShaderiv(name, GL_SHADER_TYPE, params); return;
    case GL_OBJECT_COMPILE_STATUS_ARB: glGetShaderiv(name, GL_COMPILE_STATUS, params); return;
    case GL_OBJECT_LINK_STATUS_ARB: glGetProgramiv(name, GL_LINK_STATUS, params); return;
    case GL_OBJECT_VALIDATE_STATUS_ARB: glGetProgramiv(name, GL_VALIDATE_STATUS, params); return;
    case GL_OBJECT_INFO_LOG_LENGTH_ARB:
        if (program) glGetProgramiv(name, GL_INFO_LOG_LENGTH, params);
        else glGetShaderiv(name, GL_INFO_LOG_LENGTH, params);
        return;
    case GL_OBJECT_ATTACHED_OBJECTS_ARB: glGetProgramiv(name, GL_ATTACHED_SHADERS, params); return;
    case GL_OBJECT_ACTIVE_UNIFORMS_ARB: glGetProgramiv(name, GL_ACTIVE_UNIFORMS, params); return;
    case GL_OBJECT_ACTIVE_UNIFORM_MAX_LENGTH_ARB: glGetProgramiv(name, GL_ACTIVE_UNIFORM_MAX_LENGTH, params); return;
    case GL_OBJECT_ACTIVE_ATTRIBUTES_ARB: glGetProgramiv(name, GL_ACTIVE_ATTRIBUTES, params); return;
    case GL_OBJECT_ACTIVE_ATTRIBUTE_MAX_LENGTH_ARB: glGetProgramiv(name, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, params); return;
    case GL_OBJECT_SHADER_SOURCE_LENGTH_ARB: glGetShaderiv(name, GL_SHADER_SOURCE_LENGTH, params); return;
    case GL_OBJECT_DELETE_STATUS_ARB:
        if (program) glGetProgramiv(name, GL_DELETE_STATUS, params);
        else glGetShaderiv(name, GL_DELETE_STATUS, params);
        return;
    }
    glm_error(ctx, GL_INVALID_ENUM);
}
GLM_EXPORT void glGetObjectParameterfvARB(GLhandleARB h, GLenum pname, GLfloat *params)
{
    GLint i = 0;
    glGetObjectParameterivARB(h, pname, &i);
    *params = (GLfloat)i;
}
GLM_EXPORT void glGetInfoLogARB(GLhandleARB h, GLsizei size, GLsizei *length, GLcharARB *log)
{
    GLM_CONTEXT(ctx);
    GLuint name = handle_name(h);
    if (glm_program_get(ctx, name)) glGetProgramInfoLog(name, size, length, log);
    else glGetShaderInfoLog(name, size, length, log);
}
GLM_EXPORT void glGetAttachedObjectsARB(GLhandleARB p, GLsizei max, GLsizei *count, GLhandleARB *objects_out)
{
    GLuint names[8];
    GLsizei n = 0;
    glGetAttachedShaders(handle_name(p), max < 8 ? max : 8, &n, names);
    for (GLsizei i = 0; i < n; ++i) objects_out[i] = name_handle(names[i]);
    if (count) *count = n;
}
GLM_EXPORT void glGetShaderSourceARB(GLhandleARB s, GLsizei size, GLsizei *length, GLcharARB *source)
{ glGetShaderSource(handle_name(s), size, length, source); }

#define ARB_ALIAS_F(n, params, args) GLM_EXPORT void glUniform##n##ARB params { glUniform##n args; }
ARB_ALIAS_F(1f, (GLint l, GLfloat x), (l, x))
ARB_ALIAS_F(2f, (GLint l, GLfloat x, GLfloat y), (l, x, y))
ARB_ALIAS_F(3f, (GLint l, GLfloat x, GLfloat y, GLfloat z), (l, x, y, z))
ARB_ALIAS_F(4f, (GLint l, GLfloat x, GLfloat y, GLfloat z, GLfloat w), (l, x, y, z, w))
ARB_ALIAS_F(1i, (GLint l, GLint x), (l, x))
ARB_ALIAS_F(2i, (GLint l, GLint x, GLint y), (l, x, y))
ARB_ALIAS_F(3i, (GLint l, GLint x, GLint y, GLint z), (l, x, y, z))
ARB_ALIAS_F(4i, (GLint l, GLint x, GLint y, GLint z, GLint w), (l, x, y, z, w))
ARB_ALIAS_F(1fv, (GLint l, GLsizei c, const GLfloat *v), (l, c, v))
ARB_ALIAS_F(2fv, (GLint l, GLsizei c, const GLfloat *v), (l, c, v))
ARB_ALIAS_F(3fv, (GLint l, GLsizei c, const GLfloat *v), (l, c, v))
ARB_ALIAS_F(4fv, (GLint l, GLsizei c, const GLfloat *v), (l, c, v))
ARB_ALIAS_F(1iv, (GLint l, GLsizei c, const GLint *v), (l, c, v))
ARB_ALIAS_F(2iv, (GLint l, GLsizei c, const GLint *v), (l, c, v))
ARB_ALIAS_F(3iv, (GLint l, GLsizei c, const GLint *v), (l, c, v))
ARB_ALIAS_F(4iv, (GLint l, GLsizei c, const GLint *v), (l, c, v))
ARB_ALIAS_F(Matrix2fv, (GLint l, GLsizei c, GLboolean t, const GLfloat *v), (l, c, t, v))
ARB_ALIAS_F(Matrix3fv, (GLint l, GLsizei c, GLboolean t, const GLfloat *v), (l, c, t, v))
ARB_ALIAS_F(Matrix4fv, (GLint l, GLsizei c, GLboolean t, const GLfloat *v), (l, c, t, v))

/* ---- ARB_shader_subroutine (lowered by the shader compiler) -------------- */

static bool stage_type(GLenum type)
{
    return type == GL_VERTEX_SHADER || type == GL_FRAGMENT_SHADER || type == GL_GEOMETRY_SHADER ||
           type == GL_TESS_CONTROL_SHADER || type == GL_TESS_EVALUATION_SHADER;
}

static struct glm_program *subroutine_program(struct glm_context *ctx, GLuint name, GLenum type)
{
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p) {
        glm_error(ctx, GL_INVALID_VALUE);
        return NULL;
    }
    if (!stage_type(type)) {
        glm_error(ctx, GL_INVALID_ENUM);
        return NULL;
    }
    return p;
}

GLM_EXPORT GLuint glGetSubroutineIndex(GLuint name, GLenum type, const GLchar *function)
{
    GLM_CONTEXT(ctx, GL_INVALID_INDEX);
    struct glm_program *p = subroutine_program(ctx, name, type);
    if (!p || !p->linked) return GL_INVALID_INDEX;
    for (int i = 0; i < p->result.subroutine_count; ++i) {
        const struct glm_subroutine *f = &p->result.subroutines[i];
        if (f->stage == (int)stage_of(type) && !strcmp(f->name, function)) return (GLuint)f->index;
    }
    return GL_INVALID_INDEX;
}

GLM_EXPORT GLint glGetSubroutineUniformLocation(GLuint name, GLenum type, const GLchar *uniform)
{
    GLM_CONTEXT(ctx, -1);
    struct glm_program *p = subroutine_program(ctx, name, type);
    if (!p || !p->linked) return -1;
    char base[256];
    snprintf(base, sizeof base, "%s", uniform);
    int element = 0;
    char *open = strchr(base, '[');
    if (open) {
        element = atoi(open + 1);
        *open = 0;
    }
    for (int i = 0; i < p->result.subroutine_uniform_count; ++i) {
        const struct glm_subroutine_uniform *u = &p->result.subroutine_uniforms[i];
        if (u->stage == (int)stage_of(type) && !strcmp(u->name, base))
            return element < u->array_size ? u->location + element : -1;
    }
    return -1;
}

/* Locations of `stage`: one past the highest. */
static int subroutine_locations(const struct glm_program *p, int stage)
{
    int n = 0;
    for (int i = 0; i < p->result.subroutine_uniform_count; ++i) {
        const struct glm_subroutine_uniform *u = &p->result.subroutine_uniforms[i];
        if (u->stage == stage && u->location + u->array_size > n) n = u->location + u->array_size;
    }
    return n;
}

/* The program providing `stage` for draws. */
static struct glm_program *stage_program(struct glm_context *ctx, int stage)
{
    if (ctx->current_program) return glm_current_program(ctx);
    return ctx->bound_pipeline ? glm_pipeline_stage_program(ctx, stage) : NULL;
}

GLM_EXPORT void glUniformSubroutinesuiv(GLenum type, GLsizei count, const GLuint *indices)
{
    GLM_CONTEXT(ctx);
    if (!stage_type(type)) return glm_error(ctx, GL_INVALID_ENUM);
    int stage = (int)stage_of(type);
    struct glm_program *p = stage_program(ctx, stage);
    if (!p) return glm_error(ctx, GL_INVALID_OPERATION);
    if (count != subroutine_locations(p, stage)) return glm_error(ctx, GL_INVALID_VALUE);
    for (int i = 0; i < p->result.subroutine_uniform_count; ++i) {
        const struct glm_subroutine_uniform *u = &p->result.subroutine_uniforms[i];
        if (u->stage != stage) continue;
        char name[300];
        snprintf(name, sizeof name, "glm_sub_%s", u->name);
        const struct glm_uniform_info *info = NULL;
        for (int k = 0; k < p->result.uniform_count; ++k)
            if (!strcmp(p->result.uniforms[k].name, name)) info = &p->result.uniforms[k];
        for (int e = 0; e < u->array_size; ++e) {
            GLuint index = indices[u->location + e];
            bool compatible = false;
            for (int c = 0; c < u->compatible_count; ++c) compatible |= (GLuint)u->compatible[c] == index;
            if (!compatible) return glm_error(ctx, GL_INVALID_VALUE);
            if (u->location + e < 64) p->subroutine_values[stage][u->location + e] = index;
            if (info && info->offset >= 0)
                memcpy(p->globals + info->offset + e * (info->array_stride ? info->array_stride : 4), &index, 4);
        }
    }
    p->globals_dirty = true;
    ++p->uniform_serial;
}

GLM_EXPORT void glGetUniformSubroutineuiv(GLenum type, GLint location, GLuint *params)
{
    GLM_CONTEXT(ctx);
    if (!stage_type(type)) return glm_error(ctx, GL_INVALID_ENUM);
    int stage = (int)stage_of(type);
    struct glm_program *p = stage_program(ctx, stage);
    if (!p) return glm_error(ctx, GL_INVALID_OPERATION);
    if (location < 0 || location >= subroutine_locations(p, stage) || location >= 64) return glm_error(ctx, GL_INVALID_VALUE);
    *params = p->subroutine_values[stage][location];
}

GLM_EXPORT void glGetProgramStageiv(GLuint name, GLenum type, GLenum pname, GLint *values)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = subroutine_program(ctx, name, type);
    if (!p) return;
    int stage = (int)stage_of(type), count = 0, longest = 0;
    switch (pname) {
    case GL_ACTIVE_SUBROUTINES: case GL_ACTIVE_SUBROUTINE_MAX_LENGTH:
        for (int i = 0; i < p->result.subroutine_count; ++i) {
            if (p->result.subroutines[i].stage != stage) continue;
            ++count;
            int n = (int)strlen(p->result.subroutines[i].name) + 1;
            if (n > longest) longest = n;
        }
        *values = pname == GL_ACTIVE_SUBROUTINES ? count : longest;
        return;
    case GL_ACTIVE_SUBROUTINE_UNIFORMS: case GL_ACTIVE_SUBROUTINE_UNIFORM_MAX_LENGTH:
        for (int i = 0; i < p->result.subroutine_uniform_count; ++i) {
            if (p->result.subroutine_uniforms[i].stage != stage) continue;
            ++count;
            int n = (int)strlen(p->result.subroutine_uniforms[i].name) + 1;
            if (n > longest) longest = n;
        }
        *values = pname == GL_ACTIVE_SUBROUTINE_UNIFORMS ? count : longest;
        return;
    case GL_ACTIVE_SUBROUTINE_UNIFORM_LOCATIONS: *values = subroutine_locations(p, stage); return;
    }
    glm_error(ctx, GL_INVALID_ENUM);
}

static const struct glm_subroutine_uniform *subroutine_uniform_at(const struct glm_program *p, int stage, GLuint index)
{
    GLuint seen = 0;
    for (int i = 0; i < p->result.subroutine_uniform_count; ++i)
        if (p->result.subroutine_uniforms[i].stage == stage && seen++ == index) return &p->result.subroutine_uniforms[i];
    return NULL;
}

GLM_EXPORT void glGetActiveSubroutineName(GLuint name, GLenum type, GLuint index, GLsizei size, GLsizei *length, GLchar *out)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = subroutine_program(ctx, name, type);
    if (!p) return;
    for (int i = 0; i < p->result.subroutine_count; ++i) {
        const struct glm_subroutine *f = &p->result.subroutines[i];
        if (f->stage == (int)stage_of(type) && (GLuint)f->index == index) return copy_string(f->name, size, length, out);
    }
    glm_error(ctx, GL_INVALID_VALUE);
}

GLM_EXPORT void glGetActiveSubroutineUniformName(GLuint name, GLenum type, GLuint index, GLsizei size, GLsizei *length,
                                                 GLchar *out)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = subroutine_program(ctx, name, type);
    if (!p) return;
    const struct glm_subroutine_uniform *u = subroutine_uniform_at(p, (int)stage_of(type), index);
    if (!u) return glm_error(ctx, GL_INVALID_VALUE);
    copy_string(u->name, size, length, out);
}

GLM_EXPORT void glGetActiveSubroutineUniformiv(GLuint name, GLenum type, GLuint index, GLenum pname, GLint *values)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = subroutine_program(ctx, name, type);
    if (!p) return;
    const struct glm_subroutine_uniform *u = subroutine_uniform_at(p, (int)stage_of(type), index);
    if (!u) return glm_error(ctx, GL_INVALID_VALUE);
    switch (pname) {
    case GL_NUM_COMPATIBLE_SUBROUTINES: *values = u->compatible_count; break;
    case GL_COMPATIBLE_SUBROUTINES: for (int c = 0; c < u->compatible_count; ++c) values[c] = u->compatible[c]; break;
    case GL_UNIFORM_SIZE: *values = u->array_size; break;
    case GL_UNIFORM_NAME_LENGTH: *values = (GLint)strlen(u->name) + 1; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}

/* ---- shadow programs (marshal_custom.c) ------------------------------------ */

/* A program linked on the application's thread from `sources` and the
   given bindings: the front end runs here (its result also goes to the
   compile cache, where the stream's own link finds it) and the Metal
   functions start compiling in the background. Answers queries like the
   real program will. */
struct glm_program *glm_program_shadow_link(const struct glm_compile_request *request)
{
    struct glm_program *p = calloc(1, sizeof *p);
    p->feedback_mode = GL_INTERLEAVED_ATTRIBS;
    p->ext_geometry[1] = GL_TRIANGLES;
    p->ext_geometry[2] = GL_TRIANGLE_STRIP;
    uint64_t started = glm_now_ns();
    glm_program_compile(request, &p->result);
    glm_note_stall("program compile (application thread)", started);
    p->linked = p->result.ok;
    p->log = duplicate(p->result.log);
    if (p->linked) {
        glm_backend_prewarm_program(&p->result);
        build_locations(p);
    }
    return p;
}

void glm_program_shadow_free(struct glm_program *p)
{
    if (!p) return;
    glm_compile_result_free(&p->result);
    free(p->locations);
    free(p->log);
    free(p);
}
