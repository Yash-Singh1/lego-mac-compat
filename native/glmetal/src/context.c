/* Contexts, errors, capability switches, per-fragment state and queries. */
#include "glm_internal.h"
#include "glm_apple_gets.h"
#include "arb_program.h"
#include "thread.h"
#include "queries.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glm_apple_profile.h"

static pthread_key_t current_key;
static pthread_once_t current_once = PTHREAD_ONCE_INIT;
static __thread struct glm_context *tls_current;

static void make_key(void) { pthread_key_create(&current_key, NULL); }

struct glm_context *glm_current(void) { return tls_current; }
_Static_assert(offsetof(struct glm_context, thread) == 0, "marshal_support.h reads the stream first");
_Static_assert(offsetof(struct glm_context, state_serial) == 8, "marshal_support.h reads the serial second");

void glm_set_current(struct glm_context *ctx)
{
    pthread_once(&current_once, make_key);
    tls_current = ctx;
    pthread_setspecific(current_key, ctx);
}

void glm_log(const char *format, ...)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("GLMETAL_DEBUG") != NULL;
    if (!enabled) return;
    va_list args;
    va_start(args, format);
    fputs("glmetal: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

void glm_error(struct glm_context *ctx, GLenum error)
{
    if (ctx->error == GL_NO_ERROR) ctx->error = error;
    static int debug = -1;
    if (debug < 0) debug = getenv("GLMETAL_DEBUG") != NULL;
    if (!debug) return;
    /* Name the entry point that raised it (the caller, or its caller when
       the error comes from a helper). */
    Dl_info info = {0};
    dladdr(__builtin_extract_return_addr(__builtin_return_address(0)), &info);
    glm_log("GL error 0x%04x in %s", error, info.dli_sname ? info.dli_sname : "?");
}

/* Profile is immutable. Drain first so errors from earlier queued calls keep
   their ordering and the worker cannot race the caller's error update. */
int glm_profile_rejected(struct glm_context *ctx, int core)
{
    if (!ctx || (ctx->profile == GLM_PROFILE_CORE) != core) return 0;
    glm_thread_sync(ctx);
    glm_error(ctx, GL_INVALID_OPERATION);
    return 1;
}

/* Reported once per entry point, so a game's missing feature shows up in the
   log without flooding it. */
void glm_unimplemented(const char *name)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static const char *seen[512];
    static unsigned count;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < count; ++i)
        if (seen[i] == name) {
            pthread_mutex_unlock(&lock);
            return;
        }
    if (count < 512) seen[count++] = name;
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "glmetal: unimplemented %s\n", name);
    struct glm_context *ctx = glm_current();
    if (ctx) glm_error(ctx, GL_INVALID_OPERATION);
}

/* ---- object tables ------------------------------------------------------- */

void *glm_table_get(struct glm_object_table *table, GLuint name)
{
    return name && name < table->capacity ? table->items[name] : NULL;
}

void glm_table_set(struct glm_object_table *table, GLuint name, void *item)
{
    if (!item && name >= table->capacity) return; /* nothing there to clear */
    if (name >= table->capacity) {
        GLuint capacity = table->capacity ? table->capacity : 64;
        while (capacity <= name) capacity *= 2;
        table->items = realloc(table->items, capacity * sizeof(void *));
        memset(table->items + table->capacity, 0, (capacity - table->capacity) * sizeof(void *));
        table->capacity = capacity;
    }
    table->items[name] = item;
    if (name >= table->next_name) table->next_name = name + 1;
}

/* Lowest unused name after the last one handed out, like Apple's GL. */
GLuint glm_table_reserve(struct glm_object_table *table)
{
    GLuint name = table->next_name ? table->next_name : 1;
    while (glm_table_get(table, name)) ++name;
    table->next_name = name + 1;
    return name;
}

/* ---- context lifetime ---------------------------------------------------- */

static void default_state(struct glm_context *ctx)
{
    struct glm_state *s = &ctx->state;
    memset(s, 0, sizeof *s);
    s->provoking_vertex = GL_LAST_VERTEX_CONVENTION;
    s->patch_vertices = 3;
    ctx->point_size_max = 64;
    ctx->point_fade_threshold = 1;
    ctx->point_distance[0] = 1;
    ctx->point_sprite_origin = GL_UPPER_LEFT;
    ctx->clamp_read_color = GL_FIXED_ONLY;
    s->sample_mask = ~0u;
    s->sample_coverage = 1;
    for (int i = 0; i < GLM_MAX_VIEWPORTS; ++i) s->depth_ranges[i][1] = 1;
    for (int i = 0; i < 4; ++i) s->patch_default_outer[i] = 1;
    s->patch_default_inner[0] = s->patch_default_inner[1] = 1;
    s->active_stencil_face = GL_FRONT;
    s->depth_range[1] = 1;
    s->depth_mask = true;
    s->dither = true;
    s->multisample = true;
    s->depth_func = GL_LESS;
    s->cull_mode = GL_BACK;
    s->front_face = GL_CCW;
    s->shade_model = GL_SMOOTH;
    s->polygon_mode[0] = s->polygon_mode[1] = GL_FILL;
    s->logic_op = GL_COPY;
    s->alpha_func = GL_ALWAYS;
    s->point_size = 1;
    s->line_width = 1;
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) {
        s->blend[i] = (struct glm_blend_state){false, GL_ONE, GL_ZERO, GL_ONE, GL_ZERO, GL_FUNC_ADD, GL_FUNC_ADD};
        for (int c = 0; c < 4; ++c) s->color_mask[i][c] = true;
    }
    for (int f = 0; f < 3; ++f)
        s->stencil[f] = (struct glm_stencil_face){GL_ALWAYS, GL_KEEP, GL_KEEP, GL_KEEP, 0, ~0u, ~0u};
    s->clear_depth = 1;
    for (int i = 0; i < GLM_MAX_ATTRIBS; ++i) s->current[i][3] = 1;
    s->current[GLM_ATTR_COLOR][0] = s->current[GLM_ATTR_COLOR][1] = s->current[GLM_ATTR_COLOR][2] = 1;
    s->current[GLM_ATTR_NORMAL][2] = 1;
    s->current[GLM_ATTR_SECONDARY_COLOR][3] = 1;
    s->raster_pos[3] = 1;
    s->raster_valid = true;
    s->pixel_zoom[0] = s->pixel_zoom[1] = 1;
    s->raster_color[0] = s->raster_color[1] = s->raster_color[2] = s->raster_color[3] = 1;
    for (int u = 0; u < 8; ++u) s->raster_texcoord[u][3] = 1;
    for (int i = 0; i < 4; ++i) s->pixel_scale[i] = 1;
    for (int i = 0; i < 10; ++i) s->pixel_map_size[i] = 1;
    s->depth_scale = 1;
    s->matrix_mode = GL_MODELVIEW;
    glm_mat4_identity(s->modelview.m[0]);
    glm_mat4_identity(s->projection.m[0]);
    glm_mat4_identity(s->color.m[0]);
    for (int i = 0; i < GLM_MAX_TEXTURE_UNITS; ++i) glm_mat4_identity(s->texture[i].m[0]);

    for (int i = 0; i < GLM_MAX_LIGHTS; ++i) {
        struct glm_light *l = &s->lights[i];
        l->ambient[3] = 1;
        if (i == 0) {
            l->diffuse[0] = l->diffuse[1] = l->diffuse[2] = l->diffuse[3] = 1;
            l->specular[0] = l->specular[1] = l->specular[2] = l->specular[3] = 1;
        } else {
            l->diffuse[3] = l->specular[3] = 1;
        }
        l->position[2] = 1;
        l->spot_direction[2] = -1;
        l->spot_cutoff = 180;
        l->attenuation[0] = 1;
    }
    for (int f = 0; f < 2; ++f) {
        struct glm_material *m = &s->material[f];
        m->ambient[0] = m->ambient[1] = m->ambient[2] = 0.2f;
        m->ambient[3] = 1;
        m->diffuse[0] = m->diffuse[1] = m->diffuse[2] = 0.8f;
        m->diffuse[3] = 1;
        m->specular[3] = m->emission[3] = 1;
    }
    s->light_model_ambient[0] = s->light_model_ambient[1] = s->light_model_ambient[2] = 0.2f;
    s->light_model_ambient[3] = 1;
    s->color_material_face = GL_FRONT_AND_BACK;
    s->color_material_mode = GL_AMBIENT_AND_DIFFUSE;

    s->fog_mode = GL_EXP;
    s->fog_coord_source = GL_FRAGMENT_DEPTH;
    s->fog_density = 1;
    s->fog_end = 1;

    s->active_texture = GL_TEXTURE0;
    for (int u = 0; u < GLM_MAX_TEXTURE_UNITS; ++u) {
        struct glm_texenv *e = &s->units[u].env;
        e->mode = GL_MODULATE;
        e->combine_rgb = e->combine_alpha = GL_MODULATE;
        e->source_rgb[0] = e->source_alpha[0] = GL_TEXTURE;
        e->source_rgb[1] = e->source_alpha[1] = GL_PREVIOUS;
        e->source_rgb[2] = e->source_alpha[2] = GL_CONSTANT;
        e->operand_rgb[0] = e->operand_rgb[1] = GL_SRC_COLOR;
        e->operand_rgb[2] = GL_SRC_ALPHA;
        e->operand_alpha[0] = e->operand_alpha[1] = e->operand_alpha[2] = GL_SRC_ALPHA;
        e->rgb_scale = e->alpha_scale = 1;
        for (int c = 0; c < 4; ++c) {
            s->units[u].gen[c].mode = GL_EYE_LINEAR;
            s->units[u].gen[c].object_plane[c] = s->units[u].gen[c].eye_plane[c] = c < 2 ? 1 : 0;
        }
    }
    s->unpack_alignment = s->pack_alignment = 4;
}

struct glm_context *glm_context_create(enum glm_profile profile, struct glm_context *share)
{
    if (!glm_backend_initialize()) return NULL;
    struct glm_context *ctx = calloc(1, sizeof *ctx);
    ctx->profile = profile;
    if (share) {
        ctx->share = share->share;
        pthread_mutex_lock(&ctx->share->lock);
        ++ctx->share->refcount;
        pthread_mutex_unlock(&ctx->share->lock);
    } else {
        ctx->share = calloc(1, sizeof *ctx->share);
        ctx->share->refcount = 1;
        pthread_mutex_init(&ctx->share->lock, NULL);
        ctx->share->queue = glm_backend_new_queue();
    }
    default_state(ctx);
    ctx->vao = &ctx->default_vao;
    ctx->default_vao.client_active_texture = GL_TEXTURE0;
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) ctx->default_framebuffer.draw_buffers[i] = GL_NONE;
    /* GL_NONE until a drawable is attached (cgl.m), as Apple's. */
    ctx->default_framebuffer.read_buffer = GL_NONE;
    ctx->arb_error_position = -1;
    ctx->backend = glm_backend_context_create(ctx);
    return ctx;
}

void glm_context_destroy(struct glm_context *ctx)
{
    if (!ctx) return;
    if (glm_current() == ctx) glm_set_current(NULL);
    __atomic_add_fetch(&glm_object_generation, 1, __ATOMIC_RELEASE);
    glm_backend_context_destroy(ctx->backend);
    glm_queries_destroy(ctx);
    glm_arb_context_destroy(ctx);
    for (int i = 0; i < GLM_TEX_SLOTS; ++i) glm_texture_destroy(ctx->default_textures[i]);
    pthread_mutex_lock(&ctx->share->lock);
    bool last = --ctx->share->refcount == 0;
    pthread_mutex_unlock(&ctx->share->lock);
    if (last) {
        for (GLuint i = 0; i < ctx->share->textures.capacity; ++i)
            glm_texture_destroy(ctx->share->textures.items[i]);
        free(ctx->share->textures.items);
        glm_backend_release(ctx->share->queue);
        pthread_mutex_destroy(&ctx->share->lock);
        free(ctx->share);
    }
    free(ctx);
}

