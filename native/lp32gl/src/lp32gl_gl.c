/* The GL entry points.  Almost everything is Mesa's dispatch stub, returned
 * as is.  LP32GL interposes on:
 *
 *   - string, version and limit queries, so each context reports Apple's
 *     profile (2.1 legacy or 4.1 core) and Apple's extension list, filtered
 *     to what Mesa or this file really implements;
 *   - Apple-only enums that Mesa rejects (client storage, texture range and
 *     transform hints, specular vector);
 *   - Apple and ATI extensions Mesa does not have: APPLE_fence,
 *     APPLE_vertex_array_object, APPLE_flush_buffer_range,
 *     APPLE_element_array, APPLE_flush_render, APPLE_texture_range,
 *     APPLE_vertex_array_range, APPLE_object_purgeable, EXT_debug_marker,
 *     ATI_separate_stencil;
 *   - every other entry point OpenGL.framework exports, as a stub that
 *     raises GL_INVALID_OPERATION, so imports always resolve. */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lp32gl_internal.h"

#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#endif
#ifndef GL_MAX_VARYING_COMPONENTS
#define GL_MAX_VARYING_COMPONENTS 0x8B4B
#endif
#ifndef GL_MAX_UNIFORM_BLOCK_SIZE
#define GL_MAX_UNIFORM_BLOCK_SIZE 0x8A30
#endif
#ifndef GL_MAX_ARRAY_TEXTURE_LAYERS
#define GL_MAX_ARRAY_TEXTURE_LAYERS 0x88FF
#endif
#ifndef GL_MAX_COLOR_ATTACHMENTS
#define GL_MAX_COLOR_ATTACHMENTS 0x8CDF
#endif
#ifndef GL_MAX_RENDERBUFFER_SIZE
#define GL_MAX_RENDERBUFFER_SIZE 0x84E8
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x0010
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_ALREADY_SIGNALED 0x911A
#define GL_CONDITION_SATISFIED 0x911C
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull
#endif
#define GL_BUFFER_SERIALIZED_MODIFY_APPLE_ 0x8A12
#define GL_BUFFER_FLUSHING_UNMAP_APPLE_ 0x8A13
#define GL_UNPACK_CLIENT_STORAGE_APPLE_ 0x85B2
#define GL_TEXTURE_STORAGE_HINT_APPLE_ 0x85BC
#define GL_TEXTURE_RANGE_LENGTH_APPLE_ 0x85B7
#define GL_TEXTURE_RANGE_POINTER_APPLE_ 0x85B8
#define GL_TRANSFORM_HINT_APPLE_ 0x85B1
#define GL_LIGHT_MODEL_SPECULAR_VECTOR_APPLE_ 0x85B0
#define GL_PACK_ROW_BYTES_APPLE_ 0x8A15
#define GL_UNPACK_ROW_BYTES_APPLE_ 0x8A16
#define GL_RELEASED_APPLE_ 0x8A19
#define GL_VOLATILE_APPLE_ 0x8A1A
#define GL_RETAINED_APPLE_ 0x8A1B
#define GL_UNDEFINED_APPLE_ 0x8A1C
#define GL_PURGEABLE_APPLE_ 0x8A1D
#define GL_ELEMENT_ARRAY_APPLE_ 0x8A0C
#define GL_ELEMENT_ARRAY_TYPE_APPLE_ 0x8A0D
#define GL_ELEMENT_ARRAY_POINTER_APPLE_ 0x8A0E
#define GL_FENCE_APPLE_ 0x8A0B
#define GL_DRAW_PIXELS_APPLE_ 0x8A0A

typedef struct __GLsync *GLsync_;

#ifndef MESA_VERSION_STRING
#define MESA_VERSION_STRING "unknown"
#endif

#include "lp32gl_apple_profile.h"

/* ---- Mesa entry points LP32GL calls itself ---------------------------- */

