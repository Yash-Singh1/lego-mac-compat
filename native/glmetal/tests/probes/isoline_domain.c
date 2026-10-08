#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The maximum is two patches, 64 lines, 64 segments, two endpoints. */
enum { MAX_RECORDS = 2 * 64 * 64 * 2 };
struct record { float u, v, w; GLint patch; };
struct domain_case { const char *name; float lines, segments; int spacing, patches; };

static int compare_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static int segments(float level, int spacing)
{
    float low = spacing == 1 ? 2 : 1;
    float high = spacing == 2 ? 63 : 64;
    int n = (int)ceilf(fminf(fmaxf(level, low), high));
    if (spacing == 1 && (n & 1)) ++n;
    if (spacing == 2 && !(n & 1)) ++n;
    return n;
}

static GLuint shader(GLenum stage, const char *source)
{
    GLuint result = glCreateShader(stage);
    glShaderSource(result, 1, &source, NULL);
    glCompileShader(result);
    GLint ok = 0;
    glGetShaderiv(result, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = {0};
        glGetShaderInfoLog(result, sizeof log, NULL, log);
        glc_fail("Isoline shader compilation: %s", log);
    }
    return result;
}

static void check_domain(const struct domain_case *c, int points,
                         const struct record *records, GLuint count)
{
    int discarded = !(c->lines > 0) || !(c->segments > 0);
    int lines = discarded ? 0 : segments(c->lines, 0);
    int edges = discarded ? 0 : segments(c->segments, c->spacing);
    GLuint expected = (GLuint)(c->patches * lines * (points ? edges + 1 : edges));
    if (count != expected)
        glc_fail("%s %s count %u expected %u", c->name, points ? "points" : "lines", count, expected);
    GLuint vertices = count * (points ? 1u : 2u);
    for (GLuint i = 0; i < vertices; ++i) {
        const struct record *r = records + i;
        fprintf(stderr, "  vertex %u patch=%d coord=(%a,%a,%a)\n", i, r->patch, r->u, r->v, r->w);
        if (r->patch < 0 || r->patch >= c->patches || !isfinite(r->u) || !isfinite(r->v) ||
            r->u < 0 || r->u > 1 || r->v < 0 || r->v >= 1 || r->w != 0)
            glc_fail("%s invalid domain membership at vertex %u", c->name, i);
        const float coordinates[] = {r->u, r->v, r->w};
        for (int j = 0; j < 3; ++j) {
            volatile float complement = 1.0f - coordinates[j];
            volatile float restored = 1.0f - complement;
            if ((double)complement != 1.0 - (double)coordinates[j] || restored != coordinates[j])
                glc_fail("%s coordinate exact-complement invariant failed", c->name);
        }
    }
    for (int patch = 0; patch < c->patches; ++patch) {
        float rows[64];
        int row_count = 0;
        for (GLuint i = 0; i < vertices; ++i) {
            if (records[i].patch != patch) continue;
            int found = 0;
            for (int j = 0; j < row_count; ++j) found |= rows[j] == records[i].v;
            if (!found) {
                if (row_count == 64) glc_fail("%s too many isolines", c->name);
                rows[row_count++] = records[i].v;
            }
        }
        if (row_count != lines) glc_fail("%s incorrect distinct isoline count", c->name);
        qsort(rows, (size_t)row_count, sizeof(float), compare_float);
        if (row_count && rows[0] != 0) glc_fail("%s first isoline is not at zero", c->name);
        for (int line = 0; line < lines; ++line) {
            double ideal_v = (double)line / lines;
            fprintf(stderr, "  row precision patch=%d line=%d measured=%a ideal=%a error=%a\n",
                    patch, line, rows[line], ideal_v, rows[line] - ideal_v);
            float positions[128];
            int n = 0;
            for (GLuint i = 0; i < vertices; ++i) {
                const struct record *r = records + i;
                if (r->patch == patch && r->v == rows[line]) {
                    if (n >= 128) glc_fail("%s excessive line vertices", c->name);
                    positions[n++] = r->u;
                }
            }
            int expected_n = points ? edges + 1 : edges * 2;
            if (n != expected_n) glc_fail("%s missing line vertices", c->name);
            qsort(positions, (size_t)n, sizeof(float), compare_float);
            float unique[65];
            int distinct = 0;
            for (int i = 0; i < n; ++i) {
                if (!i || positions[i] != positions[i - 1]) {
                    if (distinct >= 65) glc_fail("%s too many distinct coordinates", c->name);
                    unique[distinct++] = positions[i];
                }
            }
            if (distinct != edges + 1 || unique[0] != 0 || unique[edges] != 1)
                glc_fail("%s invalid distinct vertices/endpoints", c->name);
            if (!c->spacing) {
                for (int i = 0; i <= edges; ++i)
                    fprintf(stderr, "  column precision patch=%d line=%d vertex=%d measured=%a ideal=%a error=%a\n",
                            patch, line, i, unique[i], (double)i / edges, unique[i] - (double)i / edges);
            }
            if (!points) {
                int used[64] = {0};
                for (GLuint i = 0; i < vertices; i += 2) {
                    const struct record *a = records + i, *b = a + 1;
                    if (a->patch != b->patch || a->v != b->v)
                        glc_fail("%s segment crosses patches/isolines", c->name);
                    if (a->patch != patch || a->v != rows[line]) continue;
                    float low = fminf(a->u, b->u), high = fmaxf(a->u, b->u);
                    int edge = -1;
                    for (int j = 0; j < edges; ++j)
                        if (low == unique[j] && high == unique[j + 1]) edge = j;
                    if (edge < 0 || used[edge]++) glc_fail("%s invalid/duplicate connectivity", c->name);
                    fprintf(stderr, "  edge patch=%d line=%d %a -> %a\n", patch, line, a->u, b->u);
                }
                for (int i = 0; i < edges; ++i)
                    if (!used[i]) glc_fail("%s missing segment", c->name);
            }
        }
    }
}