/* ---- errors, flush ------------------------------------------------------- */

GLM_EXPORT GLenum glGetError(void)
{
    GLM_CONTEXT(ctx, GL_NO_ERROR);
    /* Inside glBegin/glEnd it is itself an invalid operation (recorded,
       and 0 returned). */
    if (ctx->immediate) {
        glm_error(ctx, GL_INVALID_OPERATION);
        return GL_NO_ERROR;
    }
    GLenum error = ctx->error;
    ctx->error = GL_NO_ERROR;
    return error;
}

GLM_EXPORT void glFlush(void)
{
    GLM_CONTEXT(ctx);
    glm_backend_flush(ctx, false);
}

GLM_EXPORT void glFinish(void)
{
    GLM_CONTEXT(ctx);
    glm_backend_flush(ctx, true);
}

/* ---- capabilities -------------------------------------------------------- */

static bool *capability(struct glm_context *ctx, GLenum cap, GLuint index)
{
    struct glm_state *s = &ctx->state;
    struct glm_texture_unit *unit = &s->units[s->active_texture - GL_TEXTURE0];
    switch (cap) {
    case GL_SCISSOR_TEST: return index && index < GLM_MAX_VIEWPORTS ? &s->scissor_tests[index] : &s->scissor_test;
    case GL_SAMPLE_MASK: return &s->sample_mask_enabled;
    case GL_SAMPLE_SHADING: return &s->sample_shading;
    case GL_DEPTH_TEST: return &s->depth_test;
    case GL_STENCIL_TEST: return &s->stencil_test;
    case GL_CULL_FACE: return &s->cull_face;
    case GL_ALPHA_TEST: return &s->alpha_test;
    case GL_BLEND: return &s->blend[index < GLM_MAX_DRAW_BUFFERS ? index : 0].enabled;
    case GL_DITHER: return &s->dither;
    case GL_COLOR_LOGIC_OP: return &s->color_logic_op;
    case GL_POLYGON_OFFSET_FILL: return &s->polygon_offset_fill;
    case GL_POLYGON_OFFSET_LINE: return &s->polygon_offset_line;
    case GL_POLYGON_OFFSET_POINT: return &s->polygon_offset_point;
    case GL_DEPTH_CLAMP: return &s->depth_clamp;
    case GL_RASTERIZER_DISCARD: return &s->rasterizer_discard;
    case GL_PRIMITIVE_RESTART: return &s->primitive_restart;
    case GL_PROGRAM_POINT_SIZE: return &s->program_point_size;
    case GL_MULTISAMPLE: return &s->multisample;
    case GL_SAMPLE_ALPHA_TO_COVERAGE: return &s->sample_alpha_to_coverage;
    case GL_FRAMEBUFFER_SRGB: return &s->framebuffer_srgb;
    case GL_LIGHTING: return &s->lighting;
    case GL_COLOR_MATERIAL: return &s->color_material;
    case GL_NORMALIZE: return &s->normalize;
    case GL_RESCALE_NORMAL: return &s->rescale_normal;
    case GL_FOG: return &s->fog;
    case GL_POINT_SPRITE: return &s->point_sprite;
    case GL_LINE_SMOOTH: return &s->line_smooth;
    case GL_POLYGON_SMOOTH: return &s->polygon_smooth;
    case GL_VERTEX_PROGRAM_ARB: return &s->vertex_program;
    case GL_FRAGMENT_PROGRAM_ARB: return &s->fragment_program;
    case GL_VERTEX_PROGRAM_TWO_SIDE_ARB: return &s->vertex_program_two_side;
    case GL_STENCIL_TEST_TWO_SIDE_EXT: return &s->stencil_two_side;
    case GL_TEXTURE_CUBE_MAP_SEAMLESS: return &s->texture_cube_seamless;
    case GL_TEXTURE_1D: return &unit->enabled_1d;
    case GL_TEXTURE_2D: return &unit->enabled_2d;
    case GL_TEXTURE_3D: return &unit->enabled_3d;
    case GL_TEXTURE_CUBE_MAP: return &unit->enabled_cube;
    case GL_TEXTURE_RECTANGLE_ARB: return &unit->enabled_rect;
    case GL_TEXTURE_GEN_S: return &unit->gen[0].enabled;
    case GL_TEXTURE_GEN_T: return &unit->gen[1].enabled;
    case GL_TEXTURE_GEN_R: return &unit->gen[2].enabled;
    case GL_TEXTURE_GEN_Q: return &unit->gen[3].enabled;
    default: break;
    }
    if (cap >= GL_CLIP_PLANE0 && cap < GL_CLIP_PLANE0 + GLM_MAX_CLIP_PLANES) return &s->clip_plane_enabled[cap - GL_CLIP_PLANE0];
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + GLM_MAX_LIGHTS) return &s->lights[cap - GL_LIGHT0].enabled;
    return NULL;
}

/* What Apple's implementation answers in this profile (glGet pnames and
   glIsEnabled capabilities, tools/apple_get_sweep.c): GLMetal accepts
   exactly these. */
static const struct glm_apple_get *apple_get(const struct glm_context *ctx, GLenum pname)
{
    bool core = ctx->profile == GLM_PROFILE_CORE;
    const struct glm_apple_get *t = core ? glm_apple_gets_core : glm_apple_gets_legacy;
    size_t lo = 0, hi = core ? sizeof glm_apple_gets_core / sizeof *t : sizeof glm_apple_gets_legacy / sizeof *t;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (t[mid].pname == pname) return &t[mid];
        if (t[mid].pname < pname) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static const struct glm_apple_cap *apple_cap(const struct glm_context *ctx, GLenum cap)
{
    bool core = ctx->profile == GLM_PROFILE_CORE;
    const struct glm_apple_cap *t = core ? glm_apple_caps_core : glm_apple_caps_legacy;
    size_t lo = 0, hi = core ? sizeof glm_apple_caps_core / sizeof *t : sizeof glm_apple_caps_legacy / sizeof *t;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (t[mid].cap == cap) return &t[mid];
        if (t[mid].cap < cap) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* Capabilities GLMetal does not act on (smoothing, stipples, evaluators,
   imaging): kept as state so glIsEnabled answers what glEnable set. */
static bool *extra_capability(struct glm_context *ctx, GLenum cap, bool create)
{
    for (int i = 0; i < ctx->extra_cap_count; ++i)
        if (ctx->extra_caps[i].cap == cap) return &ctx->extra_caps[i].enabled;
    if (!create || ctx->extra_cap_count == (int)(sizeof ctx->extra_caps / sizeof *ctx->extra_caps)) return NULL;
    ctx->extra_caps[ctx->extra_cap_count].cap = cap;
    ctx->extra_caps[ctx->extra_cap_count].enabled = false;
    return &ctx->extra_caps[ctx->extra_cap_count++].enabled;
}

/* Client arrays: glIsEnabled answers them; glEnable does not take them. */
static bool client_array_enabled(struct glm_context *ctx, GLenum cap, bool *enabled)
{
    switch (cap) {
    case GL_VERTEX_ARRAY: case GL_NORMAL_ARRAY: case GL_COLOR_ARRAY: case GL_SECONDARY_COLOR_ARRAY:
    case GL_FOG_COORD_ARRAY: case GL_TEXTURE_COORD_ARRAY: {
        int slot = glm_client_array_slot(ctx, cap);
        *enabled = slot >= 0 && ctx->vao->arrays[slot].enabled;
        return true;
    }
    case GL_INDEX_ARRAY: case GL_EDGE_FLAG_ARRAY: case GL_WEIGHT_ARRAY_ARB: case GL_VERTEX_ARRAY_RANGE_APPLE:
    case GL_ELEMENT_ARRAY_APPLE: case GL_POINT_SIZE_ARRAY_APPLE:
        *enabled = false;
        return true;
    }
    return false;
}

static void set_capability(struct glm_context *ctx, GLenum cap, GLuint index, bool value)
{
    /* Colour material: the material keeps the colour it last tracked. */
    if (cap == GL_COLOR_MATERIAL) glm_color_material_sync(ctx);
    const struct glm_apple_cap *known = apple_cap(ctx, cap);
    if (!known || !known->settable) {
        glm_log("glEnable/glDisable: capability 0x%04x not accepted in this profile", cap);
        return glm_error(ctx, GL_INVALID_ENUM);
    }
    /* Texture coordinate generation exists for GL_MAX_TEXTURE_COORDS (8) units. */
    if (cap >= GL_TEXTURE_GEN_S && cap <= GL_TEXTURE_GEN_Q && ctx->state.active_texture - GL_TEXTURE0 >= 8)
        return glm_error(ctx, GL_INVALID_OPERATION);
    bool *slot = capability(ctx, cap, index);
    if (!slot) slot = extra_capability(ctx, cap, true);
    if (slot) *slot = value;
}

GLM_EXPORT void glEnable(GLenum cap)
{
    GLM_CONTEXT(ctx);
    if (!apple_cap(ctx, cap)) return glm_error(ctx, GL_INVALID_ENUM);
    if (cap == GL_BLEND) {
        for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) ctx->state.blend[i].enabled = true;
        return;
    }
    if (cap == GL_SCISSOR_TEST)
        for (int i = 0; i < GLM_MAX_VIEWPORTS; ++i) ctx->state.scissor_tests[i] = true;
    set_capability(ctx, cap, 0, true);
}

GLM_EXPORT void glDisable(GLenum cap)
{
    GLM_CONTEXT(ctx);
    if (!apple_cap(ctx, cap)) return glm_error(ctx, GL_INVALID_ENUM);
    if (cap == GL_BLEND) {
        for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) ctx->state.blend[i].enabled = false;
        return;
    }
    if (cap == GL_SCISSOR_TEST)
        for (int i = 0; i < GLM_MAX_VIEWPORTS; ++i) ctx->state.scissor_tests[i] = false;
    set_capability(ctx, cap, 0, false);
}

/* The indexed capabilities, as Apple's implementation takes them: blending
   per draw buffer, scissoring per viewport (core profile only). */
static GLenum indexed_cap_error(const struct glm_context *ctx, GLenum cap, GLuint index)
{
    if (cap == GL_BLEND) return index < GLM_MAX_DRAW_BUFFERS ? GL_NO_ERROR : GL_INVALID_VALUE;
    if (cap == GL_SCISSOR_TEST && ctx->profile == GLM_PROFILE_CORE) return index < GLM_MAX_VIEWPORTS ? GL_NO_ERROR : GL_INVALID_VALUE;
    return GL_INVALID_ENUM;
}

GLM_EXPORT void glEnablei(GLenum cap, GLuint index)
{
    GLM_CONTEXT(ctx);
    GLenum error = indexed_cap_error(ctx, cap, index);
    if (error) return glm_error(ctx, error);
    set_capability(ctx, cap, index, true);
}
GLM_EXPORT void glDisablei(GLenum cap, GLuint index)
{
    GLM_CONTEXT(ctx);
    GLenum error = indexed_cap_error(ctx, cap, index);
    if (error) return glm_error(ctx, error);
    set_capability(ctx, cap, index, false);
}

GLM_EXPORT GLboolean glIsEnabled(GLenum cap)
{
    GLM_CONTEXT(ctx, GL_FALSE);
    if (!apple_cap(ctx, cap)) {
        glm_error(ctx, GL_INVALID_ENUM);
        return GL_FALSE;
    }
    bool enabled;
    if (client_array_enabled(ctx, cap, &enabled)) return enabled;
    bool *slot = capability(ctx, cap, 0);
    if (!slot) slot = extra_capability(ctx, cap, false);
    return slot && *slot;
}