static struct {
    const GLubyte *(*GetString)(GLenum);
    const GLubyte *(*GetStringi)(GLenum, GLuint);
    void (*GetIntegerv)(GLenum, GLint *);
    void (*GetFloatv)(GLenum, GLfloat *);
    void (*GetBooleanv)(GLenum, GLboolean *);
    void (*GetDoublev)(GLenum, GLdouble *);
    void (*PixelStorei)(GLenum, GLint);
    void (*PixelStoref)(GLenum, GLfloat);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*TexParameterf)(GLenum, GLenum, GLfloat);
    void (*TexParameteriv)(GLenum, GLenum, const GLint *);
    void (*TexParameterfv)(GLenum, GLenum, const GLfloat *);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    void (*CompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
    void (*TexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum,
                          const void *);
    void (*CompressedTexImage3D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, GLsizei,
                                 const void *);
    void (*CompressedTexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum,
                                    GLsizei, const void *);
    void (*BufferData)(GLenum, intptr_t, const void *, GLenum);
    void (*BufferSubData)(GLenum, intptr_t, intptr_t, const void *);
    void (*CompressedTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
    void (*Hint)(GLenum, GLenum);
    void (*LightModeli)(GLenum, GLint);
    void (*LightModelf)(GLenum, GLfloat);
    void (*LightModeliv)(GLenum, const GLint *);
    void (*LightModelfv)(GLenum, const GLfloat *);
    void (*Flush)(void);
    void (*Finish)(void);
    GLsync_ (*FenceSync)(GLenum, GLbitfield);
    GLenum (*ClientWaitSync)(GLsync_, GLbitfield, uint64_t);
    void (*DeleteSync)(GLsync_);
    void (*GenVertexArrays)(GLsizei, GLuint *);
    void (*BindVertexArray)(GLuint);
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*UseProgram)(GLuint);
    void (*FrontFace)(GLenum);
    void (*ActiveTexture)(GLenum);
    void (*BindTexture)(GLenum, GLuint);
    void (*DeleteVertexArrays)(GLsizei, const GLuint *);
    GLboolean (*IsVertexArray)(GLuint);
    void *(*MapBuffer)(GLenum, GLenum);
    void *(*MapBufferRange)(GLenum, intptr_t, intptr_t, GLbitfield);
    void (*FlushMappedBufferRange)(GLenum, intptr_t, intptr_t);
    GLboolean (*UnmapBuffer)(GLenum);
    void (*GetBufferParameteriv)(GLenum, GLenum, GLint *);
    void (*StencilOpSeparate)(GLenum, GLenum, GLenum, GLenum);
    void (*StencilFuncSeparate)(GLenum, GLenum, GLint, GLuint);
    void (*DrawElements)(GLenum, GLsizei, GLenum, const void *);
    void (*DrawRangeElements)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
    void (*Clear)(GLbitfield);
    void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void (*CopyTexImage2D)(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint);
    void (*CopyTexSubImage2D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
    void (*GenerateMipmap)(GLenum);
    void (*EndQuery)(GLenum);
    void (*MultiDrawElementsIndirect)(GLenum, GLenum, const void *, GLsizei, GLsizei);
    void (*FramebufferTextureFace)(GLenum, GLenum, GLuint, GLint, GLenum);
} mesa;

static pthread_once_t mesa_once = PTHREAD_ONCE_INIT;
static void load_mesa(void)
{
#define LOAD(field, name) mesa.field = lp32_dri_proc_address(name)
    LOAD(GetString, "glGetString");
    LOAD(GetStringi, "glGetStringi");
    LOAD(GetIntegerv, "glGetIntegerv");
    LOAD(GetFloatv, "glGetFloatv");
    LOAD(GetBooleanv, "glGetBooleanv");
    LOAD(GetDoublev, "glGetDoublev");
    LOAD(PixelStorei, "glPixelStorei");
    LOAD(PixelStoref, "glPixelStoref");
    LOAD(TexParameteri, "glTexParameteri");
    LOAD(TexParameterf, "glTexParameterf");
    LOAD(TexParameteriv, "glTexParameteriv");
    LOAD(TexParameterfv, "glTexParameterfv");
    LOAD(TexImage2D, "glTexImage2D");
    LOAD(TexSubImage2D, "glTexSubImage2D");
    LOAD(CompressedTexImage2D, "glCompressedTexImage2D");
    LOAD(CompressedTexSubImage2D, "glCompressedTexSubImage2D");
    LOAD(TexImage3D, "glTexImage3D");
    LOAD(TexSubImage3D, "glTexSubImage3D");
    LOAD(CompressedTexImage3D, "glCompressedTexImage3D");
    LOAD(CompressedTexSubImage3D, "glCompressedTexSubImage3D");
    LOAD(BufferData, "glBufferData");
    LOAD(BufferSubData, "glBufferSubData");
    LOAD(Hint, "glHint");
    LOAD(LightModeli, "glLightModeli");
    LOAD(LightModelf, "glLightModelf");
    LOAD(LightModeliv, "glLightModeliv");
    LOAD(LightModelfv, "glLightModelfv");
    LOAD(Flush, "glFlush");
    LOAD(Finish, "glFinish");
    LOAD(FenceSync, "glFenceSync");
    LOAD(ClientWaitSync, "glClientWaitSync");
    LOAD(DeleteSync, "glDeleteSync");
    LOAD(GenVertexArrays, "glGenVertexArrays");
    LOAD(BindVertexArray, "glBindVertexArray");
    LOAD(GenFramebuffers, "glGenFramebuffers");
    LOAD(BindFramebuffer, "glBindFramebuffer");
    LOAD(UseProgram, "glUseProgram");
    LOAD(FrontFace, "glFrontFace");
    LOAD(ActiveTexture, "glActiveTexture");
    LOAD(BindTexture, "glBindTexture");
    LOAD(DeleteVertexArrays, "glDeleteVertexArrays");
    LOAD(IsVertexArray, "glIsVertexArray");
    LOAD(MapBuffer, "glMapBuffer");
    LOAD(MapBufferRange, "glMapBufferRange");
    LOAD(FlushMappedBufferRange, "glFlushMappedBufferRange");
    LOAD(UnmapBuffer, "glUnmapBuffer");
    LOAD(GetBufferParameteriv, "glGetBufferParameteriv");
    LOAD(StencilOpSeparate, "glStencilOpSeparate");
    LOAD(StencilFuncSeparate, "glStencilFuncSeparate");
    LOAD(DrawElements, "glDrawElements");
    LOAD(DrawRangeElements, "glDrawRangeElements");
    LOAD(DrawArrays, "glDrawArrays");
    LOAD(Clear, "glClear");
    LOAD(BlitFramebuffer, "glBlitFramebuffer");
    LOAD(CopyTexImage2D, "glCopyTexImage2D");
    LOAD(CopyTexSubImage2D, "glCopyTexSubImage2D");
    LOAD(GenerateMipmap, "glGenerateMipmap");
    LOAD(EndQuery, "glEndQuery");
    LOAD(MultiDrawElementsIndirect, "glMultiDrawElementsIndirect");
    LOAD(FramebufferTextureFace, "glFramebufferTextureFaceARB");
#undef LOAD
}

/* Raises GL_INVALID_OPERATION in the current context: ending a query that
   was never begun is an error in every profile. */
static void raise_invalid_operation(void)
{
    if (mesa.EndQuery) mesa.EndQuery(GL_SAMPLES_PASSED);
}
/* Raises GL_INVALID_ENUM. */
static void raise_invalid_enum(void)
{
    GLint dummy;
    if (mesa.GetIntegerv) mesa.GetIntegerv(0, &dummy);
}

/* ---- per-context state ------------------------------------------------ */

struct gl_state {
    CGLContextObj context;
    bool core;
    bool shares_uploads;  /* created to share with an existing context */
    bool presented;       /* has swapped a drawable: a render context */
    bool profile_ready;
    GLuint default_vertex_array;
    GLuint reserved_framebuffer;
    GLuint draw_framebuffer, read_framebuffer;
    GLuint current_program;
    GLenum front_face;
    GLenum active_texture;
    GLuint texture_2d[32], texture_3d[32], texture_cube[32], texture_rectangle[32];
    uint64_t texture_generation;
    uint64_t mirrored_texture_generation;
    char version[160];
    char *extension_string;
    const char **extensions;
    GLint extension_count;
    GLint client_storage;
    /* APPLE_fence: names are per context */
    GLsync_ *fences;
    GLuint fence_capacity;
    GLuint next_fence;
    /* APPLE_element_array */
    GLenum element_type;
    const void *element_pointer;
    /* APPLE_flush_buffer_range: whether each target's mapping is explicit */
    bool explicit_map[16];
};

static struct gl_state *current_state(void)
{
    CGLContextObj context = lp32gl_current_context();
    return context ? context->gl_state : NULL;
}

void *lp32gl_gl_state_create(CGLContextObj context)
{
    struct gl_state *state = calloc(1, sizeof *state);
    state->context = context;
    state->core = context->profile == LP32GL_PROFILE_CORE;
    /* A context created to share with an existing one (COD4's loader
       context, MW2's worker clones) uploads for another context to use. */
    state->shares_uploads =
        __atomic_load_n(&context->share_group->refcount, __ATOMIC_RELAXED) > 1;
    state->front_face = GL_CCW;
    state->active_texture = GL_TEXTURE0;
    return state;
}

void lp32gl_gl_state_destroy(void *pointer)
{
    struct gl_state *state = pointer;
    if (!state) return;
    free(state->extension_string);
    free(state->extensions);
    free(state->fences);
    free(state);
}

/* Mesa names a few extensions Apple lists differently. */
static const char *const extension_aliases[][2] = {
    {"GL_EXT_texture_rectangle", "GL_ARB_texture_rectangle"},
    {"GL_EXT_texture_env_add", "GL_ARB_texture_env_add"},
    {"GL_ATI_texture_float", "GL_ARB_texture_float"},
    {"GL_APPLE_float_pixels", "GL_ARB_texture_float"},
    {"GL_ARB_vertex_array_bgra", "GL_EXT_vertex_array_bgra"},
    {"GL_EXT_texture_compression_dxt1", "GL_EXT_texture_compression_s3tc"},
    {"GL_NV_depth_clamp", "GL_ARB_depth_clamp"},
    {"GL_ARB_provoking_vertex", "GL_EXT_provoking_vertex"},
    {"GL_APPLE_vertex_point_size", "GL_ARB_vertex_program"},
    {"GL_EXT_debug_label", "GL_KHR_debug"},
    {"GL_ATI_separate_stencil", "GL_EXT_stencil_two_side"},
};

/* Implemented here regardless of Mesa. */
static const char *const emulated_extensions[] = {
    "GL_APPLE_client_storage", "GL_APPLE_element_array", "GL_APPLE_fence",
    "GL_APPLE_flush_buffer_range", "GL_APPLE_flush_render", "GL_APPLE_packed_pixels",
    "GL_APPLE_pixel_buffer", "GL_APPLE_specular_vector", "GL_APPLE_texture_range",
    "GL_APPLE_transform_hint", "GL_APPLE_vertex_array_object", "GL_APPLE_aux_depth_stencil",
    "GL_EXT_debug_marker",
};

static bool has_name(const char *const *list, size_t count, const char *name)
{
    for (size_t i = 0; i < count; ++i)
        if (list[i] && !strcmp(list[i], name)) return true;
    return false;
}

static void prepare_profile(struct gl_state *state)
{
    if (state->profile_ready) return;
    pthread_once(&mesa_once, load_mesa);
    /* Mesa's own list for this context. */
    const char **mesa_list = NULL;
    size_t mesa_count = 0;
    char *mesa_copy = NULL;
    if (state->core) {
        GLint n = 0;
        mesa.GetIntegerv(GL_NUM_EXTENSIONS, &n);
        mesa_list = calloc((size_t)n + 1, sizeof *mesa_list);
        for (GLint i = 0; i < n; ++i) mesa_list[mesa_count++] = (const char *)mesa.GetStringi(GL_EXTENSIONS, (GLuint)i);
    } else {
        const char *all = (const char *)mesa.GetString(GL_EXTENSIONS);
        mesa_copy = strdup(all ? all : "");
        size_t cap = 64;
        mesa_list = calloc(cap, sizeof *mesa_list);
        for (char *token = strtok(mesa_copy, " "); token; token = strtok(NULL, " ")) {
            if (mesa_count + 1 >= cap) mesa_list = realloc(mesa_list, (cap *= 2) * sizeof *mesa_list);
            mesa_list[mesa_count++] = token;
        }
    }
    const char *const *apple = state->core ? apple_core_extensions : apple_legacy_extensions;
    size_t apple_count = 0;
    while (apple[apple_count]) ++apple_count;
    state->extensions = calloc(apple_count + 1, sizeof *state->extensions);
    size_t length = 1;
    for (size_t i = 0; i < apple_count; ++i) {
        const char *name = apple[i];
        bool supported = has_name(mesa_list, mesa_count, name) ||
                         has_name(emulated_extensions,
                                  sizeof emulated_extensions / sizeof emulated_extensions[0], name);
        for (size_t a = 0; !supported && a < sizeof extension_aliases / sizeof extension_aliases[0]; ++a)
            if (!strcmp(extension_aliases[a][0], name))
                supported = has_name(mesa_list, mesa_count, extension_aliases[a][1]);
        if (!supported) {
            if (getenv("LP32GL_DEBUG")) fprintf(stderr, "lp32gl: not advertising %s\n", name);
            continue;
        }
        state->extensions[state->extension_count++] = name;
        length += strlen(name) + 1;
    }
    state->extension_string = malloc(length);
    state->extension_string[0] = 0;
    for (GLint i = 0; i < state->extension_count; ++i) {
        if (i) strcat(state->extension_string, " ");
        strcat(state->extension_string, state->extensions[i]);
    }
    free(mesa_list);
    free(mesa_copy);
    snprintf(state->version, sizeof state->version, "%s LP32GL (Mesa %s)",
             state->core ? "4.1" : "2.1", MESA_VERSION_STRING);
    state->profile_ready = true;
}

void lp32gl_gl_context_made_current(CGLContextObj context)
{
    pthread_once(&mesa_once, load_mesa);
    struct gl_state *state = context->gl_state;
    if (state && !state->profile_ready) prepare_profile(state);
    /* Older Apple core-profile clients use attribute arrays before creating
       a VAO. Mesa requires one, so give each context a private default. */
    if (state && state->core && !state->default_vertex_array &&
        mesa.GenVertexArrays && mesa.BindVertexArray) {
        mesa.GenVertexArrays(1, &state->default_vertex_array);
        mesa.BindVertexArray(state->default_vertex_array);
    }
    if (state && context->mirror_context && mesa.BindFramebuffer) {
        struct gl_state *source = context->mirror_context->gl_state;
        GLuint source_draw = __atomic_load_n(&source->draw_framebuffer, __ATOMIC_ACQUIRE);
        GLuint source_read = __atomic_load_n(&source->read_framebuffer, __ATOMIC_ACQUIRE);
        GLuint source_program = __atomic_load_n(&source->current_program, __ATOMIC_ACQUIRE);
        GLenum source_front_face = __atomic_load_n(&source->front_face, __ATOMIC_ACQUIRE);
        GLuint wanted = source_draw > source_read ? source_draw : source_read;
        while (state->reserved_framebuffer < wanted && mesa.GenFramebuffers) {
            GLuint name = 0;
            mesa.GenFramebuffers(1, &name);
            if (!name || name <= state->reserved_framebuffer) break;
            state->reserved_framebuffer = name;
        }
        mesa.BindFramebuffer(GL_DRAW_FRAMEBUFFER, source_draw);
        mesa.BindFramebuffer(GL_READ_FRAMEBUFFER, source_read);
        __atomic_store_n(&state->draw_framebuffer, source_draw, __ATOMIC_RELEASE);
        __atomic_store_n(&state->read_framebuffer, source_read, __ATOMIC_RELEASE);
        if (mesa.UseProgram) {
            mesa.UseProgram(source_program);
            __atomic_store_n(&state->current_program, source_program, __ATOMIC_RELEASE);
        }
        if (mesa.FrontFace) {
            mesa.FrontFace(source_front_face);
            __atomic_store_n(&state->front_face, source_front_face, __ATOMIC_RELEASE);
        }
        uint64_t generation = __atomic_load_n(&source->texture_generation, __ATOMIC_ACQUIRE);
        if (generation != state->mirrored_texture_generation &&
            mesa.ActiveTexture && mesa.BindTexture) {
            for (unsigned unit = 0; unit < 32; ++unit) {
                GLuint texture_2d = __atomic_load_n(&source->texture_2d[unit], __ATOMIC_ACQUIRE);
                GLuint texture_3d = __atomic_load_n(&source->texture_3d[unit], __ATOMIC_ACQUIRE);
                GLuint texture_cube = __atomic_load_n(&source->texture_cube[unit], __ATOMIC_ACQUIRE);
                GLuint texture_rectangle = __atomic_load_n(&source->texture_rectangle[unit], __ATOMIC_ACQUIRE);
                if (!texture_2d && !texture_3d && !texture_cube && !texture_rectangle) continue;
                mesa.ActiveTexture(GL_TEXTURE0 + unit);
                if (texture_2d) mesa.BindTexture(GL_TEXTURE_2D, texture_2d);
                if (texture_3d) mesa.BindTexture(GL_TEXTURE_3D, texture_3d);
                if (texture_cube) mesa.BindTexture(GL_TEXTURE_CUBE_MAP, texture_cube);
                if (texture_rectangle) mesa.BindTexture(GL_TEXTURE_RECTANGLE_ARB, texture_rectangle);
                state->texture_2d[unit] = texture_2d;
                state->texture_3d[unit] = texture_3d;
                state->texture_cube[unit] = texture_cube;
                state->texture_rectangle[unit] = texture_rectangle;
            }
            GLenum active = __atomic_load_n(&source->active_texture, __ATOMIC_ACQUIRE);
            mesa.ActiveTexture(active);
            state->active_texture = active;
            state->mirrored_texture_generation = generation;
        }
    }
}

static void lp32_glActiveTexture(GLenum unit)
{
    mesa.ActiveTexture(unit);
    struct gl_state *state = current_state();
    if (state) {
        __atomic_store_n(&state->active_texture, unit, __ATOMIC_RELEASE);
        __atomic_add_fetch(&state->texture_generation, 1, __ATOMIC_RELEASE);
    }
}

static void lp32_glBindTexture(GLenum target, GLuint texture)
{
    mesa.BindTexture(target, texture);
    struct gl_state *state = current_state();
    if (!state) return;
    GLenum active = __atomic_load_n(&state->active_texture, __ATOMIC_ACQUIRE);
    if (active < GL_TEXTURE0 || active >= GL_TEXTURE0 + 32) return;
    unsigned unit = active - GL_TEXTURE0;
    GLuint *slot = target == GL_TEXTURE_2D ? &state->texture_2d[unit] :
                   target == GL_TEXTURE_3D ? &state->texture_3d[unit] :
                   target == GL_TEXTURE_CUBE_MAP ? &state->texture_cube[unit] :
                   target == GL_TEXTURE_RECTANGLE_ARB ? &state->texture_rectangle[unit] : NULL;
    if (slot) {
        __atomic_store_n(slot, texture, __ATOMIC_RELEASE);
        __atomic_add_fetch(&state->texture_generation, 1, __ATOMIC_RELEASE);
    }
}

void lp32gl_gl_context_presented(CGLContextObj context)
{
    struct gl_state *state = context->gl_state;
    if (state) state->presented = true;
}

/* Apple makes an upload on one context visible to the contexts sharing with
 * it without a glFlush; COD4 loads level data on a shared context on its own
 * thread, and MW2 uses one public context across several threads (separate
 * Mesa clones here). A Mesa context keeps its copies in its own unsubmitted
 * batch, so publish uploads from those contexts. A flush submits work without
 * waiting for it. */
static void publish_upload(void)
{
    struct gl_state *state = current_state();
    if (state && state->shares_uploads && !state->presented) mesa.Flush();
}

/* A Zink context that needs a resource another context used in an unflushed
 * batch blocks until that context flushes, and a Metal queue left waiting on
 * such work times out (device lost). Worker contexts, which share with
 * another and never present, can go idle with work pending for seconds, so
 * they publish every draw, clear, copy and mipmap generation. Render
 * contexts flush at every swap anyway. */
#define publish_work publish_upload

static void lp32_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    mesa.DrawArrays(mode, first, count);
    publish_work();
}

