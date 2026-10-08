/* ARB_vertex_program / ARB_fragment_program assembly to legacy GLSL.
 *
 * The output uses the GLSL 1.x built-ins (gl_Vertex, gl_ModelViewMatrix,
 * gl_TexCoord, gl_FragColor...) so it goes through the same legacy
 * rewriting, glslang and SPIRV-Cross path as application GLSL, and pairs
 * with fixed-function and GLSL stages through the shared varying locations.
 * Program environment and local parameters live in the GLMARB block; its
 * arrays are sized to what the program references. */
#include "arb_translate.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- growable text ------------------------------------------------------- */

struct text {
    char *data;
    size_t length, capacity;
};

static void emit(struct text *t, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void emit(struct text *t, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (t->length + (size_t)n + 1 > t->capacity) {
        t->capacity = (t->length + (size_t)n + 1) * 2;
        t->data = realloc(t->data, t->capacity);
    }
    vsnprintf(t->data + t->length, (size_t)n + 1, format, args);
    t->length += (size_t)n;
    va_end(args);
}

/* ---- tokens -------------------------------------------------------------- */

enum token_kind { TOK_END, TOK_IDENT, TOK_NUMBER, TOK_PUNCT };

struct token {
    enum token_kind kind;
    char text[64];
    double number;
    size_t position;
};

enum { MAX_NAMES = 512 };

enum name_kind { NAME_TEMP, NAME_ADDRESS, NAME_ATTRIB, NAME_PARAM, NAME_PARAM_ARRAY, NAME_OUTPUT };

struct name {
    char ident[64];
    enum name_kind kind;
    char expression[160]; /* ATTRIB/PARAM/OUTPUT: GLSL expression or variable */
    int array_size;
    /* Arrays aliasing one env/local range index the block directly. */
    const char *alias_array;
    int alias_base;
};

struct parser {
    const char *source;
    size_t length, at;
    struct token token;
    bool vertex;
    struct text declarations, body, prologue;
    struct name names[MAX_NAMES];
    int name_count;
    int env_max, local_max; /* highest index used, -1 none */
    bool env_dynamic, local_dynamic;
    bool position_invariant;
    int fog_option; /* 0 none, 1 linear, 2 exp, 3 exp2 */
    uint32_t results_written;
    uint16_t texcoords_written;
    int draw_buffers_written;
    uint32_t attribs_used; /* generic attributes read */
    uint32_t sampler_declared[16]; /* per unit: bitmask of targets */
    int temp_counter;
    char error[256];
    size_t error_position;
    bool failed;
};

static void fail(struct parser *p, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void fail(struct parser *p, const char *format, ...)
{
    if (p->failed) return;
    p->failed = true;
    p->error_position = p->token.position;
    va_list args;
    va_start(args, format);
    vsnprintf(p->error, sizeof p->error, format, args);
    va_end(args);
}

static void next(struct parser *p)
{
    const char *s = p->source;
    for (;;) {
        while (p->at < p->length && isspace((unsigned char)s[p->at])) ++p->at;
        if (p->at < p->length && s[p->at] == '#') {
            while (p->at < p->length && s[p->at] != '\n') ++p->at;
            continue;
        }
        break;
    }
    struct token *t = &p->token;
    t->position = p->at;
    t->text[0] = 0;
    if (p->at >= p->length) {
        t->kind = TOK_END;
        return;
    }
    char c = s[p->at];
    if (isalpha((unsigned char)c) || c == '_' || c == '$') {
        size_t n = 0;
        while (p->at < p->length && (isalnum((unsigned char)s[p->at]) || s[p->at] == '_' || s[p->at] == '$')) {
            if (n + 1 < sizeof t->text) t->text[n++] = s[p->at];
            ++p->at;
        }
        t->text[n] = 0;
        t->kind = TOK_IDENT;
        return;
    }
    if (isdigit((unsigned char)c) || (c == '.' && p->at + 1 < p->length && isdigit((unsigned char)s[p->at + 1]))) {
        /* Scanned by hand: "0..3" is a range, not "0." then ".3". */
        size_t end = p->at;
        while (end < p->length && isdigit((unsigned char)s[end])) ++end;
        if (end < p->length && s[end] == '.' && !(end + 1 < p->length && s[end + 1] == '.')) {
            ++end;
            while (end < p->length && isdigit((unsigned char)s[end])) ++end;
        }
        if (end < p->length && (s[end] == 'e' || s[end] == 'E')) {
            size_t e = end + 1;
            if (e < p->length && (s[e] == '+' || s[e] == '-')) ++e;
            if (e < p->length && isdigit((unsigned char)s[e])) {
                end = e;
                while (end < p->length && isdigit((unsigned char)s[end])) ++end;
            }
        }
        size_t n = end - p->at;
        if (n >= sizeof t->text) n = sizeof t->text - 1;
        memcpy(t->text, s + p->at, n);
        t->text[n] = 0;
        t->number = strtod(t->text, NULL);
        p->at = end;
        t->kind = TOK_NUMBER;
        return;
    }
    t->kind = TOK_PUNCT;
    if (c == '.' && p->at + 1 < p->length && s[p->at + 1] == '.') {
        strcpy(t->text, "..");
        p->at += 2;
        return;
    }
    t->text[0] = c;
    t->text[1] = 0;
    ++p->at;
}

static bool is(struct parser *p, const char *text) { return p->token.kind != TOK_END && !strcmp(p->token.text, text); }

static bool accept(struct parser *p, const char *text)
{
    if (!is(p, text)) return false;
    next(p);
    return true;
}

static void expect(struct parser *p, const char *text)
{
    if (!accept(p, text)) fail(p, "expected '%s' near '%s'", text, p->token.text);
}

static int expect_integer(struct parser *p)
{
    if (p->token.kind != TOK_NUMBER) {
        fail(p, "expected an integer near '%s'", p->token.text);
        return 0;
    }
    int v = (int)p->token.number;
    next(p);
    return v;
}

static void expect_ident(struct parser *p, char *out, size_t size)
{
    if (p->token.kind != TOK_IDENT) {
        fail(p, "expected an identifier near '%s'", p->token.text);
        out[0] = 0;
        return;
    }
    snprintf(out, size, "%s", p->token.text);
    next(p);
}

static struct name *find_name(struct parser *p, const char *ident)
{
    for (int i = 0; i < p->name_count; ++i)
        if (!strcmp(p->names[i].ident, ident)) return &p->names[i];
    return NULL;
}

static struct name *add_name(struct parser *p, const char *ident, enum name_kind kind)
{
    if (find_name(p, ident)) {
        fail(p, "'%s' is already declared", ident);
        return NULL;
    }
    if (p->name_count == MAX_NAMES) {
        fail(p, "too many names");
        return NULL;
    }
    struct name *n = &p->names[p->name_count++];
    memset(n, 0, sizeof *n);
    snprintf(n->ident, sizeof n->ident, "%s", ident);
    n->kind = kind;
    return n;
}

/* ---- bindings ------------------------------------------------------------ */

static void format_number(char *out, size_t size, double v)
{
    snprintf(out, size, "%.9g", v);
    if (!strpbrk(out, ".eEn")) strncat(out, ".0", size - strlen(out) - 1);
}

/* "[n]" after a binding keyword, or default. */
static int optional_index(struct parser *p, int fallback)
{
    if (!accept(p, "[")) return fallback;
    int v = expect_integer(p);
    expect(p, "]");
    return v;
}

static const char *face_prefix(struct parser *p, bool *back)
{
    *back = false;
    if (is(p, ".")) {
        /* Peek: .front or .back */
        size_t save = p->at;
        struct token saved = p->token;
        next(p);
        if (is(p, "front")) { next(p); return "Front"; }
        if (is(p, "back")) { next(p); *back = true; return "Back"; }
        p->at = save;
        p->token = saved;
    }
    return "Front";
}

/* Matrix rows: fills up to 4 row expressions of a state.matrix binding. */
static int matrix_rows(struct parser *p, char rows[4][160], int *first_row)
{
    char base[96];
    int index = 0;
    char which[32];
    expect_ident(p, which, sizeof which);
    if (!strcmp(which, "modelview")) {
        index = optional_index(p, 0);
        snprintf(base, sizeof base, "gl_ModelViewMatrix");
        if (index) fail(p, "only modelview[0] is supported");
    } else if (!strcmp(which, "projection")) snprintf(base, sizeof base, "gl_ProjectionMatrix");
    else if (!strcmp(which, "mvp")) snprintf(base, sizeof base, "gl_ModelViewProjectionMatrix");
    else if (!strcmp(which, "texture")) {
        index = optional_index(p, 0);
        snprintf(base, sizeof base, "gl_TextureMatrix");
    } else if (!strcmp(which, "program") || !strcmp(which, "palette")) {
        optional_index(p, 0);
        snprintf(base, sizeof base, "identity");
    } else {
        fail(p, "unknown matrix '%s'", which);
        return 0;
    }
    /* Modifier: rows of M are columns of transpose(M). */
    const char *suffix = "Transpose"; /* rows of M */
    int lo = 0, hi = 3;
    while (accept(p, ".")) {
        char word[32];
        expect_ident(p, word, sizeof word);
        if (!strcmp(word, "inverse")) suffix = "InverseTranspose";
        else if (!strcmp(word, "transpose")) suffix = !strcmp(suffix, "InverseTranspose") ? "Inverse" : "";
        else if (!strcmp(word, "invtrans")) suffix = "Inverse";
        else if (!strcmp(word, "row")) {
            expect(p, "[");
            lo = hi = expect_integer(p);
            if (accept(p, "..")) hi = expect_integer(p);
            expect(p, "]");
            break;
        } else fail(p, "unknown matrix modifier '%s'", word);
    }
    if (lo < 0 || hi > 3 || lo > hi) fail(p, "bad matrix row range");
    *first_row = lo;
    for (int r = lo; r <= hi && r - lo < 4; ++r) {
        if (!strcmp(base, "identity"))
            snprintf(rows[r - lo], 160, "vec4(%s)", r == 0 ? "1.0, 0.0, 0.0, 0.0" : r == 1 ? "0.0, 1.0, 0.0, 0.0"
                                                     : r == 2 ? "0.0, 0.0, 1.0, 0.0" : "0.0, 0.0, 0.0, 1.0");
        else if (!strcmp(base, "gl_TextureMatrix"))
            snprintf(rows[r - lo], 160, "gl_TextureMatrix%s[%d][%d]", suffix, index, r);
        else
            snprintf(rows[r - lo], 160, "%s%s[%d]", base, suffix, r);
    }
    return hi - lo + 1;
}

static void use_parameter(struct parser *p, bool env, int index)
{
    if (index < 0 || index >= 256) {
        fail(p, "parameter index %d out of range", index);
        return;
    }
    int *max = env ? &p->env_max : &p->local_max;
    if (index > *max) *max = index;
}

/* A state.* or program.* binding: up to `capacity` vec4 expressions (more
   than one for matrices and ranges). Returns the count. `alias` reports a
   single env/local range for direct indexing. */
static int parameter_binding(struct parser *p, char (*out)[160], int capacity, const char **alias, int *alias_base)
{
    if (alias) *alias = NULL;
    char root[32];
    expect_ident(p, root, sizeof root);
    if (!strcmp(root, "program")) {
        expect(p, ".");
        char which[16];
        expect_ident(p, which, sizeof which);
        bool env = !strcmp(which, "env");
        if (!env && strcmp(which, "local")) fail(p, "expected program.env or program.local");
        expect(p, "[");
        int lo = expect_integer(p), hi = lo;
        if (accept(p, "..")) hi = expect_integer(p);
        expect(p, "]");
        use_parameter(p, env, lo);
        use_parameter(p, env, hi);
        if (alias && hi > lo) {
            *alias = env ? "glm_arb_env" : "glm_arb_local";
            *alias_base = lo;
        }
        int n = 0;
        for (int i = lo; i <= hi && n < capacity; ++i)
            snprintf(out[n++], 160, "%s[%d]", env ? "glm_arb_env" : "glm_arb_local", i);
        return n;
    }
    if (strcmp(root, "state")) {
        fail(p, "unknown binding '%s'", root);
        return 0;
    }
    expect(p, ".");
    char group[32];
    expect_ident(p, group, sizeof group);
    if (!strcmp(group, "matrix")) {
        expect(p, ".");
        char rows[4][160];
        int first;
        int n = matrix_rows(p, rows, &first);
        for (int i = 0; i < n && i < capacity; ++i) memcpy(out[i], rows[i], 160);
        return n < capacity ? n : capacity;
    }
    if (!strcmp(group, "material")) {
        bool back;
        const char *face = face_prefix(p, &back);
        expect(p, ".");
        char prop[32];
        expect_ident(p, prop, sizeof prop);
        if (!strcmp(prop, "shininess")) snprintf(out[0], 160, "vec4(gl_%sMaterial.shininess, 0.0, 0.0, 1.0)", face);
        else if (!strcmp(prop, "ambient") || !strcmp(prop, "diffuse") || !strcmp(prop, "specular") || !strcmp(prop, "emission"))
            snprintf(out[0], 160, "gl_%sMaterial.%s", face, prop);
        else fail(p, "unknown material property '%s'", prop);
        return 1;
    }
    if (!strcmp(group, "light")) {
        int n = optional_index(p, 0);
        expect(p, ".");
        char prop[32];
        expect_ident(p, prop, sizeof prop);
        if (!strcmp(prop, "attenuation"))
            snprintf(out[0], 160, "vec4(gl_LightSource[%d].constantAttenuation, gl_LightSource[%d].linearAttenuation, "
                                  "gl_LightSource[%d].quadraticAttenuation, gl_LightSource[%d].spotExponent)", n, n, n, n);
        else if (!strcmp(prop, "spot")) {
            expect(p, ".");
            char d[32];
            expect_ident(p, d, sizeof d);
            snprintf(out[0], 160, "vec4(gl_LightSource[%d].spotDirection, gl_LightSource[%d].spotCosCutoff)", n, n);
        } else if (!strcmp(prop, "half")) snprintf(out[0], 160, "vec4(gl_LightSource[%d].halfVector.xyz, 1.0)", n);
        else if (!strcmp(prop, "ambient") || !strcmp(prop, "diffuse") || !strcmp(prop, "specular") || !strcmp(prop, "position"))
            snprintf(out[0], 160, "gl_LightSource[%d].%s", n, prop);
        else fail(p, "unknown light property '%s'", prop);
        return 1;
    }
    if (!strcmp(group, "lightmodel")) {
        expect(p, ".");
        char prop[32];
        expect_ident(p, prop, sizeof prop);
        if (!strcmp(prop, "ambient")) snprintf(out[0], 160, "gl_LightModel.ambient");
        else if (!strcmp(prop, "scenecolor")) snprintf(out[0], 160, "gl_FrontLightModelProduct.sceneColor");
        else if (!strcmp(prop, "front") || !strcmp(prop, "back")) {
            expect(p, ".");
            char s[32];
            expect_ident(p, s, sizeof s);
            snprintf(out[0], 160, "gl_%sLightModelProduct.sceneColor", !strcmp(prop, "front") ? "Front" : "Back");
        } else fail(p, "unknown lightmodel property '%s'", prop);
        return 1;
    }
    if (!strcmp(group, "lightprod")) {
        int n = optional_index(p, 0);
        bool back;
        const char *face = face_prefix(p, &back);
        expect(p, ".");
        char prop[32];
        expect_ident(p, prop, sizeof prop);
        snprintf(out[0], 160, "gl_%sLightProduct[%d].%s", face, n, prop);
        return 1;
    }
    if (!strcmp(group, "texgen")) {
        int n = optional_index(p, 0);
        expect(p, ".");
        char space[16], coord[8];
        expect_ident(p, space, sizeof space);
        expect(p, ".");
        expect_ident(p, coord, sizeof coord);
        snprintf(out[0], 160, "gl_%sPlane%c[%d]", !strcmp(space, "eye") ? "Eye" : "Object", toupper(coord[0]), n);
        return 1;
    }
    if (!strcmp(group, "fog")) {
        expect(p, ".");
        char prop[16];
        expect_ident(p, prop, sizeof prop);
        if (!strcmp(prop, "color")) snprintf(out[0], 160, "gl_Fog.color");
        else snprintf(out[0], 160, "vec4(gl_Fog.density, gl_Fog.start, gl_Fog.end, gl_Fog.scale)");
        return 1;
    }
    if (!strcmp(group, "clip")) {
        int n = optional_index(p, 0);
        expect(p, ".");
        char prop[16];
        expect_ident(p, prop, sizeof prop);
        snprintf(out[0], 160, "gl_ClipPlane[%d]", n);
        return 1;
    }
    if (!strcmp(group, "point")) {
        expect(p, ".");
        char prop[16];
        expect_ident(p, prop, sizeof prop);
        if (!strcmp(prop, "size"))
            snprintf(out[0], 160, "vec4(gl_Point.size, gl_Point.sizeMin, gl_Point.sizeMax, gl_Point.fadeThresholdSize)");
        else
            snprintf(out[0], 160, "vec4(gl_Point.distanceConstantAttenuation, gl_Point.distanceLinearAttenuation, "
                                  "gl_Point.distanceQuadraticAttenuation, 1.0)");
        return 1;
    }
    if (!strcmp(group, "texenv")) {
        int n = optional_index(p, 0);
        expect(p, ".");
        char prop[16];
        expect_ident(p, prop, sizeof prop);
        snprintf(out[0], 160, "gl_TextureEnvColor[%d]", n);
        return 1;
    }
    if (!strcmp(group, "depth")) {
        expect(p, ".");
        char prop[16];
        expect_ident(p, prop, sizeof prop);
        snprintf(out[0], 160, "vec4(gl_DepthRange.near, gl_DepthRange.far, gl_DepthRange.diff, 1.0)");
        return 1;
    }
    fail(p, "unknown state '%s'", group);
    return 0;
}

/* {a, b, c, d} with missing components (0, 0, 0, 1); a lone number
   replicates when `scalar_replicates`. */
static void constant_vector(struct parser *p, char *out, bool scalar_replicates)
{
    double v[4] = {0, 0, 0, 1};
    int n = 0;
    if (accept(p, "{")) {
        do {
            double sign = accept(p, "-") ? -1 : (accept(p, "+"), 1);
            if (p->token.kind != TOK_NUMBER) {
                fail(p, "expected a number near '%s'", p->token.text);
                return;
            }
            if (n < 4) v[n] = sign * p->token.number;
            ++n;
            next(p);
        } while (accept(p, ","));
        expect(p, "}");
        if (n > 4) fail(p, "too many components");
    } else {
        double sign = accept(p, "-") ? -1 : (accept(p, "+"), 1);
        if (p->token.kind != TOK_NUMBER) {
            fail(p, "expected a number near '%s'", p->token.text);
            return;
        }
        v[0] = sign * p->token.number;
        next(p);
        if (scalar_replicates) v[1] = v[2] = v[3] = v[0];
    }
    char c[4][40];
    for (int i = 0; i < 4; ++i) format_number(c[i], sizeof c[i], v[i]);
    snprintf(out, 160, "vec4(%s, %s, %s, %s)", c[0], c[1], c[2], c[3]);
}

static bool starts_constant(struct parser *p)
{
    return is(p, "{") || p->token.kind == TOK_NUMBER || is(p, "-") || is(p, "+");
}

/* One PARAM initializer item (possibly several vec4s). */
static int parameter_item(struct parser *p, char (*out)[160], int capacity, bool scalar_replicates, const char **alias,
                          int *alias_base)
{
    if (alias) *alias = NULL;
    if (starts_constant(p)) {
        constant_vector(p, out[0], scalar_replicates);
        return 1;
    }
    return parameter_binding(p, out, capacity, alias, alias_base);
}

static const char *legacy_attribute(int index)
{
    switch (index) {
    case 0: return "gl_Vertex";
    case 2: return "vec4(gl_Normal, 1.0)";
    case 3: return "gl_Color";
    case 4: return "gl_SecondaryColor";
    case 5: return "vec4(gl_FogCoord, 0.0, 0.0, 1.0)";
    default: return NULL;
    }
}

/* ".primary" / ".secondary" after a colour binding; anything else after
   the dot (a swizzle) is left alone. Returns true for secondary. */
static bool color_qualifier(struct parser *p)
{
    if (!is(p, ".")) return false;
    size_t save = p->at;
    struct token saved = p->token;
    next(p);
    if (is(p, "primary")) { next(p); return false; }
    if (is(p, "secondary")) { next(p); return true; }
    p->at = save;
    p->token = saved;
    return false;
}

/* vertex.* / fragment.* attribute bindings. */
static void attribute_binding(struct parser *p, char *out)
{
    char root[16];
    expect_ident(p, root, sizeof root);
    expect(p, ".");
    char what[32];
    expect_ident(p, what, sizeof what);
    if (p->vertex) {
        if (strcmp(root, "vertex")) { fail(p, "vertex programs read vertex.* attributes"); return; }
        if (!strcmp(what, "position")) snprintf(out, 160, "gl_Vertex");
        else if (!strcmp(what, "normal")) snprintf(out, 160, "vec4(gl_Normal, 1.0)");
        else if (!strcmp(what, "fogcoord")) snprintf(out, 160, "vec4(gl_FogCoord, 0.0, 0.0, 1.0)");
        else if (!strcmp(what, "weight") || !strcmp(what, "matrixindex")) {
            optional_index(p, 0);
            snprintf(out, 160, "vec4(1.0, 0.0, 0.0, 0.0)");
        } else if (!strcmp(what, "color")) {
            bool secondary = color_qualifier(p);
            snprintf(out, 160, secondary ? "gl_SecondaryColor" : "gl_Color");
        } else if (!strcmp(what, "texcoord")) {
            int n = optional_index(p, 0);
            snprintf(out, 160, "gl_MultiTexCoord%d", n);
        } else if (!strcmp(what, "attrib")) {
            int n = optional_index(p, 0);
            if (n < 0 || n >= 16) { fail(p, "attribute index out of range"); return; }
            const char *legacy = n >= 8 ? NULL : legacy_attribute(n);
            if (n >= 8) snprintf(out, 160, "gl_MultiTexCoord%d", n - 8);
            else if (legacy) snprintf(out, 160, "%s", legacy);
            else {
                p->attribs_used |= 1u << n;
                snprintf(out, 160, "arb_attrib%d", n);
            }
        } else fail(p, "unknown vertex attribute '%s'", what);
        return;
    }
    if (strcmp(root, "fragment")) { fail(p, "fragment programs read fragment.* attributes"); return; }
    if (!strcmp(what, "color")) {
        bool secondary = color_qualifier(p);
        snprintf(out, 160, secondary ? "gl_SecondaryColor" : "gl_Color");
    } else if (!strcmp(what, "texcoord")) {
        int n = optional_index(p, 0);
        snprintf(out, 160, "gl_TexCoord[%d]", n);
    } else if (!strcmp(what, "fogcoord")) snprintf(out, 160, "vec4(gl_FogFragCoord, 0.0, 0.0, 1.0)");
    else if (!strcmp(what, "position")) snprintf(out, 160, "gl_FragCoord");
    else fail(p, "unknown fragment attribute '%s'", what);
}

enum result_slot {
    RESULT_POSITION, RESULT_COLOR0, RESULT_COLOR1, RESULT_BACK0, RESULT_BACK1, RESULT_FOG, RESULT_POINT, RESULT_DEPTH,
    RESULT_TEX0, /* 8 */ RESULT_DRAW0 = RESULT_TEX0 + 8, /* 8 draw buffers */ RESULT_COUNT = RESULT_DRAW0 + 8,
};

static const char *result_variable(int slot)
{
    static char names[RESULT_COUNT][24];
    snprintf(names[slot], sizeof names[slot], "glm_result%d", slot);
    return names[slot];
}

/* result.* binding to a result slot. */
static int result_binding(struct parser *p)
{
    char root[16];
    expect_ident(p, root, sizeof root);
    if (strcmp(root, "result")) { fail(p, "expected result.*"); return 0; }
    expect(p, ".");
    char what[32];
    expect_ident(p, what, sizeof what);
    int slot = -1;
    if (p->vertex) {
        if (!strcmp(what, "position")) slot = RESULT_POSITION;
        else if (!strcmp(what, "fogcoord")) slot = RESULT_FOG;
        else if (!strcmp(what, "pointsize")) slot = RESULT_POINT;
        else if (!strcmp(what, "texcoord")) slot = RESULT_TEX0 + optional_index(p, 0);
        else if (!strcmp(what, "color")) {
            bool back = false, secondary = false;
            /* .primary .secondary .front[.primary|.secondary] .back[...] */
            for (int k = 0; k < 2 && is(p, "."); ++k) {
                size_t save = p->at;
                struct token saved = p->token;
                next(p);
                if (is(p, "front")) next(p);
                else if (is(p, "back")) { back = true; next(p); }
                else if (is(p, "primary")) next(p);
                else if (is(p, "secondary")) { secondary = true; next(p); }
                else { p->at = save; p->token = saved; break; }
            }
            slot = back ? (secondary ? RESULT_BACK1 : RESULT_BACK0) : (secondary ? RESULT_COLOR1 : RESULT_COLOR0);
        }
    } else {
        if (!strcmp(what, "color")) slot = RESULT_DRAW0 + optional_index(p, 0);
        else if (!strcmp(what, "depth")) slot = RESULT_DEPTH;
    }
    if (slot < 0 || slot >= RESULT_COUNT) {
        fail(p, "unknown result '%s'", what);
        return 0;
    }
    return slot;
}

/* ---- operands ------------------------------------------------------------ */

static int swizzle_component(char c)
{
    switch (c) {
    case 'x': case 'r': return 0;
    case 'y': case 'g': return 1;
    case 'z': case 'b': return 2;
    case 'w': case 'a': return 3;
    default: return -1;
    }
}

/* ".xyzw"-style suffix after an operand. Returns a GLSL swizzle or "". */
static void source_swizzle(struct parser *p, char *out)
{
    out[0] = 0;
    if (!is(p, ".")) return;
    size_t save = p->at;
    struct token saved = p->token;
    next(p);
    if (p->token.kind != TOK_IDENT) {
        p->at = save;
        p->token = saved;
        return;
    }
    const char *s = p->token.text;
    size_t n = strlen(s);
    if (n != 1 && n != 4) {
        p->at = save;
        p->token = saved;
        return;
    }
    char comps[5] = {0};
    for (size_t i = 0; i < n; ++i) {
        int c = swizzle_component(s[i]);
        if (c < 0) { p->at = save; p->token = saved; return; }
        comps[i] = "xyzw"[c];
    }
    if (n == 1) comps[1] = comps[2] = comps[3] = comps[0];
    snprintf(out, 8, ".%s", comps);
    next(p);
}

/* A source operand as a vec4 GLSL expression. */
static void source_operand(struct parser *p, char *out, size_t size)
{
    bool negate = false;
    if (accept(p, "-")) negate = true;
    else accept(p, "+");
    char base[200] = {0};
    if (p->token.kind == TOK_NUMBER || is(p, "{")) {
        char c[160];
        constant_vector(p, c, true);
        snprintf(base, sizeof base, "%s", c);
    } else if (p->token.kind == TOK_IDENT) {
        if (is(p, "vertex") || is(p, "fragment")) {
            char e[160];
            attribute_binding(p, e);
            snprintf(base, sizeof base, "(%s)", e);
        } else if (is(p, "state") || is(p, "program")) {
            char e[4][160];
            if (parameter_binding(p, e, 1, NULL, NULL) < 1) return;
            snprintf(base, sizeof base, "(%s)", e[0]);
        } else {
            char ident[64];
            expect_ident(p, ident, sizeof ident);
            struct name *n = find_name(p, ident);
            if (!n) {
                fail(p, "undeclared '%s'", ident);
                return;
            }
            if (n->kind == NAME_PARAM_ARRAY) {
                expect(p, "[");
                char index[96];
                if (p->token.kind == TOK_NUMBER) {
                    snprintf(index, sizeof index, "%d", expect_integer(p));
                } else {
                    /* a.x [+ offset] */
                    char address[64];
                    expect_ident(p, address, sizeof address);
                    struct name *a = find_name(p, address);
                    if (!a || a->kind != NAME_ADDRESS) fail(p, "'%s' is not an address register", address);
                    expect(p, ".");
                    char comp[8];
                    expect_ident(p, comp, sizeof comp);
                    int offset = 0;
                    if (accept(p, "+")) offset = expect_integer(p);
                    else if (accept(p, "-")) offset = -expect_integer(p);
                    snprintf(index, sizeof index, "clamp(%s.x + %d, 0, %d)", address, offset + (n->alias_array ? n->alias_base : 0),
                             n->alias_array ? n->alias_base + n->array_size - 1 : n->array_size - 1);
                    if (n->alias_array) {
                        if (!strcmp(n->alias_array, "glm_arb_env")) p->env_dynamic = true;
                        else p->local_dynamic = true;
                    }
                    expect(p, "]");
                    snprintf(base, sizeof base, "%s[%s]", n->alias_array ? n->alias_array : n->ident, index);
                    goto swizzle;
                }
                expect(p, "]");
                if (n->alias_array) snprintf(base, sizeof base, "%s[%d]", n->alias_array, n->alias_base + atoi(index));
                else snprintf(base, sizeof base, "%s[%s]", n->ident, index);
            } else if (n->kind == NAME_TEMP || n->kind == NAME_PARAM || n->kind == NAME_ATTRIB) {
                snprintf(base, sizeof base, "%s", n->kind == NAME_TEMP ? n->ident : n->expression);
            } else if (n->kind == NAME_OUTPUT) {
                fail(p, "result registers cannot be read");
                return;
            } else {
                fail(p, "'%s' cannot be read", ident);
                return;
            }
        }
    } else {
        fail(p, "unexpected '%s'", p->token.text);
        return;
    }
swizzle:;
    char swz[8];
    source_swizzle(p, swz);
    snprintf(out, size, "%s(%s%s)", negate ? "-" : "", base, swz);
}

/* Destination: variable and write mask ("xyzw" subset). */
static void destination(struct parser *p, char *variable, size_t size, char *mask, int *slot)
{
    *slot = -1;
    if (is(p, "result")) {
        *slot = result_binding(p);
        snprintf(variable, size, "%s", result_variable(*slot));
    } else {
        char ident[64];
        expect_ident(p, ident, sizeof ident);
        struct name *n = find_name(p, ident);
        if (!n) {
            fail(p, "undeclared '%s'", ident);
            return;
        }
        if (n->kind == NAME_OUTPUT) {
            *slot = atoi(n->expression);
            snprintf(variable, size, "%s", result_variable(*slot));
        } else if (n->kind == NAME_TEMP || n->kind == NAME_ADDRESS) {
            snprintf(variable, size, "%s", ident);
        } else {
            fail(p, "'%s' cannot be written", ident);
            return;
        }
    }
    strcpy(mask, "xyzw");
    if (accept(p, ".")) {
        char m[16];
        expect_ident(p, m, sizeof m);
        size_t n = 0;
        int last = -1;
        for (size_t i = 0; m[i]; ++i) {
            int c = swizzle_component(m[i]);
            if (c <= last) {
                fail(p, "bad write mask '%s'", m);
                return;
            }
            last = c;
            mask[n++] = "xyzw"[c];
        }
        mask[n] = 0;
    }
    if (*slot >= 0) {
        p->results_written |= 1u << *slot;
        if (*slot >= RESULT_TEX0 && *slot < RESULT_TEX0 + 8) p->texcoords_written |= (uint16_t)(1 << (*slot - RESULT_TEX0));
        if (*slot >= RESULT_DRAW0 && *slot - RESULT_DRAW0 + 1 > p->draw_buffers_written)
            p->draw_buffers_written = *slot - RESULT_DRAW0 + 1;
    }
}

/* texture[n], TARGET */
static void texture_operand(struct parser *p, int *unit, char *target, size_t size)
{
    char word[16];
    expect_ident(p, word, sizeof word);
    if (strcmp(word, "texture")) fail(p, "expected texture[n]");
    *unit = optional_index(p, 0);
    if (*unit < 0 || *unit >= 16) fail(p, "texture unit out of range");
    expect(p, ",");
    /* "2D" scans as the number 2 and the identifier D. */
    if (p->token.kind == TOK_NUMBER) {
        int dims = expect_integer(p);
        char rest[8];
        expect_ident(p, rest, sizeof rest);
        snprintf(target, size, "%d%s", dims, rest);
    } else {
        expect_ident(p, target, size);
    }
}

enum { TARGET_1D, TARGET_2D, TARGET_3D, TARGET_CUBE, TARGET_RECT, TARGET_SHADOW1D, TARGET_SHADOW2D, TARGET_SHADOWRECT };

static int target_code(struct parser *p, const char *t)
{
    static const char *const names[] = {"1D", "2D", "3D", "CUBE", "RECT", "SHADOW1D", "SHADOW2D", "SHADOWRECT"};
    for (int i = 0; i < 8; ++i)
        if (!strcmp(t, names[i])) return i;
    fail(p, "unknown texture target '%s'", t);
    return TARGET_2D;
}

static void texture_sample(struct parser *p, const char *op, const char *coord, int unit, int target, char *out, size_t size)
{
    static const char *const sampler_types[] = {"sampler2D", "sampler2D", "sampler3D", "samplerCube", "sampler2D",
                                                "sampler2DShadow", "sampler2DShadow", "sampler2DShadow"};
    char sampler[40];
    snprintf(sampler, sizeof sampler, "glm_arb_tex%d_%d", unit, target);
    if (!(p->sampler_declared[unit] & (1u << target))) {
        p->sampler_declared[unit] |= 1u << target;
        emit(&p->declarations, "uniform %s %s;\n", sampler_types[target], sampler);
    }
    bool project = !strcmp(op, "TXP"), bias = !strcmp(op, "TXB");
    char c[300];
    /* Projection divides by q for every target but cube maps. */
    if (project && target != TARGET_CUBE) snprintf(c, sizeof c, "(%s / %s.w)", coord, coord);
    else snprintf(c, sizeof c, "%s", coord);
    char bias_arg[160] = "";
    if (bias) snprintf(bias_arg, sizeof bias_arg, ", %s.w", coord);
    switch (target) {
    case TARGET_1D: snprintf(out, size, "texture(%s, vec2(%s.x, 0.5)%s)", sampler, c, bias_arg); break;
    case TARGET_2D: snprintf(out, size, "texture(%s, %s.xy%s)", sampler, c, bias_arg); break;
    case TARGET_3D: case TARGET_CUBE: snprintf(out, size, "texture(%s, %s.xyz%s)", sampler, c, bias_arg); break;
    /* Rectangle samplers take texel coordinates (unnormalized samplers). */
    case TARGET_RECT: snprintf(out, size, "texture(%s, %s.xy)", sampler, c); break;
    case TARGET_SHADOW1D:
        snprintf(out, size, "vec4(vec3(texture(%s, vec3(%s.x, 0.5, %s.z))), 1.0)", sampler, c, c); break;
    case TARGET_SHADOW2D: snprintf(out, size, "vec4(vec3(texture(%s, %s.xyz)), 1.0)", sampler, c); break;
    case TARGET_SHADOWRECT:
        snprintf(out, size, "vec4(vec3(texture(%s, %s.xyz)), 1.0)", sampler, c);
        break;
    }
}

/* ---- statements ---------------------------------------------------------- */

struct opcode {
    const char *name;
    int sources;
    bool vertex, fragment;
};

static const struct opcode opcodes[] = {
    {"ABS", 1, 1, 1}, {"ADD", 2, 1, 1}, {"ARL", 1, 1, 0}, {"CMP", 3, 0, 1}, {"COS", 1, 0, 1}, {"DP3", 2, 1, 1},
    {"DP4", 2, 1, 1}, {"DPH", 2, 1, 1}, {"DST", 2, 1, 1}, {"EX2", 1, 1, 1}, {"EXP", 1, 1, 0}, {"FLR", 1, 1, 1},
    {"FRC", 1, 1, 1}, {"KIL", 1, 0, 1}, {"LG2", 1, 1, 1}, {"LIT", 1, 1, 1}, {"LOG", 1, 1, 0}, {"LRP", 3, 0, 1},
    {"MAD", 3, 1, 1}, {"MAX", 2, 1, 1}, {"MIN", 2, 1, 1}, {"MOV", 1, 1, 1}, {"MUL", 2, 1, 1}, {"POW", 2, 1, 1},
    {"RCP", 1, 1, 1}, {"RSQ", 1, 1, 1}, {"SCS", 1, 0, 1}, {"SGE", 2, 1, 1}, {"SIN", 1, 0, 1}, {"SLT", 2, 1, 1},
    {"SUB", 2, 1, 1}, {"SWZ", 1, 1, 1}, {"TEX", 1, 0, 1}, {"TXB", 1, 0, 1}, {"TXP", 1, 0, 1}, {"XPD", 2, 1, 1},
};

static const struct opcode *find_opcode(const char *word, bool *saturate)
{
    char base[16];
    snprintf(base, sizeof base, "%s", word);
    *saturate = false;
    size_t n = strlen(base);
    if (n > 4 && !strcmp(base + n - 4, "_SAT")) {
        base[n - 4] = 0;
        *saturate = true;
    }
    for (size_t i = 0; i < sizeof opcodes / sizeof *opcodes; ++i)
        if (!strcmp(opcodes[i].name, base)) return &opcodes[i];
    return NULL;
}

/* SWZ's extended swizzle: four of 0, 1, x, y, z, w (or r g b a), each
   optionally negated. */
static void extended_swizzle(struct parser *p, const char *source, char *out, size_t size)
{
    char comps[4][96];
    for (int i = 0; i < 4; ++i) {
        if (i) expect(p, ",");
        bool negate = accept(p, "-");
        if (!negate) accept(p, "+");
        if (p->token.kind == TOK_NUMBER) {
            snprintf(comps[i], sizeof comps[i], "%s%s", negate ? "-" : "", p->token.number != 0 ? "1.0" : "0.0");
            next(p);
        } else {
            char word[8];
            expect_ident(p, word, sizeof word);
            int c = swizzle_component(word[0]);
            if (c < 0 || word[1]) fail(p, "bad extended swizzle '%s'", word);
            snprintf(comps[i], sizeof comps[i], "%s%s.%c", negate ? "-" : "", source, "xyzw"[c < 0 ? 0 : c]);
        }
    }
    snprintf(out, size, "vec4(%s, %s, %s, %s)", comps[0], comps[1], comps[2], comps[3]);
}

static void instruction(struct parser *p, const struct opcode *op, bool saturate)
{
    char variable[64], mask[8];
    int slot;
    char s[3][400];
    if (!strcmp(op->name, "KIL")) {
        source_operand(p, s[0], sizeof s[0]);
        emit(&p->body, "  if (any(lessThan(%s, vec4(0.0)))) discard;\n", s[0]);
        return;
    }
    destination(p, variable, sizeof variable, mask, &slot);
    for (int i = 0; i < op->sources; ++i) {
        expect(p, ",");
        source_operand(p, s[i], sizeof s[i]);
    }
    if (p->failed) return;
    char value[1400];
    const char *n = op->name;
    if (!strcmp(n, "ARL")) {
        emit(&p->body, "  %s.x = int(floor(%s.x));\n", variable, s[0]);
        return;
    }
    if (!strcmp(n, "SWZ")) {
        /* The source was parsed without a swizzle; the rest is the mask. */
        expect(p, ",");
        extended_swizzle(p, s[0], value, sizeof value);
    } else if (!strcmp(n, "TEX") || !strcmp(n, "TXP") || !strcmp(n, "TXB")) {
        int unit;
        char target[16];
        expect(p, ",");
        texture_operand(p, &unit, target, sizeof target);
        texture_sample(p, n, s[0], unit, target_code(p, target), value, sizeof value);
    } else if (!strcmp(n, "ABS")) snprintf(value, sizeof value, "abs(%s)", s[0]);
    else if (!strcmp(n, "ADD")) snprintf(value, sizeof value, "(%s + %s)", s[0], s[1]);
    else if (!strcmp(n, "SUB")) snprintf(value, sizeof value, "(%s - %s)", s[0], s[1]);
    else if (!strcmp(n, "MUL")) snprintf(value, sizeof value, "(%s * %s)", s[0], s[1]);
    else if (!strcmp(n, "MAD")) snprintf(value, sizeof value, "(%s * %s + %s)", s[0], s[1], s[2]);
    else if (!strcmp(n, "MOV")) snprintf(value, sizeof value, "%s", s[0]);
    else if (!strcmp(n, "MAX")) snprintf(value, sizeof value, "max(%s, %s)", s[0], s[1]);
    else if (!strcmp(n, "MIN")) snprintf(value, sizeof value, "min(%s, %s)", s[0], s[1]);
    else if (!strcmp(n, "DP3")) snprintf(value, sizeof value, "vec4(dot(%s.xyz, %s.xyz))", s[0], s[1]);
    else if (!strcmp(n, "DP4")) snprintf(value, sizeof value, "vec4(dot(%s, %s))", s[0], s[1]);
    else if (!strcmp(n, "DPH")) snprintf(value, sizeof value, "vec4(dot(%s.xyz, %s.xyz) + %s.w)", s[0], s[1], s[1]);
    else if (!strcmp(n, "DST")) snprintf(value, sizeof value, "vec4(1.0, %s.y * %s.y, %s.z, %s.w)", s[0], s[1], s[0], s[1]);
    else if (!strcmp(n, "EX2")) snprintf(value, sizeof value, "vec4(exp2(%s.x))", s[0]);
    else if (!strcmp(n, "LG2")) snprintf(value, sizeof value, "vec4(log2(%s.x))", s[0]);
    else if (!strcmp(n, "EXP")) snprintf(value, sizeof value, "glm_arb_exp(%s.x)", s[0]);
    else if (!strcmp(n, "LOG")) snprintf(value, sizeof value, "glm_arb_log(%s.x)", s[0]);
    else if (!strcmp(n, "FLR")) snprintf(value, sizeof value, "floor(%s)", s[0]);
    else if (!strcmp(n, "FRC")) snprintf(value, sizeof value, "fract(%s)", s[0]);
    else if (!strcmp(n, "LIT")) snprintf(value, sizeof value, "glm_arb_lit(%s)", s[0]);
    else if (!strcmp(n, "POW")) snprintf(value, sizeof value, "vec4(pow(%s.x, %s.x))", s[0], s[1]);
    else if (!strcmp(n, "RCP")) snprintf(value, sizeof value, "vec4(1.0 / %s.x)", s[0]);
    else if (!strcmp(n, "RSQ")) snprintf(value, sizeof value, "vec4(inversesqrt(abs(%s.x)))", s[0]);
    else if (!strcmp(n, "SGE")) snprintf(value, sizeof value, "vec4(greaterThanEqual(%s, %s))", s[0], s[1]);
    else if (!strcmp(n, "SLT")) snprintf(value, sizeof value, "vec4(lessThan(%s, %s))", s[0], s[1]);
    else if (!strcmp(n, "XPD")) snprintf(value, sizeof value, "vec4(cross(%s.xyz, %s.xyz), 1.0)", s[0], s[1]);
    else if (!strcmp(n, "CMP")) snprintf(value, sizeof value, "mix(%s, %s, lessThan(%s, vec4(0.0)))", s[2], s[1], s[0]);
    else if (!strcmp(n, "LRP")) snprintf(value, sizeof value, "mix(%s, %s, %s)", s[2], s[1], s[0]);
    else if (!strcmp(n, "COS")) snprintf(value, sizeof value, "vec4(cos(%s.x))", s[0]);
    else if (!strcmp(n, "SIN")) snprintf(value, sizeof value, "vec4(sin(%s.x))", s[0]);
    else if (!strcmp(n, "SCS")) snprintf(value, sizeof value, "vec4(cos(%s.x), sin(%s.x), 0.0, 1.0)", s[0], s[0]);
    else {
        fail(p, "unsupported instruction %s", n);
        return;
    }
    if (p->failed) return;
    /* Evaluate fully before writing: a source may alias the destination. */
    int id = p->temp_counter++;
    emit(&p->body, "  vec4 glm_v%d = %s%s%s;\n", id, saturate ? "clamp(" : "", value, saturate ? ", 0.0, 1.0)" : "");
    if (!strcmp(mask, "xyzw")) emit(&p->body, "  %s = glm_v%d;\n", variable, id);
    else emit(&p->body, "  %s.%s = glm_v%d.%s;\n", variable, mask, id, mask);
}

static void declaration_list(struct parser *p, enum name_kind kind)
{
    do {
        char ident[64];
        expect_ident(p, ident, sizeof ident);
        if (p->failed) return;
        if (!add_name(p, ident, kind)) return;
        if (kind == NAME_TEMP) emit(&p->prologue, "  vec4 %s = vec4(0.0);\n", ident);
        else emit(&p->prologue, "  ivec4 %s = ivec4(0);\n", ident);
    } while (accept(p, ","));
}

static void param_statement(struct parser *p)
{
    char ident[64];
    expect_ident(p, ident, sizeof ident);
    if (p->failed) return;
    bool array = false;
    int declared = -1;
    if (accept(p, "[")) {
        array = true;
        if (!is(p, "]")) declared = expect_integer(p);
        expect(p, "]");
    }
    expect(p, "=");
    if (p->failed) return;
    if (!array) {
        struct name *n = add_name(p, ident, NAME_PARAM);
        if (!n) return;
        char items[4][160];
        int count = parameter_item(p, items, 4, true, NULL, NULL);
        if (count != 1) fail(p, "'%s' needs a single vector binding", ident);
        snprintf(n->expression, sizeof n->expression, "%s", items[0]);
        /* Constant vectors are hoisted into locals. */
        if (!strncmp(items[0], "vec4(", 5)) {
            emit(&p->prologue, "  vec4 %s = %s;\n", ident, items[0]);
            snprintf(n->expression, sizeof n->expression, "%s", ident);
        } else {
            snprintf(n->expression, sizeof n->expression, "(%s)", items[0]);
        }
        return;
    }
    struct name *n = add_name(p, ident, NAME_PARAM_ARRAY);
    if (!n) return;
    expect(p, "{");
    char (*items)[160] = calloc(256, sizeof *items);
    int count = 0;
    const char *alias = NULL;
    int alias_base = 0, groups = 0;
    do {
        const char *a;
        int base;
        int got = parameter_item(p, items + count, 256 - count, false, &a, &base);
        if (groups == 0 && a) { alias = a; alias_base = base; }
        else alias = NULL;
        count += got;
        ++groups;
    } while (!p->failed && accept(p, ",") && count < 256);
    expect(p, "}");
    if (groups != 1) alias = NULL;
    if (declared >= 0 && declared != count) fail(p, "'%s' declared with %d elements but has %d", ident, declared, count);
    n->array_size = count;
    if (alias) {
        n->alias_array = alias;
        n->alias_base = alias_base;
    } else if (!p->failed) {
        emit(&p->prologue, "  vec4 %s[%d] = vec4[%d](", ident, count, count);
        for (int i = 0; i < count; ++i) emit(&p->prologue, "%s%s", i ? ", " : "", items[i]);
        emit(&p->prologue, ");\n");
    }
    free(items);
}

static void statement(struct parser *p)
{
    char word[64];
    expect_ident(p, word, sizeof word);
    if (p->failed) return;
    if (!strcmp(word, "OPTION")) {
        char option[64];
        expect_ident(p, option, sizeof option);
        if (!strcmp(option, "ARB_position_invariant") && p->vertex) p->position_invariant = true;
        else if (!strcmp(option, "ARB_fog_linear") && !p->vertex) p->fog_option = 1;
        else if (!strcmp(option, "ARB_fog_exp") && !p->vertex) p->fog_option = 2;
        else if (!strcmp(option, "ARB_fog_exp2") && !p->vertex) p->fog_option = 3;
        else if (strncmp(option, "ARB_precision_hint_", 19) && strcmp(option, "ARB_draw_buffers") &&
                 strncmp(option, "NV_", 3) && strcmp(option, "ATI_draw_buffers") && strcmp(option, "ARB_fragment_program_shadow"))
            fail(p, "unsupported option %s", option);
    } else if (!strcmp(word, "TEMP")) {
        declaration_list(p, NAME_TEMP);
    } else if (!strcmp(word, "ADDRESS")) {
        declaration_list(p, NAME_ADDRESS);
    } else if (!strcmp(word, "ATTRIB")) {
        char ident[64];
        expect_ident(p, ident, sizeof ident);
        expect(p, "=");
        struct name *n = add_name(p, ident, NAME_ATTRIB);
        if (!n) return;
        char e[160];
        attribute_binding(p, e);
        snprintf(n->expression, sizeof n->expression, "(%s)", e);
    } else if (!strcmp(word, "PARAM")) {
        param_statement(p);
    } else if (!strcmp(word, "OUTPUT")) {
        char ident[64];
        expect_ident(p, ident, sizeof ident);
        expect(p, "=");
        struct name *n = add_name(p, ident, NAME_OUTPUT);
        if (!n) return;
        int slot = result_binding(p);
        snprintf(n->expression, sizeof n->expression, "%d", slot);
    } else if (!strcmp(word, "ALIAS")) {
        char ident[64], target[64];
        expect_ident(p, ident, sizeof ident);
        expect(p, "=");
        expect_ident(p, target, sizeof target);
        struct name *t = find_name(p, target);
        if (!t) {
            fail(p, "undeclared '%s'", target);
            return;
        }
        struct name copy = *t;
        struct name *n = add_name(p, ident, t->kind);
        if (!n) return;
        *n = copy;
        snprintf(n->ident, sizeof n->ident, "%s", ident);
        if (copy.kind == NAME_TEMP || copy.kind == NAME_ADDRESS) {
            /* The alias names the same variable. */
            n->kind = NAME_ATTRIB;
            snprintf(n->expression, sizeof n->expression, "%s", target);
            if (copy.kind == NAME_TEMP) {
                /* Writable alias: redirect through a macro-like rename. */
                n->kind = NAME_TEMP;
                emit(&p->declarations, "#define %s %s\n", ident, target);
            }
        }
    } else {
        bool saturate;
        const struct opcode *op = find_opcode(word, &saturate);
        if (!op || (p->vertex ? !op->vertex : !op->fragment) || (saturate && p->vertex)) {
            fail(p, "unknown instruction '%s'", word);
            return;
        }
        instruction(p, op, saturate);
    }
    if (!p->failed) expect(p, ";");
}

/* ---- program ------------------------------------------------------------- */

static const char helpers[] =
    "vec4 glm_arb_lit(vec4 a) {\n"
    "  float d = max(a.x, 0.0), s = max(a.y, 0.0), e = clamp(a.w, -128.0, 128.0);\n"
    "  return vec4(1.0, d, a.x > 0.0 ? (s == 0.0 && e == 0.0 ? 1.0 : pow(s, e)) : 0.0, 1.0);\n}\n"
    "vec4 glm_arb_exp(float a) { float f = floor(a); return vec4(exp2(f), a - f, exp2(a), 1.0); }\n"
    "vec4 glm_arb_log(float a) {\n"
    "  float v = abs(a), e = floor(log2(v));\n"
    "  return vec4(e, v / exp2(e), log2(v), 1.0);\n}\n";

bool glm_arb_translate(const char *source, size_t length, bool vertex, struct glm_arb_translation *out)
{
    memset(out, 0, sizeof *out);
    struct parser *p = calloc(1, sizeof *p);
    p->source = source;
    p->length = length;
    p->vertex = vertex;
    p->env_max = p->local_max = -1;
    const char *header = vertex ? "!!ARBvp1.0" : "!!ARBfp1.0";
    size_t header_length = strlen(header);
    if (length < header_length || strncmp(source, header, header_length)) {
        p->failed = true;
        snprintf(p->error, sizeof p->error, "missing %s header", header);
        p->error_position = 0;
    } else {
        p->at = header_length;
        next(p);
        while (!p->failed && !is(p, "END")) {
            if (p->token.kind == TOK_END) {
                fail(p, "missing END");
                break;
            }
            statement(p);
        }
    }
    if (p->failed) {
        snprintf(out->error, sizeof out->error, "%s", p->error);
        out->error_position = (int)p->error_position;
        free(p->declarations.data);
        free(p->body.data);
        free(p->prologue.data);
        free(p);
        return false;
    }

    struct text t = {0};
    emit(&t, "#version 120\n");
    int env_count = p->env_dynamic ? 256 : p->env_max + 1;
    int local_count = p->local_dynamic ? 256 : p->local_max + 1;
    if (env_count < 1) env_count = 1;
    if (local_count < 1) local_count = 1;
    emit(&t, "layout(std140, set = 1, binding = 13) uniform GLMARB { vec4 glm_arb_env[%d]; vec4 glm_arb_local[%d]; };\n", env_count, local_count);
    out->env_count = env_count;
    out->local_count = local_count;
    for (int i = 0; i < 16; ++i)
        if (p->attribs_used & (1u << i)) emit(&t, "attribute vec4 arb_attrib%d;\n", i);
    out->attribs_used = p->attribs_used;
    emit(&t, "%s", helpers);
    if (p->declarations.data) emit(&t, "%s", p->declarations.data);
    for (int slot = 0; slot < RESULT_COUNT; ++slot) {
        bool written = (p->results_written & (1u << slot)) != 0;
        if (written) emit(&t, "vec4 %s = vec4(0.0, 0.0, 0.0, 1.0);\n", result_variable(slot));
    }
    emit(&t, "void main() {\n");
    if (p->prologue.data) emit(&t, "%s", p->prologue.data);
    if (p->body.data) emit(&t, "%s", p->body.data);
    uint32_t w = p->results_written;
    if (vertex) {
        if (p->position_invariant) emit(&t, "  gl_Position = ftransform();\n");
        else if (w & (1u << RESULT_POSITION)) emit(&t, "  gl_Position = %s;\n", result_variable(RESULT_POSITION));
        if (w & (1u << RESULT_COLOR0)) emit(&t, "  gl_FrontColor = %s;\n", result_variable(RESULT_COLOR0));
        if (w & (1u << RESULT_COLOR1)) emit(&t, "  gl_FrontSecondaryColor = %s;\n", result_variable(RESULT_COLOR1));
        if (w & (1u << RESULT_BACK0)) emit(&t, "  gl_BackColor = %s;\n", result_variable(RESULT_BACK0));
        if (w & (1u << RESULT_BACK1)) emit(&t, "  gl_BackSecondaryColor = %s;\n", result_variable(RESULT_BACK1));
        if (w & (1u << RESULT_FOG)) emit(&t, "  gl_FogFragCoord = %s.x;\n", result_variable(RESULT_FOG));
        if (w & (1u << RESULT_POINT)) emit(&t, "  gl_PointSize = %s.x;\n", result_variable(RESULT_POINT));
        for (int u = 0; u < 8; ++u)
            if (p->texcoords_written & (1 << u)) emit(&t, "  gl_TexCoord[%d] = %s;\n", u, result_variable(RESULT_TEX0 + u));
    } else {
        if (p->fog_option && p->draw_buffers_written) {
            const char *r = result_variable(RESULT_DRAW0);
            switch (p->fog_option) {
            case 1: emit(&t, "  float glm_fog = (gl_Fog.end - abs(gl_FogFragCoord)) * gl_Fog.scale;\n"); break;
            case 2: emit(&t, "  float glm_fog = exp(-gl_Fog.density * abs(gl_FogFragCoord));\n"); break;
            case 3: emit(&t, "  float glm_fog = exp(-(gl_Fog.density * gl_FogFragCoord) * (gl_Fog.density * gl_FogFragCoord));\n"); break;
            }
            emit(&t, "  %s.rgb = mix(gl_Fog.color.rgb, %s.rgb, clamp(glm_fog, 0.0, 1.0));\n", r, r);
        }
        if (p->draw_buffers_written == 1) emit(&t, "  gl_FragColor = %s;\n", result_variable(RESULT_DRAW0));
        else
            for (int i = 0; i < p->draw_buffers_written; ++i)
                emit(&t, "  gl_FragData[%d] = %s;\n", i, result_variable(RESULT_DRAW0 + i));
        if (w & (1u << RESULT_DEPTH)) emit(&t, "  gl_FragDepth = %s.z;\n", result_variable(RESULT_DEPTH));
    }
    emit(&t, "}\n");
    out->glsl = t.data;
    free(p->declarations.data);
    free(p->body.data);
    free(p->prologue.data);
    free(p);
    return true;
}