GLM_EXPORT GLboolean glIsEnabledi(GLenum cap, GLuint index)
{
    GLM_CONTEXT(ctx, GL_FALSE);
    GLenum error = indexed_cap_error(ctx, cap, index);
    if (error) {
        glm_error(ctx, error);
        return GL_FALSE;
    }
    bool *slot = capability(ctx, cap, index);
    return slot ? *slot : GL_FALSE;
}

/* ---- per-fragment and rasterizer state ----------------------------------- */

/* ---- viewports (ARB_viewport_array: the plain calls set every index) ----- */

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void set_viewport(struct glm_state *s, GLuint i, float x, float y, float w, float h)
{
    w = w > 16384 ? 16384 : w;
    h = h > 16384 ? 16384 : h;
    s->viewports[i][0] = x;
    s->viewports[i][1] = y;
    s->viewports[i][2] = w;
    s->viewports[i][3] = h;
    if (i == 0) {
        s->viewport[0] = (GLint)x;
        s->viewport[1] = (GLint)y;
        s->viewport[2] = (GLint)w;
        s->viewport[3] = (GLint)h;
    }
}

static void set_scissor(struct glm_state *s, GLuint i, GLint x, GLint y, GLsizei w, GLsizei h)
{
    GLint *r = i == 0 ? s->scissor : s->scissors[i];
    r[0] = x;
    r[1] = y;
    r[2] = w;
    r[3] = h;
    if (i == 0) memcpy(s->scissors[0], s->scissor, sizeof s->scissor);
}

static void set_depth_range(struct glm_state *s, GLuint i, double n, double f)
{
    s->depth_ranges[i][0] = clamp01(n);
    s->depth_ranges[i][1] = clamp01(f);
    if (i == 0) memcpy(s->depth_range, s->depth_ranges[0], sizeof s->depth_range);
}

GLM_EXPORT void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    GLM_CONTEXT(ctx);
    if (width < 0 || height < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLuint i = 0; i < GLM_MAX_VIEWPORTS; ++i) set_viewport(&ctx->state, i, (float)x, (float)y, (float)width, (float)height);
    ctx->state.viewport[0] = x;
    ctx->state.viewport[1] = y;
    ctx->state.viewport[2] = width;
    ctx->state.viewport[3] = height;
}

GLM_EXPORT void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    GLM_CONTEXT(ctx);
    if (width < 0 || height < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLuint i = 0; i < GLM_MAX_VIEWPORTS; ++i) set_scissor(&ctx->state, i, x, y, width, height);
}

GLM_EXPORT void glDepthRange(GLclampd n, GLclampd f)
{
    GLM_CONTEXT(ctx);
    for (GLuint i = 0; i < GLM_MAX_VIEWPORTS; ++i) set_depth_range(&ctx->state, i, n, f);
}
GLM_EXPORT void glDepthRangef(GLclampf n, GLclampf f) { glDepthRange(n, f); }

GLM_EXPORT void glViewportArrayv(GLuint first, GLsizei count, const GLfloat *v)
{
    GLM_CONTEXT(ctx);
    if (count < 0 || first + (GLuint)count > GLM_MAX_VIEWPORTS) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i) {
        const GLfloat *r = v + i * 4;
        if (r[2] < 0 || r[3] < 0) return glm_error(ctx, GL_INVALID_VALUE);
        set_viewport(&ctx->state, first + (GLuint)i, r[0], r[1], r[2], r[3]);
    }
}
GLM_EXPORT void glViewportIndexedf(GLuint index, GLfloat x, GLfloat y, GLfloat w, GLfloat h)
{
    const GLfloat v[4] = {x, y, w, h};
    glViewportArrayv(index, 1, v);
}
GLM_EXPORT void glViewportIndexedfv(GLuint index, const GLfloat *v) { glViewportArrayv(index, 1, v); }

GLM_EXPORT void glScissorArrayv(GLuint first, GLsizei count, const GLint *v)
{
    GLM_CONTEXT(ctx);
    if (count < 0 || first + (GLuint)count > GLM_MAX_VIEWPORTS) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i) {
        const GLint *r = v + i * 4;
        if (r[2] < 0 || r[3] < 0) return glm_error(ctx, GL_INVALID_VALUE);
        set_scissor(&ctx->state, first + (GLuint)i, r[0], r[1], r[2], r[3]);
    }
}
GLM_EXPORT void glScissorIndexed(GLuint index, GLint x, GLint y, GLsizei w, GLsizei h)
{
    const GLint v[4] = {x, y, w, h};
    glScissorArrayv(index, 1, v);
}
GLM_EXPORT void glScissorIndexedv(GLuint index, const GLint *v) { glScissorArrayv(index, 1, v); }

GLM_EXPORT void glDepthRangeArrayv(GLuint first, GLsizei count, const GLclampd *v)
{
    GLM_CONTEXT(ctx);
    if (count < 0 || first + (GLuint)count > GLM_MAX_VIEWPORTS) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i) set_depth_range(&ctx->state, first + (GLuint)i, v[i * 2], v[i * 2 + 1]);
}
GLM_EXPORT void glDepthRangeIndexed(GLuint index, GLclampd n, GLclampd f)
{
    const GLclampd v[2] = {n, f};
    glDepthRangeArrayv(index, 1, v);
}

int glm_indexed_state_query(struct glm_context *ctx, GLenum pname, GLuint index, double *v)
{
    const struct glm_state *s = &ctx->state;
    switch (pname) {
    case GL_VIEWPORT: case GL_SCISSOR_BOX: case GL_DEPTH_RANGE: case GL_SCISSOR_TEST: break;
    case GL_SAMPLE_MASK_VALUE: if (index >= 1) return -1; v[0] = s->sample_mask; return 1;
    default: return 0;
    }
    if (index >= GLM_MAX_VIEWPORTS) return -1;
    switch (pname) {
    case GL_VIEWPORT: for (int i = 0; i < 4; ++i) v[i] = s->viewports[index][i]; return 4;
    case GL_SCISSOR_BOX: for (int i = 0; i < 4; ++i) v[i] = index ? s->scissors[index][i] : s->scissor[i]; return 4;
    case GL_DEPTH_RANGE: v[0] = s->depth_ranges[index][0]; v[1] = s->depth_ranges[index][1]; return 2;
    default: v[0] = index ? s->scissor_tests[index] : s->scissor_test; return 1;
    }
}

GLM_EXPORT void glDepthFunc(GLenum func) { GLM_CONTEXT(ctx); if (func < GL_NEVER || func > GL_ALWAYS) return glm_error(ctx, GL_INVALID_ENUM); ctx->state.depth_func = func; }
GLM_EXPORT void glDepthMask(GLboolean flag) { GLM_CONTEXT(ctx); ctx->state.depth_mask = flag; }
GLM_EXPORT void glCullFace(GLenum mode) { GLM_CONTEXT(ctx); if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK) return glm_error(ctx, GL_INVALID_ENUM); ctx->state.cull_mode = mode; }
GLM_EXPORT void glFrontFace(GLenum mode) { GLM_CONTEXT(ctx); if (mode != GL_CW && mode != GL_CCW) return glm_error(ctx, GL_INVALID_ENUM); ctx->state.front_face = mode; }
GLM_EXPORT void glShadeModel(GLenum mode) { GLM_CONTEXT(ctx); if (mode != GL_FLAT && mode != GL_SMOOTH) return glm_error(ctx, GL_INVALID_ENUM); ctx->state.shade_model = mode; }
GLM_EXPORT void glLogicOp(GLenum op) { GLM_CONTEXT(ctx); if (op < GL_CLEAR || op > GL_SET) return glm_error(ctx, GL_INVALID_ENUM); ctx->state.logic_op = op; }
GLM_EXPORT void glPointSize(GLfloat size) { GLM_CONTEXT(ctx); if (size <= 0) return glm_error(ctx, GL_INVALID_VALUE); ctx->state.point_size = size; }
GLM_EXPORT void glLineWidth(GLfloat width) { GLM_CONTEXT(ctx); if (width <= 0) return glm_error(ctx, GL_INVALID_VALUE); ctx->state.line_width = width; }
GLM_EXPORT void glPrimitiveRestartIndex(GLuint index) { GLM_CONTEXT(ctx); ctx->state.primitive_restart_index = index; }

GLM_EXPORT void glAlphaFunc(GLenum func, GLclampf ref)
{
    GLM_CONTEXT(ctx);
    if (func < GL_NEVER || func > GL_ALWAYS) return glm_error(ctx, GL_INVALID_ENUM);
    ctx->state.alpha_func = func;
    ctx->state.alpha_ref = ref < 0 ? 0 : ref > 1 ? 1 : ref;
}

GLM_EXPORT void glPolygonMode(GLenum face, GLenum mode)
{
    GLM_CONTEXT(ctx);
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) return glm_error(ctx, GL_INVALID_ENUM);
    if (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL) return glm_error(ctx, GL_INVALID_ENUM);
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) ctx->state.polygon_mode[0] = mode;
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) ctx->state.polygon_mode[1] = mode;
}

GLM_EXPORT void glPolygonOffset(GLfloat factor, GLfloat units)
{
    GLM_CONTEXT(ctx);
    ctx->state.polygon_offset_factor = factor;
    ctx->state.polygon_offset_units = units;
}

GLM_EXPORT void glBlendFunc(GLenum src, GLenum dst) { glBlendFuncSeparate(src, dst, src, dst); }

GLM_EXPORT void glBlendFuncSeparate(GLenum src_rgb, GLenum dst_rgb, GLenum src_alpha, GLenum dst_alpha)
{
    GLM_CONTEXT(ctx);
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) {
        struct glm_blend_state *b = &ctx->state.blend[i];
        b->src_rgb = src_rgb;
        b->dst_rgb = dst_rgb;
        b->src_alpha = src_alpha;
        b->dst_alpha = dst_alpha;
    }
}

GLM_EXPORT void glBlendFuncSeparatei(GLuint buf, GLenum src_rgb, GLenum dst_rgb, GLenum src_alpha, GLenum dst_alpha)
{
    GLM_CONTEXT(ctx);
    if (buf >= GLM_MAX_DRAW_BUFFERS) return glm_error(ctx, GL_INVALID_VALUE);
    struct glm_blend_state *b = &ctx->state.blend[buf];
    b->src_rgb = src_rgb;
    b->dst_rgb = dst_rgb;
    b->src_alpha = src_alpha;
    b->dst_alpha = dst_alpha;
}

GLM_EXPORT void glBlendFunci(GLuint buf, GLenum src, GLenum dst) { glBlendFuncSeparatei(buf, src, dst, src, dst); }

static bool valid_blend_equation(GLenum mode)
{
    return mode == GL_FUNC_ADD || mode == GL_FUNC_SUBTRACT || mode == GL_FUNC_REVERSE_SUBTRACT ||
           mode == GL_MIN || mode == GL_MAX;
}

GLM_EXPORT void glBlendEquation(GLenum mode) { glBlendEquationSeparate(mode, mode); }

GLM_EXPORT void glBlendEquationSeparate(GLenum rgb, GLenum alpha)
{
    GLM_CONTEXT(ctx);
    if (!valid_blend_equation(rgb) || !valid_blend_equation(alpha)) return glm_error(ctx, GL_INVALID_ENUM);
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) {
        ctx->state.blend[i].eq_rgb = rgb;
        ctx->state.blend[i].eq_alpha = alpha;
    }
}

GLM_EXPORT void glBlendEquationSeparatei(GLuint buf, GLenum rgb, GLenum alpha)
{
    GLM_CONTEXT(ctx);
    if (!valid_blend_equation(rgb) || !valid_blend_equation(alpha)) return glm_error(ctx, GL_INVALID_ENUM);
    if (buf >= GLM_MAX_DRAW_BUFFERS) return glm_error(ctx, GL_INVALID_VALUE);
    ctx->state.blend[buf].eq_rgb = rgb;
    ctx->state.blend[buf].eq_alpha = alpha;
}

GLM_EXPORT void glBlendEquationi(GLuint buf, GLenum mode) { glBlendEquationSeparatei(buf, mode, mode); }