static void lp32_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    mesa.DrawElements(mode, count, type, indices);
    publish_work();
}

static void lp32_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count,
                                     GLenum type, const void *indices)
{
    mesa.DrawRangeElements(mode, start, end, count, type, indices);
    publish_work();
}

static void lp32_glClear(GLbitfield mask)
{
    mesa.Clear(mask);
    publish_work();
}

static void lp32_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
                                   GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
    mesa.BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
    publish_work();
}

static void lp32_glCopyTexImage2D(GLenum target, GLint level, GLenum internal_format, GLint x,
                                  GLint y, GLsizei width, GLsizei height, GLint border)
{
    mesa.CopyTexImage2D(target, level, internal_format, x, y, width, height, border);
    publish_work();
}

static void lp32_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                     GLint x, GLint y, GLsizei width, GLsizei height)
{
    mesa.CopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
    publish_work();
}

static void lp32_glGenerateMipmap(GLenum target)
{
    mesa.GenerateMipmap(target);
    publish_work();
}

/* MW2's core render context also uploads textures its worker clones sample. */
static void publish_texture_upload(void)
{
    struct gl_state *state = current_state();
    if (state && (state->core || state->shares_uploads)) mesa.Flush();
}

static void lp32_glTexImage2D(GLenum target, GLint level, GLint internal_format,
                             GLsizei width, GLsizei height, GLint border,
                             GLenum format, GLenum type, const void *pixels)
{
    mesa.TexImage2D(target, level, internal_format, width, height, border,
                    format, type, pixels);
    publish_texture_upload();
}