GLC_CASE(driver_isoline_domain, .profile = GLC_CORE)
{
    struct domain_case cases[] = {
        {"one", 1, 1, 0, 1}, {"two", 2, 2, 0, 1}, {"three", 3, 3, 0, 1},
        {"line1_segment3", 1, 3, 0, 1}, {"line3_segment1", 3, 1, 0, 1},
        {"fractional_equal", 2.25f, 2.25f, 0, 1},
        {"fractional_even", 2.25f, 2.25f, 1, 1},
        {"fractional_odd", 2.25f, 2.25f, 2, 1},
        {"even_minimum", .5f, .5f, 1, 1}, {"odd_minimum", .5f, .5f, 2, 1},
        {"zero_lines", 0, 3, 0, 1}, {"negative_segments", 2, -1, 0, 1},
        {"nan_lines", NAN, 3, 0, 1}, {"nan_segments", 2, NAN, 0, 1},
        {"maximum", 64, 64, 0, 1}, {"odd_maximum", 64, 63, 2, 1},
        {"multiple_patches", 2, 3, 0, 2}
    };
    const char *vs = "#version 410 core\nvoid main(){gl_Position=vec4(0,0,0,1);}";
    const char *tcs = "#version 410 core\nlayout(vertices=1)out;uniform vec2 levels;"
        "void main(){gl_out[0].gl_Position=gl_in[0].gl_Position;"
        "gl_TessLevelOuter[0]=levels.x;gl_TessLevelOuter[1]=levels.y;"
        "gl_TessLevelOuter[2]=1;gl_TessLevelOuter[3]=1;"
        "gl_TessLevelInner[0]=1;gl_TessLevelInner[1]=1;}";
    const char *spacing[] = {"equal_spacing", "fractional_even_spacing", "fractional_odd_spacing"};
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &maximum);
    fprintf(stderr, "Advertised maximum tessellation level=%d\n", maximum);
    if (maximum < 64) glc_fail("Provider tessellation limit is below the GL4.1 minimum");
    /* Upper-clamp cases are safe only when the advertised maximum is 64. */
    struct domain_case upper[] = {{"upper_clamp", 65, 65, 0, 1},
                                 {"odd_upper_clamp", 65, 65, 2, 1}};
    size_t regular_count = sizeof cases / sizeof cases[0];
    size_t case_count = regular_count + (maximum == 64 ? 2 : 0);
    GLuint vao, buffer, query, generated_query;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, MAX_RECORDS * sizeof(struct record), NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glGenQueries(1, &query);
    glGenQueries(1, &generated_query);
    glPatchParameteri(GL_PATCH_VERTICES, 1);
    glEnable(GL_RASTERIZER_DISCARD);
    struct record *records = malloc(MAX_RECORDS * sizeof *records);
    if (!records) glc_fail("Isoline diagnostic allocation failed");
    for (size_t c = 0; c < case_count; ++c) {
        const struct domain_case *current = c < regular_count ? cases + c : upper + c - regular_count;
        for (int points = 0; points < 2; ++points) {
            char tes[1024];
            snprintf(tes, sizeof tes, "#version 410 core\nlayout(isolines,%s%s)in;"
                "out vec3 coord;flat out int patch_id;void main(){coord=gl_TessCoord;"
                "patch_id=gl_PrimitiveID;gl_Position=vec4(gl_TessCoord.xy,0,1);gl_PointSize=1;}",
                spacing[current->spacing], points ? ",point_mode" : "");
            GLuint stages[] = {shader(GL_VERTEX_SHADER, vs), shader(GL_TESS_CONTROL_SHADER, tcs),
                               shader(GL_TESS_EVALUATION_SHADER, tes)};
            GLuint program = glCreateProgram();
            for (int i = 0; i < 3; ++i) glAttachShader(program, stages[i]);
            const char *varyings[] = {"coord", "patch_id"};
            glTransformFeedbackVaryings(program, 2, varyings, GL_INTERLEAVED_ATTRIBS);
            glLinkProgram(program);
            GLint linked = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (!linked) {
                char log[4096] = {0};
                glGetProgramInfoLog(program, sizeof log, NULL, log);
                glc_fail("Isoline link failed: %s", log);
            }
            glUseProgram(program);
            glUniform2f(glGetUniformLocation(program, "levels"), current->lines, current->segments);
            glBeginQuery(GL_PRIMITIVES_GENERATED, generated_query);
            glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, query);
            glBeginTransformFeedback(points ? GL_POINTS : GL_LINES);
            glDrawArrays(GL_PATCHES, 0, current->patches);
            glEndTransformFeedback();
            glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
            glEndQuery(GL_PRIMITIVES_GENERATED);
            GLuint count = 0, generated = 0;
            glGetQueryObjectuiv(query, GL_QUERY_RESULT, &count);
            glGetQueryObjectuiv(generated_query, GL_QUERY_RESULT, &generated);
            if (generated != count) glc_fail("Isoline generated=%u captured=%u", generated, count);
            GLuint vertices = count * (points ? 1u : 2u);
            if (vertices > MAX_RECORDS) glc_fail("Isoline feedback count exceeds capacity");
            if (vertices) glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, vertices * sizeof *records, records);
            fprintf(stderr, "isoline %s %s levels=(%a,%a) primitives=%u vertices=%u\n",
                current->name, points ? "points" : "lines", current->lines, current->segments, count, vertices);
            GLenum error = glGetError();
            if (error) glc_fail("Isoline diagnostic GL error %x", error);
            check_domain(current, points, records, count);
            glUseProgram(0);
            glDeleteProgram(program);
            for (int i = 0; i < 3; ++i) glDeleteShader(stages[i]);
            usleep(50000);
        }
    }
    free(records);
    glDisable(GL_RASTERIZER_DISCARD);
    glDeleteQueries(1, &query);
    glDeleteQueries(1, &generated_query);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