GLM_EXPORT void glBlendColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    GLM_CONTEXT(ctx);
    float *c = ctx->state.blend_color;
    c[0] = r; c[1] = g; c[2] = b; c[3] = a;
}

GLM_EXPORT void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    GLM_CONTEXT(ctx);
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) {
        bool *m = ctx->state.color_mask[i];
        m[0] = r; m[1] = g; m[2] = b; m[3] = a;
    }
}

GLM_EXPORT void glColorMaski(GLuint buf, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    GLM_CONTEXT(ctx);
    if (buf >= GLM_MAX_DRAW_BUFFERS) return glm_error(ctx, GL_INVALID_VALUE);
    bool *m = ctx->state.color_mask[buf];
    m[0] = r; m[1] = g; m[2] = b; m[3] = a;
}

/* Internal face name: EXT_stencil_two_side's back state. */
enum { GLM_EXT_BACK = 0x7fff0001 };

/* The stencil states (indices into state.stencil) a face name sets. */
static int stencil_face_indices(GLenum face, int *index)
{
    switch (face) {
    case GL_FRONT: index[0] = 0; return 1;
    case GL_BACK: index[0] = 1; return 1;
    case GL_FRONT_AND_BACK: index[0] = 0; index[1] = 1; return 2;
    case GLM_EXT_BACK: index[0] = 2; return 1;
    default: return 0;
    }
}

GLM_EXPORT void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask)
{
    GLM_CONTEXT(ctx);
    if (func < GL_NEVER || func > GL_ALWAYS) return glm_error(ctx, GL_INVALID_ENUM);
    int index[2], n = stencil_face_indices(face, index);
    if (!n) return glm_error(ctx, GL_INVALID_ENUM);
    for (int k = 0; k < n; ++k) {
        ctx->state.stencil[index[k]].func = func;
        ctx->state.stencil[index[k]].ref = ref;
        ctx->state.stencil[index[k]].value_mask = mask;
    }
}
/* EXT_stencil_two_side: with GL_BACK active, the one-sided calls set only
   the back state; otherwise both, as in GL 2.0. */
static GLenum one_sided_face(struct glm_context *ctx)
{
    return ctx->state.active_stencil_face == GL_BACK ? GLM_EXT_BACK : GL_FRONT_AND_BACK;
}

GLM_EXPORT void glActiveStencilFaceEXT(GLenum face)
{
    GLM_CONTEXT(ctx);
    if (face != GL_FRONT && face != GL_BACK) return glm_error(ctx, GL_INVALID_ENUM);
    ctx->state.active_stencil_face = face;
}

GLM_EXPORT void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
    GLM_CONTEXT(ctx);
    glStencilFuncSeparate(one_sided_face(ctx), func, ref, mask);
}

GLM_EXPORT void glStencilOpSeparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass)
{
    GLM_CONTEXT(ctx);
    int index[2], n = stencil_face_indices(face, index);
    if (!n) return glm_error(ctx, GL_INVALID_ENUM);
    for (int k = 0; k < n; ++k) {
        ctx->state.stencil[index[k]].fail = fail;
        ctx->state.stencil[index[k]].zfail = zfail;
        ctx->state.stencil[index[k]].zpass = zpass;
    }
}
GLM_EXPORT void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
    GLM_CONTEXT(ctx);
    glStencilOpSeparate(one_sided_face(ctx), fail, zfail, zpass);
}

GLM_EXPORT void glStencilMaskSeparate(GLenum face, GLuint mask)
{
    GLM_CONTEXT(ctx);
    int index[2], n = stencil_face_indices(face, index);
    if (!n) return glm_error(ctx, GL_INVALID_ENUM);
    for (int k = 0; k < n; ++k) ctx->state.stencil[index[k]].write_mask = mask;
}
GLM_EXPORT void glStencilMask(GLuint mask)
{
    GLM_CONTEXT(ctx);
    glStencilMaskSeparate(one_sided_face(ctx), mask);
}

/* ATI names Apple still exports. */
GLM_EXPORT void glStencilOpSeparateATI(GLenum face, GLenum fail, GLenum zfail, GLenum zpass) { glStencilOpSeparate(face, fail, zfail, zpass); }
GLM_EXPORT void glStencilFuncSeparateATI(GLenum front, GLenum back, GLint ref, GLuint mask)
{
    GLM_CONTEXT(ctx);
    if (front < GL_NEVER || front > GL_ALWAYS || back < GL_NEVER || back > GL_ALWAYS)
        return glm_error(ctx, GL_INVALID_ENUM);
    ctx->state.stencil[0].func = front;
    ctx->state.stencil[1].func = back;
    for (int i = 0; i < 2; ++i) {
        ctx->state.stencil[i].ref = ref;
        ctx->state.stencil[i].value_mask = mask;
    }
}

GLM_EXPORT void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    GLM_CONTEXT(ctx);
    float *c = ctx->state.clear_color;
    c[0] = r; c[1] = g; c[2] = b; c[3] = a;
}
GLM_EXPORT void glClearDepth(GLclampd depth) { GLM_CONTEXT(ctx); ctx->state.clear_depth = clamp01(depth); }
GLM_EXPORT void glClearDepthf(GLclampf depth) { glClearDepth(depth); }
GLM_EXPORT void glClearStencil(GLint s) { GLM_CONTEXT(ctx); ctx->state.clear_stencil = s; }

GLM_EXPORT void glClear(GLbitfield mask)
{
    GLM_CONTEXT(ctx);
    if (mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT))
        return glm_error(ctx, GL_INVALID_VALUE);
    if (ctx->immediate) return glm_error(ctx, GL_INVALID_OPERATION);
    if (ctx->conditional_discard) return;
    glm_backend_clear(ctx, mask);
}

/* Depth and stencil through glClear with the clear values swapped in. */
static void clear_depth_stencil(struct glm_context *ctx, GLbitfield mask, double depth, GLint stencil)
{
    struct glm_state *s = &ctx->state;
    double saved_depth = s->clear_depth;
    GLint saved_stencil = s->clear_stencil;
    s->clear_depth = clamp01(depth);
    s->clear_stencil = stencil;
    glm_backend_clear(ctx, mask);
    s->clear_depth = saved_depth;
    s->clear_stencil = saved_stencil;
}

static void clear_buffer(GLenum buffer, GLint drawbuffer, int kind, const void *value)
{
    GLM_CONTEXT(ctx);
    if (ctx->immediate) return glm_error(ctx, GL_INVALID_OPERATION);
    if (ctx->state.rasterizer_discard || ctx->conditional_discard) return;
    switch (buffer) {
    case GL_COLOR:
        if (drawbuffer < 0 || drawbuffer >= GLM_MAX_DRAW_BUFFERS) return glm_error(ctx, GL_INVALID_VALUE);
        glm_backend_clear_buffer(ctx, drawbuffer, kind, value);
        break;
    case GL_DEPTH:
        if (kind != 0 || drawbuffer != 0) return glm_error(ctx, drawbuffer ? GL_INVALID_VALUE : GL_INVALID_ENUM);
        clear_depth_stencil(ctx, GL_DEPTH_BUFFER_BIT, *(const GLfloat *)value, ctx->state.clear_stencil);
        break;
    case GL_STENCIL:
        if (kind != 1 || drawbuffer != 0) return glm_error(ctx, drawbuffer ? GL_INVALID_VALUE : GL_INVALID_ENUM);
        clear_depth_stencil(ctx, GL_STENCIL_BUFFER_BIT, ctx->state.clear_depth, *(const GLint *)value);
        break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}

GLM_EXPORT void glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat *value) { clear_buffer(buffer, drawbuffer, 0, value); }
GLM_EXPORT void glClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint *value) { clear_buffer(buffer, drawbuffer, 1, value); }
GLM_EXPORT void glClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint *value)
{
    if (buffer != GL_COLOR) {
        GLM_CONTEXT(ctx);
        return glm_error(ctx, GL_INVALID_ENUM);
    }
    clear_buffer(buffer, drawbuffer, 2, value);
}
GLM_EXPORT void glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil)
{
    GLM_CONTEXT(ctx);
    if (buffer != GL_DEPTH_STENCIL) return glm_error(ctx, GL_INVALID_ENUM);
    if (drawbuffer != 0) return glm_error(ctx, GL_INVALID_VALUE);
    if (ctx->state.rasterizer_discard) return;
    clear_depth_stencil(ctx, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, depth, stencil);
}

GLM_EXPORT void glSampleMaski(GLuint index, GLbitfield mask)
{
    GLM_CONTEXT(ctx);
    if (index >= 1) return glm_error(ctx, GL_INVALID_VALUE);
    ctx->state.sample_mask = mask;
}

GLM_EXPORT void glSampleCoverage(GLfloat value, GLboolean invert)
{
    GLM_CONTEXT(ctx);
    ctx->state.sample_coverage = (float)clamp01(value);
    ctx->state.sample_coverage_invert = invert;
}
GLM_EXPORT void glSampleCoverageARB(GLfloat value, GLboolean invert) { glSampleCoverage(value, invert); }

GLM_EXPORT void glMinSampleShading(GLfloat value) { GLM_CONTEXT(ctx); ctx->state.min_sample_shading = (float)clamp01(value); }
GLM_EXPORT void glMinSampleShadingARB(GLfloat value) { glMinSampleShading(value); }

static int hint_index(GLenum target)
{
    switch (target) {
    case GL_PERSPECTIVE_CORRECTION_HINT: return 0;
    case GL_POINT_SMOOTH_HINT: return 1;
    case GL_LINE_SMOOTH_HINT: return 2;
    case GL_POLYGON_SMOOTH_HINT: return 3;
    case GL_FOG_HINT: return 4;
    case GL_GENERATE_MIPMAP_HINT: return 5;
    case GL_TEXTURE_COMPRESSION_HINT: return 6;
    case GL_FRAGMENT_SHADER_DERIVATIVE_HINT: return 7;
    case 0x80F0 /* GL_CLIP_VOLUME_CLIPPING_HINT_EXT */: return 8;
    case 0x8534 /* GL_MULTISAMPLE_FILTER_HINT_NV */: return 9;
    case 0x85B1 /* GL_TRANSFORM_HINT_APPLE */: return 10;
    default: return -1;
    }
}

/* Hints are state only (Metal has no use for them). */
GLM_EXPORT void glHint(GLenum target, GLenum mode)
{
    GLM_CONTEXT(ctx);
    int i = hint_index(target);
    if (i < 0 || !apple_get(ctx, target)) return glm_error(ctx, GL_INVALID_ENUM);
    if (mode != GL_FASTEST && mode != GL_NICEST && mode != GL_DONT_CARE) return glm_error(ctx, GL_INVALID_ENUM);
    ctx->hints[i] = mode;
}


/* ---- strings ------------------------------------------------------------- */

static const char *extension_string(struct glm_context *ctx)
{
    static char *strings[2];
    int profile = ctx->profile == GLM_PROFILE_CORE;
    if (!strings[profile]) {
        const char *const *list = profile ? glm_core_extensions : glm_legacy_extensions;
        size_t length = 1;
        for (int i = 0; list[i]; ++i) length += strlen(list[i]) + 1;
        char *text = calloc(1, length);
        for (int i = 0; list[i]; ++i) {
            strcat(text, list[i]);
            strcat(text, " ");
        }
        strings[profile] = text;
    }
    return strings[profile];
}

GLM_HIDDEN const char *glm_renderer_name(void);