static void lp32_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                                GLsizei width, GLsizei height, GLenum format,
                                GLenum type, const void *pixels)
{
    mesa.TexSubImage2D(target, level, x, y, width, height, format, type, pixels);
    publish_texture_upload();
}

static void lp32_glCompressedTexImage2D(GLenum target, GLint level,
                                       GLenum internal_format, GLsizei width,
                                       GLsizei height, GLint border,
                                       GLsizei image_size, const void *data)
{
    mesa.CompressedTexImage2D(target, level, internal_format, width, height,
                              border, image_size, data);
    publish_texture_upload();
}

static void lp32_glCompressedTexSubImage2D(GLenum target, GLint level,
                                          GLint x, GLint y, GLsizei width,
                                          GLsizei height, GLenum format,
                                          GLsizei image_size, const void *data)
{
    mesa.CompressedTexSubImage2D(target, level, x, y, width, height, format,
                                 image_size, data);
    publish_texture_upload();
}

static void lp32_glTexImage3D(GLenum target, GLint level, GLint internal_format, GLsizei width,
                              GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type,
                              const void *pixels)
{
    mesa.TexImage3D(target, level, internal_format, width, height, depth, border, format, type, pixels);
    publish_texture_upload();
}

static void lp32_glTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width,
                                 GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                 const void *pixels)
{
    mesa.TexSubImage3D(target, level, x, y, z, width, height, depth, format, type, pixels);
    publish_texture_upload();
}

static void lp32_glCompressedTexImage3D(GLenum target, GLint level, GLenum internal_format,
                                        GLsizei width, GLsizei height, GLsizei depth, GLint border,
                                        GLsizei image_size, const void *data)
{
    mesa.CompressedTexImage3D(target, level, internal_format, width, height, depth, border,
                              image_size, data);
    publish_texture_upload();
}

static void lp32_glCompressedTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z,
                                           GLsizei width, GLsizei height, GLsizei depth,
                                           GLenum format, GLsizei image_size, const void *data)
{
    mesa.CompressedTexSubImage3D(target, level, x, y, z, width, height, depth, format, image_size,
                                 data);
    publish_texture_upload();
}

static void lp32_glBufferData(GLenum target, intptr_t size, const void *data, GLenum usage)
{
    mesa.BufferData(target, size, data, usage);
    if (data) publish_upload();
}

static void lp32_glBufferSubData(GLenum target, intptr_t offset, intptr_t size, const void *data)
{
    mesa.BufferSubData(target, offset, size, data);
    publish_upload();
}

static void lp32_glActiveTextureARB(GLenum unit) { lp32_glActiveTexture(unit); }

static void lp32_glUseProgram(GLuint program)
{
    mesa.UseProgram(program);
    struct gl_state *state = current_state();
    if (state) __atomic_store_n(&state->current_program, program, __ATOMIC_RELEASE);
}

static void lp32_glUseProgramObjectARB(GLuint program)
{ lp32_glUseProgram(program); }

static void lp32_glFrontFace(GLenum mode)
{
    mesa.FrontFace(mode);
    struct gl_state *state = current_state();
    if (state) __atomic_store_n(&state->front_face, mode, __ATOMIC_RELEASE);
}

static void lp32_glGenFramebuffers(GLsizei count, GLuint *names)
{
    mesa.GenFramebuffers(count, names);
    struct gl_state *state = current_state();
    if (state && state->context->mirror_context && names)
        for (GLsizei i = 0; i < count; ++i)
            if (names[i] > state->reserved_framebuffer)
                state->reserved_framebuffer = names[i];
}

static void lp32_glBindFramebuffer(GLenum target, GLuint name)
{
    struct gl_state *state = current_state();
    if (state && state->context->mirror_context && mesa.GenFramebuffers)
        while (state->reserved_framebuffer < name) {
            GLuint generated = 0;
            mesa.GenFramebuffers(1, &generated);
            if (!generated || generated <= state->reserved_framebuffer) break;
            state->reserved_framebuffer = generated;
        }
    mesa.BindFramebuffer(target, name);
    if (state) {
        if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
            __atomic_store_n(&state->draw_framebuffer, name, __ATOMIC_RELEASE);
        if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)
            __atomic_store_n(&state->read_framebuffer, name, __ATOMIC_RELEASE);
    }
}

static void lp32_glGenFramebuffersEXT(GLsizei count, GLuint *names)
{ lp32_glGenFramebuffers(count, names); }
static void lp32_glBindFramebufferEXT(GLenum target, GLuint name)
{ lp32_glBindFramebuffer(target, name); }

/* ---- queries ---------------------------------------------------------- */

static const char *renderer_name(void);

static const GLubyte *lp32_glGetString(GLenum name)
{
    struct gl_state *state = current_state();
    if (!state) return mesa.GetString(name);
    prepare_profile(state);
    switch (name) {
    case GL_VENDOR: return (const GLubyte *)"Apple";
    case GL_RENDERER: return (const GLubyte *)renderer_name();
    case GL_VERSION: return (const GLubyte *)state->version;
    case GL_SHADING_LANGUAGE_VERSION:
        return (const GLubyte *)(state->core ? apple_core_glsl : apple_legacy_glsl);
    case GL_EXTENSIONS:
        if (state->core) { raise_invalid_enum(); return NULL; }
        return (const GLubyte *)state->extension_string;
    default: return mesa.GetString(name);
    }
}

static const GLubyte *lp32_glGetStringi(GLenum name, GLuint index)
{
    struct gl_state *state = current_state();
    if (!state || name != GL_EXTENSIONS) return mesa.GetStringi(name, index);
    prepare_profile(state);
    if (index >= (GLuint)state->extension_count) { mesa.GetStringi(name, 0xffffffffu); return NULL; }
    return (const GLubyte *)state->extensions[index];
}

static bool clamp_limit(struct gl_state *state, GLenum pname, GLint *value)
{
    const struct lp32gl_limit *limits = state->core ? apple_core_limits : apple_legacy_limits;
    for (; limits->pname; ++limits)
        if (limits->pname == pname) {
            if (*value > limits->value) *value = limits->value;
            return true;
        }
    return false;
}