GLM_EXPORT const GLubyte *glGetString(GLenum name)
{
    GLM_CONTEXT(ctx, NULL);
    bool core = ctx->profile == GLM_PROFILE_CORE;
    switch (name) {
    case GL_VENDOR: return (const GLubyte *)"Apple";
    case GL_RENDERER: return (const GLubyte *)glm_renderer_name();
    case GL_VERSION: return (const GLubyte *)(core ? glm_core_version : glm_legacy_version);
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)(core ? glm_core_glsl : glm_legacy_glsl);
    case GL_EXTENSIONS:
        if (core) break;
        return (const GLubyte *)extension_string(ctx);
    case GL_PROGRAM_ERROR_STRING_ARB:
        if (core) break;
        return (const GLubyte *)glm_arb_error_string(ctx);
    }
    glm_error(ctx, GL_INVALID_ENUM);
    return NULL;
}

GLM_EXPORT const GLubyte *glGetStringi(GLenum name, GLuint index)
{
    GLM_CONTEXT(ctx, NULL);
    const char *const *list = ctx->profile == GLM_PROFILE_CORE ? glm_core_extensions : glm_legacy_extensions;
    if (name != GL_EXTENSIONS) return glm_error(ctx, GL_INVALID_ENUM), NULL;
    for (GLuint i = 0; list[i]; ++i)
        if (i == index) return (const GLubyte *)list[i];
    glm_error(ctx, GL_INVALID_VALUE);
    return NULL;
}

/* ---- queries ------------------------------------------------------------- */

/* Fills up to 16 values for `pname`; returns the count, 0 if unknown. All
   typed getters convert from these doubles. */