/* Answers the queries whose value LP32GL owns.  Returns false for the rest. */
static bool profile_query(struct gl_state *state, GLenum pname, GLint *value)
{
    switch (pname) {
    case GL_NUM_EXTENSIONS:
        if (!state->core) return false;
        prepare_profile(state);
        *value = state->extension_count;
        return true;
    case GL_MAJOR_VERSION:
        if (!state->core) return false;
        *value = 4;
        return true;
    case GL_MINOR_VERSION:
        if (!state->core) return false;
        *value = 1;
        return true;
    case GL_UNPACK_CLIENT_STORAGE_APPLE_:
        *value = state->client_storage;
        return true;
    default:
        return false;
    }
}

static void lp32_glGetIntegerv(GLenum pname, GLint *params)
{
    struct gl_state *state = current_state();
    if (state && profile_query(state, pname, params)) return;
    mesa.GetIntegerv(pname, params);
    if (state) clamp_limit(state, pname, params);
}

static void lp32_glGetFloatv(GLenum pname, GLfloat *params)
{
    struct gl_state *state = current_state();
    GLint value;
    if (state && profile_query(state, pname, &value)) { *params = (GLfloat)value; return; }
    mesa.GetFloatv(pname, params);
    if (state) {
        value = (GLint)params[0];
        if (clamp_limit(state, pname, &value)) params[0] = (GLfloat)value;
    }
}

static void lp32_glGetBooleanv(GLenum pname, GLboolean *params)
{
    struct gl_state *state = current_state();
    GLint value;
    if (state && profile_query(state, pname, &value)) { *params = value != 0; return; }
    mesa.GetBooleanv(pname, params);
}

static void lp32_glGetDoublev(GLenum pname, GLdouble *params)
{
    struct gl_state *state = current_state();
    GLint value;
    if (state && profile_query(state, pname, &value)) { *params = value; return; }
    mesa.GetDoublev(pname, params);
    if (state) {
        value = (GLint)params[0];
        if (clamp_limit(state, pname, &value)) params[0] = value;
    }
}

/* ---- Apple-only enums on core entry points ---------------------------- */

static void lp32_glPixelStorei(GLenum pname, GLint param)
{
    switch (pname) {
    case GL_UNPACK_CLIENT_STORAGE_APPLE_: {
        /* A promise that the client keeps the pixels alive; Mesa copies
           them, which satisfies the same contract. */
        struct gl_state *state = current_state();
        if (state) state->client_storage = param != 0;
        return;
    }
    case GL_PACK_ROW_BYTES_APPLE_:
    case GL_UNPACK_ROW_BYTES_APPLE_:
        if (param != 0) raise_invalid_enum(); /* APPLE_row_bytes is not advertised */
        return;
    default:
        mesa.PixelStorei(pname, param);
    }
}

static void lp32_glPixelStoref(GLenum pname, GLfloat param)
{
    if (pname == GL_UNPACK_CLIENT_STORAGE_APPLE_ || pname == GL_PACK_ROW_BYTES_APPLE_ ||
        pname == GL_UNPACK_ROW_BYTES_APPLE_) {
        lp32_glPixelStorei(pname, (GLint)param);
        return;
    }
    mesa.PixelStoref(pname, param);
}

static bool texture_hint(GLenum pname)
{
    return pname == GL_TEXTURE_STORAGE_HINT_APPLE_;
}

static void lp32_glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    if (texture_hint(pname)) return;
    mesa.TexParameteri(target, pname, param);
}
static void lp32_glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    if (texture_hint(pname)) return;
    mesa.TexParameterf(target, pname, param);
}
static void lp32_glTexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
    if (texture_hint(pname)) return;
    mesa.TexParameteriv(target, pname, params);
}
static void lp32_glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
    if (texture_hint(pname)) return;
    mesa.TexParameterfv(target, pname, params);
}

static void lp32_glHint(GLenum target, GLenum mode)
{
    if (target == GL_TRANSFORM_HINT_APPLE_) return;
    mesa.Hint(target, mode);
}

static void lp32_glLightModeli(GLenum pname, GLint param)
{
    if (pname == GL_LIGHT_MODEL_SPECULAR_VECTOR_APPLE_) return;
    mesa.LightModeli(pname, param);
}
static void lp32_glLightModelf(GLenum pname, GLfloat param)
{
    if (pname == GL_LIGHT_MODEL_SPECULAR_VECTOR_APPLE_) return;
    mesa.LightModelf(pname, param);
}
static void lp32_glLightModeliv(GLenum pname, const GLint *params)
{
    if (pname == GL_LIGHT_MODEL_SPECULAR_VECTOR_APPLE_) return;
    mesa.LightModeliv(pname, params);
}
static void lp32_glLightModelfv(GLenum pname, const GLfloat *params)
{
    if (pname == GL_LIGHT_MODEL_SPECULAR_VECTOR_APPLE_) return;
    mesa.LightModelfv(pname, params);
}

/* ---- APPLE_fence -------------------------------------------------------- */

static void lp32_glGenFencesAPPLE(GLsizei n, GLuint *fences)
{
    struct gl_state *state = current_state();
    if (!state || n < 0) { raise_invalid_operation(); return; }
    for (GLsizei i = 0; i < n; ++i) {
        GLuint name = ++state->next_fence;
        if (name >= state->fence_capacity) {
            GLuint capacity = state->fence_capacity ? state->fence_capacity * 2 : 64;
            while (capacity <= name) capacity *= 2;
            state->fences = realloc(state->fences, capacity * sizeof *state->fences);
            memset(state->fences + state->fence_capacity, 0,
                   (capacity - state->fence_capacity) * sizeof *state->fences);
            state->fence_capacity = capacity;
        }
        state->fences[name] = NULL;
        fences[i] = name;
    }
}

static GLsync_ *fence_slot(struct gl_state *state, GLuint name)
{
    return state && name && name < state->fence_capacity ? &state->fences[name] : NULL;
}

static void lp32_glDeleteFencesAPPLE(GLsizei n, const GLuint *fences)
{
    struct gl_state *state = current_state();
    for (GLsizei i = 0; i < n; ++i) {
        GLsync_ *slot = fence_slot(state, fences[i]);
        if (slot && *slot) {
            mesa.DeleteSync(*slot);
            *slot = NULL;
        }
    }
}