static int query(struct glm_context *ctx, GLenum pname, double *v, bool *is_color)
{
    struct glm_state *s = &ctx->state;
    const struct glm_limit *limits = ctx->profile == GLM_PROFILE_CORE ? glm_core_limits : glm_legacy_limits;
    *is_color = false;
    for (int i = 0; limits[i].pname; ++i)
        if (limits[i].pname == pname) {
            for (int k = 0; k < limits[i].count; ++k) v[k] = limits[i].values[k];
            return limits[i].count;
        }
    bool *cap = capability(ctx, pname, 0);
    if (cap) {
        v[0] = *cap;
        return 1;
    }
    switch (pname) {
    case GL_VIEWPORT: for (int i = 0; i < 4; ++i) v[i] = s->viewport[i]; return 4;
    case GL_AUX_BUFFERS: if (ctx->profile == GLM_PROFILE_CORE) return 0; v[0] = 0; return 1;
    case GL_PACK_SWAP_BYTES: v[0] = s->pack_swap_bytes; return 1;
    case GL_UNPACK_SWAP_BYTES: case GL_UNPACK_LSB_FIRST: case GL_PACK_LSB_FIRST: v[0] = 0; return 1;
    case GL_PROVOKING_VERTEX: v[0] = s->provoking_vertex; return 1;
    case GL_PATCH_VERTICES: v[0] = s->patch_vertices; return 1;
    case GL_PATCH_DEFAULT_OUTER_LEVEL: for (int i = 0; i < 4; ++i) v[i] = s->patch_default_outer[i]; return 4;
    case GL_PATCH_DEFAULT_INNER_LEVEL: v[0] = s->patch_default_inner[0]; v[1] = s->patch_default_inner[1]; return 2;
    case GL_MAX_PATCH_VERTICES: v[0] = GLM_MAX_PATCH_VERTICES; return 1;
    case GL_MAX_TESS_GEN_LEVEL: v[0] = 64; return 1;
    case GL_ACTIVE_STENCIL_FACE_EXT: if (ctx->profile == GLM_PROFILE_CORE) return 0; v[0] = s->active_stencil_face; return 1;
    case GL_QUADS_FOLLOW_PROVOKING_VERTEX_CONVENTION: v[0] = 1; return 1;
    case GL_SCISSOR_BOX: for (int i = 0; i < 4; ++i) v[i] = s->scissor[i]; return 4;
    case GL_DEPTH_RANGE: v[0] = s->depth_range[0]; v[1] = s->depth_range[1]; return 2;
    case GL_COLOR_CLEAR_VALUE: *is_color = true; for (int i = 0; i < 4; ++i) v[i] = s->clear_color[i]; return 4;
    case GL_BLEND_COLOR: *is_color = true; for (int i = 0; i < 4; ++i) v[i] = s->blend_color[i]; return 4;
    case GL_CURRENT_COLOR: *is_color = true; for (int i = 0; i < 4; ++i) v[i] = s->current[GLM_ATTR_COLOR][i]; return 4;
    case GL_CURRENT_NORMAL: for (int i = 0; i < 3; ++i) v[i] = s->current[GLM_ATTR_NORMAL][i]; return 3;
    case GL_CURRENT_TEXTURE_COORDS:
        for (int i = 0; i < 4; ++i) v[i] = s->current[GLM_ATTR_TEX0 + (s->active_texture - GL_TEXTURE0) % 8][i];
        return 4;
    case GL_DEPTH_CLEAR_VALUE: v[0] = s->clear_depth; return 1;
    case GL_STENCIL_CLEAR_VALUE: v[0] = s->clear_stencil; return 1;
    case GL_DEPTH_FUNC: v[0] = s->depth_func; return 1;
    case GL_DEPTH_WRITEMASK: v[0] = s->depth_mask; return 1;
    case GL_COLOR_WRITEMASK: for (int i = 0; i < 4; ++i) v[i] = s->color_mask[0][i]; return 4;
    case GL_CULL_FACE_MODE: v[0] = s->cull_mode; return 1;
    case GL_FRONT_FACE: v[0] = s->front_face; return 1;
    case GL_SHADE_MODEL: v[0] = s->shade_model; return 1;
    case GL_POLYGON_MODE: v[0] = s->polygon_mode[0]; v[1] = s->polygon_mode[1]; return 2;
    case GL_POLYGON_OFFSET_FACTOR: v[0] = s->polygon_offset_factor; return 1;
    case GL_POLYGON_OFFSET_UNITS: v[0] = s->polygon_offset_units; return 1;
    case GL_ALPHA_TEST_FUNC: v[0] = s->alpha_func; return 1;
    case GL_ALPHA_TEST_REF: v[0] = s->alpha_ref; return 1;
    case GL_LOGIC_OP_MODE: v[0] = s->logic_op; return 1;
    case GL_BLEND_SRC_RGB: v[0] = s->blend[0].src_rgb; return 1;
    case GL_BLEND_DST_RGB: v[0] = s->blend[0].dst_rgb; return 1;
    case GL_BLEND_SRC_ALPHA: v[0] = s->blend[0].src_alpha; return 1;
    case GL_BLEND_DST_ALPHA: v[0] = s->blend[0].dst_alpha; return 1;
    case GL_BLEND_EQUATION_RGB: v[0] = s->blend[0].eq_rgb; return 1;
    case GL_BLEND_EQUATION_ALPHA: v[0] = s->blend[0].eq_alpha; return 1;
    /* EXT_stencil_two_side: the one-sided names report the active face. */
    case GL_STENCIL_FUNC: v[0] = s->stencil[s->active_stencil_face == GL_BACK ? 2 : 0].func; return 1;
    case GL_STENCIL_REF: v[0] = s->stencil[s->active_stencil_face == GL_BACK ? 2 : 0].ref; return 1;
    case GL_STENCIL_VALUE_MASK: v[0] = s->stencil[s->active_stencil_face == GL_BACK ? 2 : 0].value_mask; return 1;
    case GL_STENCIL_WRITEMASK: v[0] = s->stencil[s->active_stencil_face == GL_BACK ? 2 : 0].write_mask; return 1;
    case GL_STENCIL_FAIL: v[0] = s->stencil[s->active_stencil_face == GL_BACK ? 2 : 0].fail; return 1;
    case GL_STENCIL_PASS_DEPTH_FAIL: v[0] = s->stencil[s->active_stencil_face == GL_BACK ? 2 : 0].zfail; return 1;
    case GL_STENCIL_PASS_DEPTH_PASS: v[0] = s->stencil[s->active_stencil_face == GL_BACK ? 2 : 0].zpass; return 1;
    case GL_STENCIL_BACK_FUNC: v[0] = s->stencil[1].func; return 1;
    case GL_STENCIL_BACK_REF: v[0] = s->stencil[1].ref; return 1;
    case GL_STENCIL_BACK_VALUE_MASK: v[0] = s->stencil[1].value_mask; return 1;
    case GL_STENCIL_BACK_WRITEMASK: v[0] = s->stencil[1].write_mask; return 1;
    case GL_STENCIL_BACK_FAIL: v[0] = s->stencil[1].fail; return 1;
    case GL_STENCIL_BACK_PASS_DEPTH_FAIL: v[0] = s->stencil[1].zfail; return 1;
    case GL_STENCIL_BACK_PASS_DEPTH_PASS: v[0] = s->stencil[1].zpass; return 1;
    case GL_POINT_SIZE: v[0] = s->point_size; return 1;
    case GL_LINE_WIDTH: v[0] = s->line_width; return 1;
    case GL_POINT_SIZE_RANGE: case GL_ALIASED_POINT_SIZE_RANGE: v[0] = 1; v[1] = 511; return 2;
    case GL_LINE_WIDTH_RANGE: case GL_ALIASED_LINE_WIDTH_RANGE: v[0] = 1; v[1] = ctx->profile == GLM_PROFILE_CORE ? 1 : 16; return 2;
    case GL_MATRIX_MODE: v[0] = s->matrix_mode; return 1;
    case GL_MODELVIEW_MATRIX: for (int i = 0; i < 16; ++i) v[i] = s->modelview.m[s->modelview.depth][i]; return 16;
    case GL_PROJECTION_MATRIX: for (int i = 0; i < 16; ++i) v[i] = s->projection.m[s->projection.depth][i]; return 16;
    case GL_TEXTURE_MATRIX: {
        struct glm_matrix_stack *t = &s->texture[s->active_texture - GL_TEXTURE0];
        for (int i = 0; i < 16; ++i) v[i] = t->m[t->depth][i];
        return 16;
    }
    case GL_MODELVIEW_STACK_DEPTH: v[0] = s->modelview.depth + 1; return 1;
    case GL_PROJECTION_STACK_DEPTH: v[0] = s->projection.depth + 1; return 1;
    case GL_TEXTURE_STACK_DEPTH: v[0] = s->texture[s->active_texture - GL_TEXTURE0].depth + 1; return 1;
    case GL_MAX_MODELVIEW_STACK_DEPTH: case GL_MAX_PROJECTION_STACK_DEPTH: case GL_MAX_TEXTURE_STACK_DEPTH:
        v[0] = GLM_MATRIX_STACK_DEPTH; return 1;
    case GL_ACTIVE_TEXTURE: v[0] = s->active_texture; return 1;
    case GL_CLIENT_ACTIVE_TEXTURE: v[0] = ctx->vao->client_active_texture; return 1;
    case GL_MAX_TEXTURE_UNITS: v[0] = 8; return 1;
    case GL_MAX_TEXTURE_COORDS: v[0] = 8; return 1;
    case GL_MAX_LIGHTS: v[0] = GLM_MAX_LIGHTS; return 1;
    case GL_MAX_CLIP_PLANES: v[0] = GLM_MAX_CLIP_PLANES; return 1;
    case GL_MAX_VIEWPORT_DIMS: v[0] = v[1] = 16384; return 2;
    case GL_SUBPIXEL_BITS: v[0] = 8; return 1;
    case GL_TEXTURE_BINDING_1D: case GL_TEXTURE_BINDING_2D: case GL_TEXTURE_BINDING_3D: case GL_TEXTURE_BINDING_CUBE_MAP:
    case GL_TEXTURE_BINDING_RECTANGLE_ARB: case GL_TEXTURE_BINDING_2D_ARRAY: {
        static const GLenum targets[] = {GL_TEXTURE_BINDING_1D, GL_TEXTURE_BINDING_2D, GL_TEXTURE_BINDING_3D,
                                         GL_TEXTURE_BINDING_CUBE_MAP, GL_TEXTURE_BINDING_RECTANGLE_ARB, GL_TEXTURE_BINDING_2D_ARRAY};
        int slot = 0;
        while (targets[slot] != pname) ++slot;
        v[0] = s->units[s->active_texture - GL_TEXTURE0].bound[slot];
        return 1;
    }
    case GL_ARRAY_BUFFER_BINDING: v[0] = ctx->array_buffer; return 1;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: v[0] = ctx->vao->element_buffer; return 1;
    case GL_PIXEL_PACK_BUFFER_BINDING: v[0] = ctx->pixel_pack_buffer; return 1;
    case GL_PIXEL_UNPACK_BUFFER_BINDING: v[0] = ctx->pixel_unpack_buffer; return 1;
    case GL_UNIFORM_BUFFER_BINDING: v[0] = ctx->uniform_buffer; return 1;
    case GL_DRAW_INDIRECT_BUFFER_BINDING: v[0] = ctx->draw_indirect_buffer; return 1;
    case GL_VERTEX_ARRAY_BINDING: v[0] = ctx->vao == &ctx->default_vao ? 0 : ctx->vao->name; return 1;
    case GL_CURRENT_PROGRAM: v[0] = ctx->current_program; return 1;
    case GL_DRAW_FRAMEBUFFER_BINDING: v[0] = ctx->draw_framebuffer; return 1;
    case GL_READ_FRAMEBUFFER_BINDING: v[0] = ctx->read_framebuffer; return 1;
    case GL_RENDERBUFFER_BINDING: v[0] = ctx->renderbuffer_binding; return 1;
    case GL_DRAW_BUFFER: case GL_DRAW_BUFFER0: {
        struct glm_framebuffer *fb = glm_framebuffer_get(ctx, ctx->draw_framebuffer);
        v[0] = fb ? fb->draw_buffers[0] : GL_NONE;
        return 1;
    }
    case GL_READ_BUFFER: {
        struct glm_framebuffer *fb = glm_framebuffer_get(ctx, ctx->read_framebuffer);
        v[0] = fb ? fb->read_buffer : GL_NONE;
        return 1;
    }
    case GL_UNPACK_ALIGNMENT: v[0] = s->unpack_alignment; return 1;
    case GL_UNPACK_ROW_BYTES_APPLE: v[0] = s->unpack_row_bytes; return 1;
    case GL_UNPACK_IMAGE_BYTES_APPLE: v[0] = s->unpack_image_bytes; return 1;
    case GL_PACK_ROW_BYTES_APPLE: v[0] = s->pack_row_bytes; return 1;
    case GL_PACK_IMAGE_BYTES_APPLE: v[0] = s->pack_image_bytes; return 1;
    case GL_PACK_ALIGNMENT: v[0] = s->pack_alignment; return 1;
    case GL_UNPACK_ROW_LENGTH: v[0] = s->unpack_row_length; return 1;
    case GL_UNPACK_SKIP_ROWS: v[0] = s->unpack_skip_rows; return 1;
    case GL_UNPACK_SKIP_PIXELS: v[0] = s->unpack_skip_pixels; return 1;
    case GL_UNPACK_IMAGE_HEIGHT: v[0] = s->unpack_image_height; return 1;
    case GL_UNPACK_SKIP_IMAGES: v[0] = s->unpack_skip_images; return 1;
    case GL_UNPACK_CLIENT_STORAGE_APPLE: v[0] = s->unpack_client_storage; return 1;
    case GL_PACK_IMAGE_HEIGHT: v[0] = s->pack_image_height; return 1;
    case GL_PACK_SKIP_IMAGES: v[0] = s->pack_skip_images; return 1;
    case GL_PACK_ROW_LENGTH: v[0] = s->pack_row_length; return 1;
    case GL_PACK_SKIP_ROWS: v[0] = s->pack_skip_rows; return 1;
    case GL_PACK_SKIP_PIXELS: v[0] = s->pack_skip_pixels; return 1;
    case GL_FOG_COLOR: *is_color = true; for (int i = 0; i < 4; ++i) v[i] = s->fog_color[i]; return 4;
    case GL_FOG_MODE: v[0] = s->fog_mode; return 1;
    case GL_FOG_DENSITY: v[0] = s->fog_density; return 1;
    case GL_FOG_START: v[0] = s->fog_start; return 1;
    case GL_FOG_END: v[0] = s->fog_end; return 1;
    case GL_LIGHT_MODEL_AMBIENT: *is_color = true; for (int i = 0; i < 4; ++i) v[i] = s->light_model_ambient[i]; return 4;
    case GL_PRIMITIVE_RESTART_INDEX: v[0] = s->primitive_restart_index; return 1;
    case GL_MAJOR_VERSION: v[0] = ctx->profile == GLM_PROFILE_CORE ? 4 : 2; return 1;
    case GL_MINOR_VERSION: v[0] = 1; return 1;
    case GL_NUM_EXTENSIONS: {
        const char *const *list = ctx->profile == GLM_PROFILE_CORE ? glm_core_extensions : glm_legacy_extensions;
        int n = 0;
        while (list[n]) ++n;
        v[0] = n;
        return 1;
    }
    case GL_CONTEXT_PROFILE_MASK: v[0] = ctx->profile == GLM_PROFILE_CORE ? GL_CONTEXT_CORE_PROFILE_BIT : 0; return 1;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: v[0] = 8; return 1;
    case GL_DEPTH_BITS: v[0] = 24; return 1;
    case GL_STENCIL_BITS: v[0] = 8; return 1;
    case GL_DOUBLEBUFFER: v[0] = 1; return 1;
    case GL_SAMPLE_BUFFERS: case GL_SAMPLES: {
        unsigned samples = 0;
        struct glm_framebuffer *fb = glm_framebuffer_get(ctx, ctx->draw_framebuffer);
        /* Default drawable storage is single sampled. Query object storage
           directly, without starting a render encoder or consulting READ. */
        if (ctx->draw_framebuffer && fb) {
            const struct glm_attachment *attachments[GLM_MAX_DRAW_BUFFERS + 2];
            for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) attachments[i] = &fb->color[i];
            attachments[GLM_MAX_DRAW_BUFFERS] = &fb->depth;
            attachments[GLM_MAX_DRAW_BUFFERS + 1] = &fb->stencil;
            for (int i = 0; i < GLM_MAX_DRAW_BUFFERS + 2; ++i) {
                const struct glm_attachment *a = attachments[i];
                if (a->type == GL_TEXTURE) {
                    struct glm_texture *t = glm_texture_get(ctx, a->name);
                    if (t && t->samples > 0) samples = (unsigned)t->samples;
                } else if (a->type == GL_RENDERBUFFER) {
                    struct glm_renderbuffer *rb = glm_renderbuffer_get(ctx, a->name);
                    if (rb && rb->samples > 0) samples = (unsigned)rb->samples;
                }
            }
        }
        v[0] = pname == GL_SAMPLE_BUFFERS ? samples != 0 : samples;
        return 1;
    }
    case GL_MAX_ELEMENTS_VERTICES: case GL_MAX_ELEMENTS_INDICES: v[0] = 1048575; return 1;
    case GL_ATTRIB_STACK_DEPTH: v[0] = ctx->attrib_depth; return 1;
    case GL_MAX_ATTRIB_STACK_DEPTH: v[0] = 16; return 1;
    case GL_CURRENT_RASTER_POSITION_VALID: v[0] = s->raster_valid; return 1;
    case GL_CURRENT_RASTER_POSITION: for (int i = 0; i < 4; ++i) v[i] = s->raster_pos[i]; return 4;
    case GL_ZOOM_X: v[0] = s->pixel_zoom[0]; return 1;
    case GL_ZOOM_Y: v[0] = s->pixel_zoom[1]; return 1;
    case GL_CURRENT_RASTER_COLOR: *is_color = true; for (int i = 0; i < 4; ++i) v[i] = s->raster_color[i]; return 4;
    case GL_RED_SCALE: case GL_GREEN_SCALE: case GL_BLUE_SCALE: case GL_ALPHA_SCALE:
        v[0] = s->pixel_scale[pname == GL_RED_SCALE ? 0 : pname == GL_GREEN_SCALE ? 1 : pname == GL_BLUE_SCALE ? 2 : 3];
        return 1;
    case GL_RED_BIAS: case GL_GREEN_BIAS: case GL_BLUE_BIAS: case GL_ALPHA_BIAS:
        v[0] = s->pixel_bias[pname == GL_RED_BIAS ? 0 : pname == GL_GREEN_BIAS ? 1 : pname == GL_BLUE_BIAS ? 2 : 3];
        return 1;
    case GL_PIXEL_MAP_I_TO_I_SIZE: case GL_PIXEL_MAP_S_TO_S_SIZE: case GL_PIXEL_MAP_I_TO_R_SIZE:
    case GL_PIXEL_MAP_I_TO_G_SIZE: case GL_PIXEL_MAP_I_TO_B_SIZE: case GL_PIXEL_MAP_I_TO_A_SIZE:
    case GL_PIXEL_MAP_R_TO_R_SIZE: case GL_PIXEL_MAP_G_TO_G_SIZE: case GL_PIXEL_MAP_B_TO_B_SIZE:
    case GL_PIXEL_MAP_A_TO_A_SIZE:
        v[0] = s->pixel_map_size[pname - GL_PIXEL_MAP_I_TO_I_SIZE];
        return 1;
    case GL_MAP_COLOR: v[0] = s->map_color; return 1;
    case GL_MAP_STENCIL: v[0] = s->map_stencil; return 1;
    case GL_INDEX_SHIFT: v[0] = s->index_shift; return 1;
    case GL_INDEX_OFFSET: v[0] = s->index_offset; return 1;
    case GL_DEPTH_SCALE: v[0] = s->depth_scale; return 1;
    case GL_DEPTH_BIAS: v[0] = s->depth_bias; return 1;
    case GL_SAMPLE_COVERAGE_VALUE: v[0] = s->sample_coverage; return 1;
    case GL_SAMPLE_COVERAGE_INVERT: v[0] = s->sample_coverage_invert; return 1;
    case GL_MIN_SAMPLE_SHADING_VALUE: v[0] = s->min_sample_shading; return 1;
    case GL_MAX_SAMPLE_MASK_WORDS: v[0] = 1; return 1;
    case GL_MAX_DUAL_SOURCE_DRAW_BUFFERS: v[0] = 1; return 1;
    case GL_MAX_SUBROUTINES: case GL_MAX_SUBROUTINE_UNIFORM_LOCATIONS:
        if (ctx->profile != GLM_PROFILE_CORE) return 0;
        v[0] = 4096;
        return 1;
    case GL_POINT_SIZE_MIN: v[0] = ctx->point_size_min; return 1;
    case GL_POINT_SIZE_MAX: v[0] = ctx->point_size_max; return 1;
    case GL_POINT_FADE_THRESHOLD_SIZE: v[0] = ctx->point_fade_threshold; return 1;
    case GL_POINT_DISTANCE_ATTENUATION: for (int i = 0; i < 3; ++i) v[i] = ctx->point_distance[i]; return 3;
    case GL_POINT_SPRITE_COORD_ORIGIN: v[0] = ctx->point_sprite_origin; return 1;
    case GL_CLAMP_READ_COLOR: v[0] = ctx->clamp_read_color; return 1;
    case GL_NUM_PROGRAM_BINARY_FORMATS: case GL_NUM_SHADER_BINARY_FORMATS: v[0] = 0; return 1;
    case GL_SHADER_COMPILER: v[0] = 1; return 1;
    case GL_MAX_VERTEX_STREAMS: if (ctx->profile != GLM_PROFILE_CORE) return 0; v[0] = 4; return 1;
    case GL_PROGRAM_PIPELINE_BINDING: v[0] = ctx->bound_pipeline; return 1;
    case GL_MAX_VIEWPORTS: if (ctx->profile != GLM_PROFILE_CORE) return 0; v[0] = GLM_MAX_VIEWPORTS; return 1;
    case GL_VIEWPORT_SUBPIXEL_BITS: if (ctx->profile != GLM_PROFILE_CORE) return 0; v[0] = 0; return 1;
    case GL_VIEWPORT_BOUNDS_RANGE: if (ctx->profile != GLM_PROFILE_CORE) return 0; v[0] = -32768; v[1] = 32767; return 2;
    case GL_LAYER_PROVOKING_VERTEX: case GL_VIEWPORT_INDEX_PROVOKING_VERTEX:
        if (ctx->profile != GLM_PROFILE_CORE) return 0;
        v[0] = GL_UNDEFINED_VERTEX;
        return 1;
    case GL_TIMESTAMP: if (ctx->profile != GLM_PROFILE_CORE) return 0; v[0] = (double)glm_now_ns(); return 1;
    case GL_MAX_SERVER_WAIT_TIMEOUT: v[0] = 0; return 1;
    }
    return 0;
}