static void lp32_glSetFenceAPPLE(GLuint fence)
{
    struct gl_state *state = current_state();
    GLsync_ *slot = fence_slot(state, fence);
    if (!slot) { raise_invalid_operation(); return; }
    if (*slot) mesa.DeleteSync(*slot);
    *slot = mesa.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

static GLboolean lp32_glIsFenceAPPLE(GLuint fence)
{
    GLsync_ *slot = fence_slot(current_state(), fence);
    return slot && *slot;
}

static GLboolean test_sync(GLsync_ *slot)
{
    if (!slot || !*slot) return GL_TRUE;
    GLenum result = mesa.ClientWaitSync(*slot, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
    return result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
}

static GLboolean lp32_glTestFenceAPPLE(GLuint fence)
{
    return test_sync(fence_slot(current_state(), fence));
}

static void lp32_glFinishFenceAPPLE(GLuint fence)
{
    GLsync_ *slot = fence_slot(current_state(), fence);
    if (slot && *slot) mesa.ClientWaitSync(*slot, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
}

/* Object-level tests: a fence after everything issued so far covers every
   prior use of the object, which is what the extension promises. */
static GLboolean lp32_glTestObjectAPPLE(GLenum object, GLuint name)
{
    (void)object; (void)name;
    GLsync_ sync = mesa.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    GLboolean done = test_sync(&sync);
    mesa.DeleteSync(sync);
    return done;
}

static void lp32_glFinishObjectAPPLE(GLenum object, GLint name)
{
    (void)object; (void)name;
    GLsync_ sync = mesa.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    mesa.ClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
    mesa.DeleteSync(sync);
}

/* ---- APPLE_vertex_array_object ------------------------------------------ */

static void lp32_glGenVertexArraysAPPLE(GLsizei n, GLuint *arrays) { mesa.GenVertexArrays(n, arrays); }
static void lp32_glBindVertexArrayAPPLE(GLuint array) { mesa.BindVertexArray(array); }
static void lp32_glDeleteVertexArraysAPPLE(GLsizei n, const GLuint *arrays)
{
    mesa.DeleteVertexArrays(n, arrays);
}
static GLboolean lp32_glIsVertexArrayAPPLE(GLuint array) { return mesa.IsVertexArray(array); }

/* ---- APPLE_flush_buffer_range ------------------------------------------- */

struct buffer_flags {
    const void *share_group;
    GLuint buffer;
    bool serialized_modify; /* default true */
    bool flushing_unmap;    /* default true */
};
static struct buffer_flags *buffer_flags;
static size_t buffer_flag_count, buffer_flag_capacity;
static pthread_mutex_t buffer_lock = PTHREAD_MUTEX_INITIALIZER;

static GLenum binding_for_target(GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
    case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
    case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
    case 0x8A11 /* GL_UNIFORM_BUFFER */: return 0x8A28;
    case 0x8C2A /* GL_TEXTURE_BUFFER */: return 0x8C2A;
    case 0x8F36 /* GL_COPY_READ_BUFFER */: return 0x8F36;
    case 0x8F37 /* GL_COPY_WRITE_BUFFER */: return 0x8F37;
    case 0x8C8E /* GL_TRANSFORM_FEEDBACK_BUFFER */: return 0x8C8F;
    default: return 0;
    }
}

static unsigned target_slot(GLenum target)
{
    return (unsigned)(target * 2654435761u) >> 28;
}

static struct buffer_flags *find_flags(const void *share_group, GLuint buffer, bool create)
{
    for (size_t i = 0; i < buffer_flag_count; ++i)
        if (buffer_flags[i].share_group == share_group && buffer_flags[i].buffer == buffer)
            return &buffer_flags[i];
    if (!create) return NULL;
    if (buffer_flag_count == buffer_flag_capacity) {
        buffer_flag_capacity = buffer_flag_capacity ? buffer_flag_capacity * 2 : 64;
        buffer_flags = realloc(buffer_flags, buffer_flag_capacity * sizeof *buffer_flags);
    }
    struct buffer_flags *flags = &buffer_flags[buffer_flag_count++];
    flags->share_group = share_group;
    flags->buffer = buffer;
    flags->serialized_modify = true;
    flags->flushing_unmap = true;
    return flags;
}

static GLuint bound_buffer(GLenum target)
{
    GLenum binding = binding_for_target(target);
    GLint buffer = 0;
    if (binding) mesa.GetIntegerv(binding, &buffer);
    return (GLuint)buffer;
}

static void lp32_glBufferParameteriAPPLE(GLenum target, GLenum pname, GLint param)
{
    struct gl_state *state = current_state();
    if (!state) return;
    GLuint buffer = bound_buffer(target);
    if (!buffer) { raise_invalid_operation(); return; }
    if (pname != GL_BUFFER_SERIALIZED_MODIFY_APPLE_ && pname != GL_BUFFER_FLUSHING_UNMAP_APPLE_) {
        raise_invalid_enum();
        return;
    }
    pthread_mutex_lock(&buffer_lock);
    struct buffer_flags *flags = find_flags(state->context->share_group, buffer, true);
    if (pname == GL_BUFFER_SERIALIZED_MODIFY_APPLE_) flags->serialized_modify = param != 0;
    else flags->flushing_unmap = param != 0;
    pthread_mutex_unlock(&buffer_lock);
}

static void *map_buffer(GLenum target, GLenum access)
{
    struct gl_state *state = current_state();
    if (!state) return mesa.MapBuffer(target, access);
    bool serialized = true, flushing = true;
    pthread_mutex_lock(&buffer_lock);
    if (buffer_flag_count) {
        struct buffer_flags *flags = find_flags(state->context->share_group, bound_buffer(target), false);
        if (flags) {
            serialized = flags->serialized_modify;
            flushing = flags->flushing_unmap;
        }
    }
    pthread_mutex_unlock(&buffer_lock);
    state->explicit_map[target_slot(target)] = false;
    if ((serialized && flushing) || access == GL_READ_ONLY) return mesa.MapBuffer(target, access);
    GLint size = 0;
    mesa.GetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    GLbitfield bits = GL_MAP_WRITE_BIT | (access == GL_READ_WRITE ? GL_MAP_READ_BIT : 0);
    if (!flushing) bits |= GL_MAP_FLUSH_EXPLICIT_BIT;
    if (!serialized) bits |= GL_MAP_UNSYNCHRONIZED_BIT;
    void *pointer = mesa.MapBufferRange(target, 0, size, bits);
    state->explicit_map[target_slot(target)] = !flushing && pointer;
    return pointer;
}

static void *lp32_glMapBuffer(GLenum target, GLenum access) { return map_buffer(target, access); }

static void lp32_glFlushMappedBufferRangeAPPLE(GLenum target, intptr_t offset, intptr_t size)
{
    struct gl_state *state = current_state();
    /* Only meaningful for a mapping made with FLUSHING_UNMAP off; Apple
       accepts it otherwise, and the unmap flushes everything anyway. */
    if (state && state->explicit_map[target_slot(target)])
        mesa.FlushMappedBufferRange(target, offset, size);
}

static GLboolean lp32_glUnmapBuffer(GLenum target)
{
    struct gl_state *state = current_state();
    if (state) state->explicit_map[target_slot(target)] = false;
    GLboolean result = mesa.UnmapBuffer(target);
    publish_upload();
    return result;
}

/* ---- APPLE_element_array ------------------------------------------------ */

static size_t index_size(GLenum type)
{
    return type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
}

static void lp32_glElementPointerAPPLE(GLenum type, const void *pointer)
{
    struct gl_state *state = current_state();
    if (!state) return;
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT) {
        raise_invalid_enum();
        return;
    }
    state->element_type = type;
    state->element_pointer = pointer;
}

static void lp32_glDrawElementArrayAPPLE(GLenum mode, GLint first, GLsizei count)
{
    struct gl_state *state = current_state();
    if (!state || !state->element_type) { raise_invalid_operation(); return; }
    mesa.DrawElements(mode, count, state->element_type,
                      (const char *)state->element_pointer + (size_t)first * index_size(state->element_type));
}

static void lp32_glDrawRangeElementArrayAPPLE(GLenum mode, GLuint start, GLuint end, GLint first,
                                              GLsizei count)
{
    struct gl_state *state = current_state();
    if (!state || !state->element_type) { raise_invalid_operation(); return; }
    mesa.DrawRangeElements(mode, start, end, count, state->element_type,
                           (const char *)state->element_pointer +
                               (size_t)first * index_size(state->element_type));
}

static void lp32_glMultiDrawElementArrayAPPLE(GLenum mode, const GLint *first, const GLsizei *count,
                                              GLsizei primcount)
{
    for (GLsizei i = 0; i < primcount; ++i) lp32_glDrawElementArrayAPPLE(mode, first[i], count[i]);
}

static void lp32_glMultiDrawRangeElementArrayAPPLE(GLenum mode, GLuint start, GLuint end,
                                                   const GLint *first, const GLsizei *count,
                                                   GLsizei primcount)
{
    for (GLsizei i = 0; i < primcount; ++i)
        lp32_glDrawRangeElementArrayAPPLE(mode, start, end, first[i], count[i]);
}

/* ---- APPLE_flush_render ------------------------------------------------- */

static void lp32_glFlushRenderAPPLE(void) { mesa.Flush(); }
static void lp32_glFinishRenderAPPLE(void) { mesa.Finish(); }
static void lp32_glSwapAPPLE(void)
{
    CGLContextObj context = lp32gl_current_context();
    if (context) CGLFlushDrawable(context);
}

/* ---- hints with no observable effect ------------------------------------ */

static void lp32_glTextureRangeAPPLE(GLenum target, GLsizei length, const void *pointer)
{
    (void)target; (void)length; (void)pointer;
}
static void lp32_glGetTexParameterPointervAPPLE(GLenum target, GLenum pname, void **params)
{
    (void)target;
    if (pname != GL_TEXTURE_RANGE_POINTER_APPLE_) { raise_invalid_enum(); return; }
    *params = NULL;
}
static void lp32_glVertexArrayRangeAPPLE(GLsizei length, void *pointer) { (void)length; (void)pointer; }
static void lp32_glFlushVertexArrayRangeAPPLE(GLsizei length, void *pointer) { (void)length; (void)pointer; }
static void lp32_glVertexArrayParameteriAPPLE(GLenum pname, GLint param) { (void)pname; (void)param; }
static GLenum lp32_glObjectPurgeableAPPLE(GLenum type, GLuint name, GLenum option)
{
    (void)type; (void)name;
    return option == GL_VOLATILE_APPLE_ ? GL_VOLATILE_APPLE_ : GL_RELEASED_APPLE_;
}
static GLenum lp32_glObjectUnpurgeableAPPLE(GLenum type, GLuint name, GLenum option)
{
    (void)type; (void)name; (void)option;
    return GL_RETAINED_APPLE_; /* contents never discarded */
}
static void lp32_glGetObjectParameterivAPPLE(GLenum type, GLuint name, GLenum pname, GLint *params)
{
    (void)type; (void)name; (void)pname;
    *params = GL_FALSE;
}
static void lp32_glInsertEventMarkerEXT(GLsizei length, const char *marker) { (void)length; (void)marker; }
static void lp32_glPushGroupMarkerEXT(GLsizei length, const char *marker) { (void)length; (void)marker; }
static void lp32_glPopGroupMarkerEXT(void) {}
static void lp32_glLabelObjectWithResponsibleProcessAPPLE(GLenum type, GLuint name, GLint pid)
{
    (void)type; (void)name; (void)pid;
}

/* ---- ATI_separate_stencil ----------------------------------------------- */

static void lp32_glStencilOpSeparateATI(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass)
{
    mesa.StencilOpSeparate(face, sfail, dpfail, dppass);
}
static void lp32_glStencilFuncSeparateATI(GLenum frontfunc, GLenum backfunc, GLint ref, GLuint mask)
{
    mesa.StencilFuncSeparate(GL_FRONT, frontfunc, ref, mask);
    mesa.StencilFuncSeparate(GL_BACK, backfunc, ref, mask);
}

/* ---- forwarders to differently named Mesa entry points ------------------ */

static void lp32_glMultiDrawElementsIndirectAPPLE(GLenum mode, GLenum type, const void *indirect,
                                                  GLsizei drawcount, GLsizei stride)
{
    if (mesa.MultiDrawElementsIndirect) mesa.MultiDrawElementsIndirect(mode, type, indirect, drawcount, stride);
    else raise_invalid_operation();
}
static void lp32_glFramebufferTextureFaceEXT(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                             GLenum face)
{
    if (mesa.FramebufferTextureFace) mesa.FramebufferTextureFace(target, attachment, texture, level, face);
    else raise_invalid_operation();
}

/* ---- entry points with no implementation -------------------------------- */

/* Extensions Apple exports but only advertises on other hardware (NV
   register combiners, ATI PN triangles, ARB vertex blend, EXT color tables,
   EXT_bindable_uniform, APPLE vertex program evaluators).  Callers that
   ignore the extension string get GL_INVALID_OPERATION and a zero result. */
static uintptr_t lp32_unsupported(void)
{
    raise_invalid_operation();
    return 0;
}

static const char *const unsupported_entries[] = {
    "glColorTableEXT", "glGetColorTableEXT", "glGetColorTableParameterfvEXT",
    "glGetColorTableParameterivEXT", "glCombinerInputNV", "glCombinerOutputNV",
    "glCombinerParameterfNV", "glCombinerParameterfvNV", "glCombinerParameteriNV",
    "glCombinerParameterivNV", "glCombinerStageParameterfvNV", "glFinalCombinerInputNV",
    "glGetCombinerInputParameterfvNV", "glGetCombinerInputParameterivNV",
    "glGetCombinerOutputParameterfvNV", "glGetCombinerOutputParameterivNV",
    "glGetCombinerStageParameterfvNV", "glGetFinalCombinerInputParameterfvNV",
    "glGetFinalCombinerInputParameterivNV", "glGetUniformBufferSizeEXT", "glGetUniformOffsetEXT",
    "glUniformBufferEXT", "glDisableVertexAttribAPPLE", "glEnableVertexAttribAPPLE",
    "glIsVertexAttribEnabledAPPLE", "glMapVertexAttrib1dAPPLE", "glMapVertexAttrib1fAPPLE",
    "glMapVertexAttrib2dAPPLE", "glMapVertexAttrib2fAPPLE",
    "glMultiDrawElementsIndirectBaseInstanceAPPLE", "glPNTrianglesfATI", "glPNTrianglesfATIX",
    "glPNTrianglesiATI", "glPNTrianglesiATIX", "glPointSizePointerAPPLE", "glSamplePass",
    "glSamplePassARB", "glVertexBlendARB", "glVertexPointSizefAPPLE", "glWeightPointerARB",
    "glWeightbvARB", "glWeightdvARB", "glWeightfvARB", "glWeightivARB", "glWeightsvARB",
    "glWeightubvARB", "glWeightuivARB", "glWeightusvARB",
};

/* ---- diagnostics (LP32GL_CHECK_DRAWS=1) ---------------------------------- */

/* Before each draw, reports an incomplete draw framebuffer; after it, any GL
   error the draw raised.  Each distinct (call, framebuffer, status) is
   reported once.  Consumes GL errors, so only for debugging. */
static struct {
    GLenum (*CheckFramebufferStatus)(GLenum);
    GLenum (*GetError)(void);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
    void (*DrawElements)(GLenum, GLsizei, GLenum, const void *);
    void (*DrawRangeElements)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
    void (*GetFramebufferAttachmentParameteriv)(GLenum, GLenum, GLenum, GLint *);
} check;
static pthread_mutex_t check_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t reported[512];
static unsigned reported_count;

static bool first_report(uint64_t key)
{
    bool first = true;
    pthread_mutex_lock(&check_lock);
    for (unsigned i = 0; i < reported_count; ++i)
        if (reported[i] == key) { first = false; break; }
    if (first && reported_count < 512) reported[reported_count++] = key;
    pthread_mutex_unlock(&check_lock);
    return first;
}

static void check_before_draw(const char *call)
{
    GLint fbo = 0;
    mesa.GetIntegerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &fbo);
    GLenum status = check.CheckFramebufferStatus(0x8CA9 /* GL_DRAW_FRAMEBUFFER */);
    if (status != 0x8CD5 /* COMPLETE */ && first_report(((uint64_t)fbo << 32) | status)) {
        fprintf(stderr, "lp32gl: %s into incomplete framebuffer %d (status 0x%x)", call, fbo, status);
        for (GLenum attachment = 0x8CE0; fbo && attachment <= 0x8D20; attachment += attachment == 0x8CE7 ? 0x8D00 - 0x8CE7 : 1) {
            GLint type = 0, name = 0;
            check.GetFramebufferAttachmentParameteriv(0x8CA9, attachment, 0x8CD0, &type);
            if (!type) continue;
            check.GetFramebufferAttachmentParameteriv(0x8CA9, attachment, 0x8CD1, &name);
            fprintf(stderr, " att0x%x=%s:%d", attachment, type == 0x1702 ? "tex" : "rb", name);
        }
        fprintf(stderr, "\n");
    }
}