GLM_HIDDEN int glm_program_query(struct glm_context *ctx, GLenum pname, double *v);

static const float *current_matrix(struct glm_context *ctx)
{
    struct glm_state *s = &ctx->state;
    struct glm_matrix_stack *stack = &s->modelview;
    if (s->matrix_mode == GL_PROJECTION) stack = &s->projection;
    else if (s->matrix_mode == GL_TEXTURE) stack = &s->texture[(s->active_texture - GL_TEXTURE0) % GLM_MAX_TEXTURE_UNITS];
    else if (s->matrix_mode == GL_COLOR) stack = &s->color;
    return stack->m[stack->depth];
}

static int matrix_values(const float *m, bool transpose, double *v)
{
    for (int i = 0; i < 16; ++i) v[i] = transpose ? m[(i % 4) * 4 + i / 4] : m[i];
    return 16;
}

/* Normalized state: integer queries map [-1, 1] to the full range. */
static bool normalized_pname(GLenum pname)
{
    switch (pname) {
    case GL_CURRENT_NORMAL: case GL_DEPTH_RANGE: case GL_DEPTH_CLEAR_VALUE: case GL_CURRENT_SECONDARY_COLOR:
    case GL_CURRENT_RASTER_SECONDARY_COLOR: case GL_ACCUM_CLEAR_VALUE: case GL_CURRENT_WEIGHT_ARB:
        return true;
    default:
        return false;
    }
}

/* State Apple answers beyond query()'s, read from GLMetal's own state. */
static int state_query(struct glm_context *ctx, GLenum pname, double *v, bool *is_color)
{
    struct glm_state *s = &ctx->state;
    struct glm_texture_unit *unit = &s->units[(s->active_texture - GL_TEXTURE0) % GLM_MAX_TEXTURE_UNITS];
    int hint = hint_index(pname);
    if (hint >= 0) {
        v[0] = ctx->hints[hint] ? ctx->hints[hint] : GL_DONT_CARE;
        return 1;
    }
    bool enabled;
    if (client_array_enabled(ctx, pname, &enabled)) {
        v[0] = enabled;
        return 1;
    }
    bool *extra = extra_capability(ctx, pname, false);
    if (extra) {
        v[0] = *extra;
        return 1;
    }
    /* Client array parameters, by legacy array. */
    GLenum array = 0;
    switch (pname) {
    case GL_VERTEX_ARRAY_SIZE: case GL_VERTEX_ARRAY_TYPE: case GL_VERTEX_ARRAY_STRIDE: case GL_VERTEX_ARRAY_BUFFER_BINDING:
        array = GL_VERTEX_ARRAY; break;
    case GL_NORMAL_ARRAY_TYPE: case GL_NORMAL_ARRAY_STRIDE: case GL_NORMAL_ARRAY_BUFFER_BINDING: array = GL_NORMAL_ARRAY; break;
    case GL_COLOR_ARRAY_SIZE: case GL_COLOR_ARRAY_TYPE: case GL_COLOR_ARRAY_STRIDE: case GL_COLOR_ARRAY_BUFFER_BINDING:
        array = GL_COLOR_ARRAY; break;
    case GL_SECONDARY_COLOR_ARRAY_SIZE: case GL_SECONDARY_COLOR_ARRAY_TYPE: case GL_SECONDARY_COLOR_ARRAY_STRIDE:
    case GL_SECONDARY_COLOR_ARRAY_BUFFER_BINDING:
        array = GL_SECONDARY_COLOR_ARRAY; break;
    case GL_FOG_COORD_ARRAY_TYPE: case GL_FOG_COORD_ARRAY_STRIDE: case GL_FOG_COORD_ARRAY_BUFFER_BINDING:
        array = GL_FOG_COORD_ARRAY; break;
    case GL_TEXTURE_COORD_ARRAY_SIZE: case GL_TEXTURE_COORD_ARRAY_TYPE: case GL_TEXTURE_COORD_ARRAY_STRIDE:
    case GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING:
        array = GL_TEXTURE_COORD_ARRAY; break;
    }
    if (array) {
        int slot = glm_client_array_slot(ctx, array);
        if (slot < 0) return 0;
        const struct glm_array *a = &ctx->vao->arrays[slot];
        switch (pname) {
        case GL_VERTEX_ARRAY_SIZE: case GL_COLOR_ARRAY_SIZE: case GL_SECONDARY_COLOR_ARRAY_SIZE:
        case GL_TEXTURE_COORD_ARRAY_SIZE:
            /* Never specified: GL's initial sizes. */
            v[0] = a->size ? a->size : pname == GL_SECONDARY_COLOR_ARRAY_SIZE ? 3 : 4;
            return 1;
        case GL_VERTEX_ARRAY_STRIDE: case GL_NORMAL_ARRAY_STRIDE: case GL_COLOR_ARRAY_STRIDE:
        case GL_SECONDARY_COLOR_ARRAY_STRIDE: case GL_FOG_COORD_ARRAY_STRIDE: case GL_TEXTURE_COORD_ARRAY_STRIDE:
            v[0] = a->stride; return 1;
        case GL_VERTEX_ARRAY_BUFFER_BINDING: case GL_NORMAL_ARRAY_BUFFER_BINDING: case GL_COLOR_ARRAY_BUFFER_BINDING:
        case GL_SECONDARY_COLOR_ARRAY_BUFFER_BINDING: case GL_FOG_COORD_ARRAY_BUFFER_BINDING:
        case GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING:
            v[0] = a->buffer; return 1;
        default: v[0] = a->type ? a->type : GL_FLOAT; return 1;
        }
    }
    if (pname >= GL_DRAW_BUFFER1 && pname <= GL_DRAW_BUFFER15) {
        struct glm_framebuffer *fb = glm_framebuffer_get(ctx, ctx->draw_framebuffer);
        int i = (int)(pname - GL_DRAW_BUFFER0);
        v[0] = fb && i < GLM_MAX_DRAW_BUFFERS ? fb->draw_buffers[i] : GL_NONE;
        return 1;
    }
    switch (pname) {
    case GL_BLEND_SRC: v[0] = s->blend[0].src_rgb; return 1;
    case GL_BLEND_DST: v[0] = s->blend[0].dst_rgb; return 1;
    case GL_LIGHT_MODEL_LOCAL_VIEWER: v[0] = s->light_model_local_viewer; return 1;
    case GL_LIGHT_MODEL_TWO_SIDE: v[0] = s->light_model_two_side; return 1;
    case GL_LIGHT_MODEL_COLOR_CONTROL:
        v[0] = s->light_model_separate_specular ? GL_SEPARATE_SPECULAR_COLOR : GL_SINGLE_COLOR;
        return 1;
    case GL_COLOR_MATERIAL_FACE: v[0] = s->color_material_face; return 1;
    case GL_COLOR_MATERIAL_PARAMETER: v[0] = s->color_material_mode; return 1;
    case GL_FOG_COORD_SRC: v[0] = s->fog_coord_source; return 1;
    case GL_CURRENT_FOG_COORD: v[0] = s->current_fog_coord; return 1;
    case GL_CURRENT_SECONDARY_COLOR:
        *is_color = true;
        for (int i = 0; i < 4; ++i) v[i] = s->current[GLM_ATTR_SECONDARY_COLOR][i];
        return 4;
    case GL_MODELVIEW: return matrix_values(s->modelview.m[s->modelview.depth], false, v);
    case GL_TRANSPOSE_MODELVIEW_MATRIX: return matrix_values(s->modelview.m[s->modelview.depth], true, v);
    case GL_TRANSPOSE_PROJECTION_MATRIX: return matrix_values(s->projection.m[s->projection.depth], true, v);
    case GL_TRANSPOSE_TEXTURE_MATRIX: {
        struct glm_matrix_stack *t = &s->texture[(s->active_texture - GL_TEXTURE0) % GLM_MAX_TEXTURE_UNITS];
        return matrix_values(t->m[t->depth], true, v);
    }
    case GL_COLOR_MATRIX: return matrix_values(s->color.m[s->color.depth], false, v);
    case GL_TRANSPOSE_COLOR_MATRIX: return matrix_values(s->color.m[s->color.depth], true, v);
    case GL_COLOR_MATRIX_STACK_DEPTH: v[0] = s->color.depth + 1; return 1;
    case GL_CURRENT_MATRIX_ARB: return matrix_values(current_matrix(ctx), false, v);
    case GL_TRANSPOSE_CURRENT_MATRIX_ARB: return matrix_values(current_matrix(ctx), true, v);
    case GL_TEXTURE_BINDING_1D_ARRAY: v[0] = unit->bound[GLM_TEX_1D_ARRAY]; return 1;
    case GL_TEXTURE_BINDING_BUFFER: v[0] = unit->bound[GLM_TEX_BUFFER]; return 1;
    case GL_TEXTURE_BINDING_CUBE_MAP_ARRAY: v[0] = unit->bound[GLM_TEX_CUBE_ARRAY]; return 1;
    case GL_TEXTURE_BINDING_2D_MULTISAMPLE: v[0] = unit->bound[GLM_TEX_2D_MULTISAMPLE]; return 1;
    case GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY: v[0] = unit->bound[GLM_TEX_2D_MULTISAMPLE_ARRAY]; return 1;
    case GL_TEXTURE_BUFFER: v[0] = ctx->texture_buffer; return 1;
    case GL_COPY_READ_BUFFER: v[0] = ctx->copy_read_buffer; return 1;
    case GL_COPY_WRITE_BUFFER: v[0] = ctx->copy_write_buffer; return 1;
    case GL_TRANSFORM_FEEDBACK_BUFFER_PAUSED: v[0] = ctx->xfb.paused; return 1;
    case GL_TRANSFORM_FEEDBACK_BUFFER_ACTIVE: v[0] = ctx->xfb.active; return 1;
    case GL_TRANSFORM_FEEDBACK_BINDING: v[0] = ctx->feedback_object; return 1;
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING: v[0] = ctx->transform_feedback_buffer; return 1;
    case 0x8DEF /* GL_UNIFORM_BUFFER_BINDING_EXT */: v[0] = ctx->uniform_buffer; return 1;
    case GL_LIST_BASE: v[0] = ctx->list_base; return 1;
    case GL_CLIENT_ATTRIB_STACK_DEPTH: v[0] = glm_client_attrib_depth(); return 1;
    case GL_QUADS_FOLLOW_PROVOKING_VERTEX_CONVENTION: v[0] = 0; return 1;
    case GL_MAX_SERVER_WAIT_TIMEOUT: v[0] = -1; return 1; /* Apple: -1 from every getter */
    case GL_TIMESTAMP: v[0] = 0; return 1;                /* Apple has no timestamps */
    case GL_DEPTH_BITS: case GL_STENCIL_BITS:
        if (ctx->draw_framebuffer || !ctx->default_bits_known) return 0;
        v[0] = pname == GL_DEPTH_BITS ? ctx->default_depth_bits : ctx->default_stencil_bits;
        return 1;
    }
    return 0;
}


/* GLMetal answers exactly the pnames Apple's implementation answers in
   this profile; state it does not keep reads as Apple's new context does. */
static int typed_query(struct glm_context *ctx, GLenum pname, double *v, bool *is_color)
{
    *is_color = false;
    /* Required core limits follow compiler resources rather than Apple's
       accepted-pname table and generic implementation-limit fallback. */
    if (ctx->profile == GLM_PROFILE_CORE &&
        (pname == GL_MAX_VARYING_COMPONENTS || pname == GL_MAX_VARYING_VECTORS))
        return glm_program_query(ctx, pname, v);
    const struct glm_apple_get *reference = apple_get(ctx, pname);
    if (!reference) {
        glm_log("glGet: pname 0x%04x not accepted in this profile", pname);
        glm_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    /* Texture coordinate state exists for GL_MAX_TEXTURE_COORDS (8) units. */
    switch (pname) {
    case GL_TEXTURE_MATRIX: case GL_TRANSPOSE_TEXTURE_MATRIX: case GL_TEXTURE_STACK_DEPTH: case GL_CURRENT_TEXTURE_COORDS:
    case GL_CURRENT_RASTER_TEXTURE_COORDS:
        if (ctx->state.active_texture - GL_TEXTURE0 >= 8) {
            glm_error(ctx, GL_INVALID_OPERATION);
            return 0;
        }
        if (pname == GL_CURRENT_RASTER_TEXTURE_COORDS) {
            for (int i = 0; i < 4; ++i) v[i] = ctx->state.raster_texcoord[ctx->state.active_texture - GL_TEXTURE0][i];
            return 4;
        }
        break;
    }
    int n = state_query(ctx, pname, v, is_color);
    if (!n) n = query(ctx, pname, v, is_color);
    if (!n) n = glm_program_query(ctx, pname, v);
    if (!n) n = glm_arb_query(ctx, pname, v);
    bool normalized = normalized_pname(pname);
    if (!n) {
        n = reference->count;
        for (int i = 0; i < n; ++i) v[i] = normalized ? reference->values[i] / 2147483647.0 : reference->values[i];
    }
    if (normalized) *is_color = true;
    return n;
}

GLM_EXPORT void glGetIntegerv(GLenum pname, GLint *out)
{
    GLM_CONTEXT(ctx);
    double v[16];
    bool is_color;
    int n = typed_query(ctx, pname, v, &is_color);
    for (int i = 0; i < n; ++i) {
        /* Colours map [-1,1] to the full integer range (GL 2.1 6.1.2). */
        out[i] = is_color ? (GLint)(v[i] * 2147483647.0) : (GLint)(v[i] < 0 ? v[i] - 0.5 : v[i] + 0.5);
        if (!is_color && v[i] == (double)(GLint)v[i]) out[i] = (GLint)v[i];
        /* Unsigned state (stencil masks) keeps its bits: 0xffffffff is -1. */
        if (!is_color && v[i] > 2147483647.0 && v[i] <= 4294967295.0) out[i] = (GLint)(uint32_t)v[i];
    }
}

GLM_EXPORT void glGetFloatv(GLenum pname, GLfloat *out)
{
    GLM_CONTEXT(ctx);
    double v[16];
    bool is_color;
    int n = typed_query(ctx, pname, v, &is_color);
    for (int i = 0; i < n; ++i) out[i] = (GLfloat)v[i];
}

GLM_EXPORT void glGetDoublev(GLenum pname, GLdouble *out)
{
    GLM_CONTEXT(ctx);
    double v[16];
    bool is_color;
    int n = typed_query(ctx, pname, v, &is_color);
    for (int i = 0; i < n; ++i) out[i] = v[i];
}

GLM_EXPORT void glGetBooleanv(GLenum pname, GLboolean *out)
{
    GLM_CONTEXT(ctx);
    double v[16];
    bool is_color;
    int n = typed_query(ctx, pname, v, &is_color);
    for (int i = 0; i < n; ++i) out[i] = v[i] != 0;
}

GLM_EXPORT void glGetInteger64v(GLenum pname, GLint64 *out)
{
    GLM_CONTEXT(ctx);
    double v[16];
    bool is_color;
    int n = typed_query(ctx, pname, v, &is_color);
    for (int i = 0; i < n; ++i)
        out[i] = is_color ? (GLint64)(v[i] * 2147483647.0) : (GLint64)(v[i] < 0 ? v[i] - 0.5 : v[i] + 0.5);
}

/* Indexed state (glGet*i_v); 0 for a pname without an index. */
static int indexed_query(struct glm_context *ctx, GLenum pname, GLuint index, double *v)
{
    const struct glm_state *s = &ctx->state;
    const struct glm_indexed_buffer *binding = NULL;
    switch (pname) {
    case GL_UNIFORM_BUFFER_BINDING: case GL_UNIFORM_BUFFER_START: case GL_UNIFORM_BUFFER_SIZE:
        if (index >= 80) return -1;
        binding = &ctx->uniform_bindings[index];
        break;
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING: case GL_TRANSFORM_FEEDBACK_BUFFER_START: case GL_TRANSFORM_FEEDBACK_BUFFER_SIZE:
        if (index >= 4) return -1;
        binding = &ctx->feedback_bindings[index];
        break;
    }
    if (binding) {
        v[0] = pname == GL_UNIFORM_BUFFER_BINDING || pname == GL_TRANSFORM_FEEDBACK_BUFFER_BINDING ? binding->buffer
               : pname == GL_UNIFORM_BUFFER_START || pname == GL_TRANSFORM_FEEDBACK_BUFFER_START ? (double)binding->offset
                                                                                                   : (double)binding->size;
        return 1;
    }
    bool draw_buffer_state = pname == GL_BLEND || pname == GL_COLOR_WRITEMASK || pname == GL_BLEND_SRC_RGB ||
                             pname == GL_BLEND_DST_RGB || pname == GL_BLEND_SRC_ALPHA || pname == GL_BLEND_DST_ALPHA ||
                             pname == GL_BLEND_EQUATION_RGB || pname == GL_BLEND_EQUATION_ALPHA;
    if (draw_buffer_state && index >= GLM_MAX_DRAW_BUFFERS) return -1;
    switch (pname) {
    case GL_BLEND: v[0] = s->blend[index].enabled; return 1;
    case GL_COLOR_WRITEMASK: for (int i = 0; i < 4; ++i) v[i] = s->color_mask[index][i]; return 4;
    case GL_BLEND_SRC_RGB: v[0] = s->blend[index].src_rgb; return 1;
    case GL_BLEND_DST_RGB: v[0] = s->blend[index].dst_rgb; return 1;
    case GL_BLEND_SRC_ALPHA: v[0] = s->blend[index].src_alpha; return 1;
    case GL_BLEND_DST_ALPHA: v[0] = s->blend[index].dst_alpha; return 1;
    case GL_BLEND_EQUATION_RGB: v[0] = s->blend[index].eq_rgb; return 1;
    case GL_BLEND_EQUATION_ALPHA: v[0] = s->blend[index].eq_alpha; return 1;
    }
    return glm_indexed_state_query(ctx, pname, index, v);
}

/* Indexed values, or the pname's plain value for index 0. */
static int typed_indexed_query(struct glm_context *ctx, GLenum pname, GLuint index, double *v, bool *is_color)
{
    *is_color = false;
    int n = indexed_query(ctx, pname, index, v);
    if (n < 0) {
        glm_error(ctx, GL_INVALID_VALUE);
        return 0;
    }
    return n ? n : typed_query(ctx, pname, v, is_color);
}

GLM_EXPORT void glGetIntegeri_v(GLenum pname, GLuint index, GLint *out)
{
    GLM_CONTEXT(ctx);
    double v[16];
    bool is_color;
    int n = typed_indexed_query(ctx, pname, index, v, &is_color);
    for (int i = 0; i < n; ++i) out[i] = (GLint)(v[i] < 0 ? v[i] - 0.5 : v[i] + 0.5);
}

GLM_EXPORT void glGetInteger64i_v(GLenum pname, GLuint index, GLint64 *out)
{
    GLM_CONTEXT(ctx);
    double v[16];
    bool is_color;
    int n = typed_indexed_query(ctx, pname, index, v, &is_color);
    for (int i = 0; i < n; ++i) out[i] = (GLint64)(v[i] < 0 ? v[i] - 0.5 : v[i] + 0.5);
}

GLM_EXPORT void glGetBooleani_v(GLenum pname, GLuint index, GLboolean *out)
{
    GLM_CONTEXT(ctx);
    double v[16];
    bool is_color;
    int n = typed_indexed_query(ctx, pname, index, v, &is_color);
    for (int i = 0; i < n; ++i) out[i] = v[i] != 0;
}

GLM_EXPORT void glGetFloati_v(GLenum pname, GLuint index, GLfloat *out)
{
    GLM_CONTEXT(ctx);
    double v[16];
    bool is_color;
    int n = typed_indexed_query(ctx, pname, index, v, &is_color);
    for (int i = 0; i < n; ++i) out[i] = (GLfloat)v[i];
}

GLM_EXPORT void glGetDoublei_v(GLenum pname, GLuint index, GLdouble *out)
{
    GLM_CONTEXT(ctx);
    double v[16];
    bool is_color;
    int n = typed_indexed_query(ctx, pname, index, v, &is_color);
    for (int i = 0; i < n; ++i) out[i] = v[i];
}

/* ---- pixel store ---------------------------------------------------------- */

GLM_EXPORT void glPixelStorei(GLenum pname, GLint param)
{
    GLM_CONTEXT(ctx);
    struct glm_state *s = &ctx->state;
    if ((pname == GL_UNPACK_ALIGNMENT || pname == GL_PACK_ALIGNMENT) && param != 1 && param != 2 && param != 4 && param != 8)
        return glm_error(ctx, GL_INVALID_VALUE);
    if (param < 0) return glm_error(ctx, GL_INVALID_VALUE);
    switch (pname) {
    case GL_UNPACK_ALIGNMENT: s->unpack_alignment = param; break;
    case GL_UNPACK_ROW_LENGTH: s->unpack_row_length = param; break;
    case GL_UNPACK_SKIP_ROWS: s->unpack_skip_rows = param; break;
    case GL_UNPACK_SKIP_PIXELS: s->unpack_skip_pixels = param; break;
    case GL_UNPACK_IMAGE_HEIGHT: s->unpack_image_height = param; break;
    case GL_UNPACK_SKIP_IMAGES: s->unpack_skip_images = param; break;
    case GL_UNPACK_SWAP_BYTES: s->unpack_swap_bytes = param; break;
    case GL_UNPACK_LSB_FIRST: s->unpack_lsb_first = param; break;
    case GL_UNPACK_CLIENT_STORAGE_APPLE: s->unpack_client_storage = param; break;
    case GL_PACK_ALIGNMENT: s->pack_alignment = param; break;
    case GL_PACK_ROW_LENGTH: s->pack_row_length = param; break;
    case GL_PACK_SKIP_ROWS: s->pack_skip_rows = param; break;
    case GL_PACK_SKIP_PIXELS: s->pack_skip_pixels = param; break;
    case GL_PACK_IMAGE_HEIGHT: s->pack_image_height = param; break;
    case GL_PACK_SKIP_IMAGES: s->pack_skip_images = param; break;
    case GL_PACK_SWAP_BYTES: s->pack_swap_bytes = param != 0; break;
    case GL_PACK_LSB_FIRST: break;
    case GL_UNPACK_ROW_BYTES_APPLE: s->unpack_row_bytes = param; break;
    case GL_UNPACK_IMAGE_BYTES_APPLE: s->unpack_image_bytes = param; break;
    case GL_PACK_ROW_BYTES_APPLE: s->pack_row_bytes = param; break;
    case GL_PACK_IMAGE_BYTES_APPLE: s->pack_image_bytes = param; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glPixelStoref(GLenum pname, GLfloat param) { glPixelStorei(pname, (GLint)param); }