static void check_after_draw(const char *call)
{
    GLenum error = check.GetError();
    if (error && first_report(((uint64_t)(uintptr_t)call << 16) ^ error))
        fprintf(stderr, "lp32gl: %s raised GL error 0x%x\n", call, error);
}

static void checked_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    check_before_draw("glDrawArrays");
    check.DrawArrays(mode, first, count);
    check_after_draw("glDrawArrays");
}
static void checked_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    check_before_draw("glDrawElements");
    check.DrawElements(mode, count, type, indices);
    check_after_draw("glDrawElements");
}
static void checked_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                        const void *indices)
{
    check_before_draw("glDrawRangeElements");
    check.DrawRangeElements(mode, start, end, count, type, indices);
    check_after_draw("glDrawRangeElements");
}

static void *checked_entry(const char *name)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("LP32GL_CHECK_DRAWS") != NULL;
        if (enabled) {
            check.CheckFramebufferStatus = lp32_dri_proc_address("glCheckFramebufferStatus");
            check.GetError = lp32_dri_proc_address("glGetError");
            check.DrawArrays = lp32_dri_proc_address("glDrawArrays");
            check.DrawElements = lp32_dri_proc_address("glDrawElements");
            check.DrawRangeElements = lp32_dri_proc_address("glDrawRangeElements");
            check.GetFramebufferAttachmentParameteriv =
                lp32_dri_proc_address("glGetFramebufferAttachmentParameteriv");
        }
    }
    if (!enabled) return NULL;
    if (!strcmp(name, "glDrawArrays") || !strcmp(name, "glDrawArraysEXT")) return (void *)checked_glDrawArrays;
    if (!strcmp(name, "glDrawElements")) return (void *)checked_glDrawElements;
    if (!strcmp(name, "glDrawRangeElements") || !strcmp(name, "glDrawRangeElementsEXT"))
        return (void *)checked_glDrawRangeElements;
    return NULL;
}

/* ---- lookup --------------------------------------------------------------- */

#define OVERRIDE(name) {#name, (void *)lp32_##name}
static const struct { const char *name; void *function; } overrides[] = {
    OVERRIDE(glGenFramebuffers), OVERRIDE(glGenFramebuffersEXT),
    OVERRIDE(glBindFramebuffer), OVERRIDE(glBindFramebufferEXT),
    OVERRIDE(glUseProgram), OVERRIDE(glUseProgramObjectARB),
    OVERRIDE(glFrontFace),
    OVERRIDE(glActiveTexture), OVERRIDE(glActiveTextureARB), OVERRIDE(glBindTexture),
    OVERRIDE(glTexImage2D), OVERRIDE(glTexSubImage2D),
    OVERRIDE(glCompressedTexImage2D), OVERRIDE(glCompressedTexSubImage2D),
    OVERRIDE(glDrawArrays), OVERRIDE(glDrawElements), OVERRIDE(glDrawRangeElements),
    OVERRIDE(glClear), OVERRIDE(glBlitFramebuffer), OVERRIDE(glCopyTexImage2D),
    OVERRIDE(glCopyTexSubImage2D), OVERRIDE(glGenerateMipmap),
    OVERRIDE(glTexImage3D), OVERRIDE(glTexSubImage3D), OVERRIDE(glCompressedTexImage3D),
    OVERRIDE(glCompressedTexSubImage3D), OVERRIDE(glBufferData), OVERRIDE(glBufferSubData),
    OVERRIDE(glGetString), OVERRIDE(glGetStringi), OVERRIDE(glGetIntegerv), OVERRIDE(glGetFloatv),
    OVERRIDE(glGetBooleanv), OVERRIDE(glGetDoublev), OVERRIDE(glPixelStorei), OVERRIDE(glPixelStoref),
    OVERRIDE(glTexParameteri), OVERRIDE(glTexParameterf), OVERRIDE(glTexParameteriv),
    OVERRIDE(glTexParameterfv), OVERRIDE(glHint), OVERRIDE(glLightModeli), OVERRIDE(glLightModelf),
    OVERRIDE(glLightModeliv), OVERRIDE(glLightModelfv), OVERRIDE(glGenFencesAPPLE),
    OVERRIDE(glDeleteFencesAPPLE), OVERRIDE(glSetFenceAPPLE), OVERRIDE(glIsFenceAPPLE),
    OVERRIDE(glTestFenceAPPLE), OVERRIDE(glFinishFenceAPPLE), OVERRIDE(glTestObjectAPPLE),
    OVERRIDE(glFinishObjectAPPLE), OVERRIDE(glGenVertexArraysAPPLE), OVERRIDE(glBindVertexArrayAPPLE),
    OVERRIDE(glDeleteVertexArraysAPPLE), OVERRIDE(glIsVertexArrayAPPLE),
    OVERRIDE(glBufferParameteriAPPLE), OVERRIDE(glFlushMappedBufferRangeAPPLE), OVERRIDE(glMapBuffer),
    OVERRIDE(glUnmapBuffer), OVERRIDE(glElementPointerAPPLE), OVERRIDE(glDrawElementArrayAPPLE),
    OVERRIDE(glDrawRangeElementArrayAPPLE), OVERRIDE(glMultiDrawElementArrayAPPLE),
    OVERRIDE(glMultiDrawRangeElementArrayAPPLE), OVERRIDE(glFlushRenderAPPLE),
    OVERRIDE(glFinishRenderAPPLE), OVERRIDE(glSwapAPPLE), OVERRIDE(glTextureRangeAPPLE),
    OVERRIDE(glGetTexParameterPointervAPPLE), OVERRIDE(glVertexArrayRangeAPPLE),
    OVERRIDE(glFlushVertexArrayRangeAPPLE), OVERRIDE(glVertexArrayParameteriAPPLE),
    OVERRIDE(glObjectPurgeableAPPLE), OVERRIDE(glObjectUnpurgeableAPPLE),
    OVERRIDE(glGetObjectParameterivAPPLE), OVERRIDE(glInsertEventMarkerEXT),
    OVERRIDE(glPushGroupMarkerEXT), OVERRIDE(glPopGroupMarkerEXT),
    OVERRIDE(glLabelObjectWithResponsibleProcessAPPLE), OVERRIDE(glStencilOpSeparateATI),
    OVERRIDE(glStencilFuncSeparateATI), OVERRIDE(glMultiDrawElementsIndirectAPPLE),
    OVERRIDE(glFramebufferTextureFaceEXT),
};

/* Mesa exposes the ARB/core names; these are the same functions. */
static const char *const map_aliases[][2] = {
    {"glMapBufferARB", "glMapBuffer"},
    {"glUnmapBufferARB", "glUnmapBuffer"},
    {"glBufferDataARB", "glBufferData"},
    {"glBindTextureEXT", "glBindTexture"},
    {"glDrawRangeElementsEXT", "glDrawRangeElements"},
    {"glBlitFramebufferEXT", "glBlitFramebuffer"},
    {"glGenerateMipmapEXT", "glGenerateMipmap"},
    {"glBufferSubDataARB", "glBufferSubData"},
    {"glTexImage3DEXT", "glTexImage3D"},
    {"glTexSubImage3DEXT", "glTexSubImage3D"},
    {"glCompressedTexImage2DARB", "glCompressedTexImage2D"},
    {"glCompressedTexSubImage2DARB", "glCompressedTexSubImage2D"},
    {"glCompressedTexImage3DARB", "glCompressedTexImage3D"},
    {"glCompressedTexSubImage3DARB", "glCompressedTexSubImage3D"},
};

void *lp32gl_gl_proc_address(const char *name)
{
    pthread_once(&mesa_once, load_mesa);
    for (size_t i = 0; i < sizeof map_aliases / sizeof map_aliases[0]; ++i)
        if (!strcmp(name, map_aliases[i][0])) name = map_aliases[i][1];
    for (size_t i = 0; i < sizeof overrides / sizeof overrides[0]; ++i)
        if (!strcmp(name, overrides[i].name)) return overrides[i].function;
    void *function = checked_entry(name);
    if (function) return function;
    function = lp32_dri_proc_address(name);
    if (function) return function;
    for (size_t i = 0; i < sizeof unsupported_entries / sizeof unsupported_entries[0]; ++i)
        if (!strcmp(name, unsupported_entries[i])) return (void *)lp32_unsupported;
    return NULL;
}

/* ---- renderer name ------------------------------------------------------ */

static char renderer[128];
static pthread_once_t renderer_once = PTHREAD_ONCE_INIT;
extern const char *lp32gl_metal_device_name(void);
static void load_renderer(void)
{
    const char *name = lp32gl_metal_device_name();
    snprintf(renderer, sizeof renderer, "%s", name && name[0] ? name : "Apple GPU");
}
static const char *renderer_name(void)
{
    pthread_once(&renderer_once, load_renderer);
    return renderer;
}
