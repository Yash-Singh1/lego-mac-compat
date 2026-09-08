#include "gl_shader_bridge.h"
#include "tfu_timing.h"
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>

static int tfu_compat;
void gl_shader_bridge32_enable_tfu_compat(void) { tfu_compat = 1; }

/* Aspyr translates D3D9 bytecode to GLSL. D3D9 RSQ takes abs(src), and CMP
   selects a value without doing arithmetic on its unselected input. The
   original translation can therefore spread NaNs from an unused normal-map
   decode or light through an otherwise valid high-detail material.
   https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/rsq---ps
   https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/cmp---ps */
static void repair_tfu_shader(GLuint shader)
{
    if (!tfu_compat || getenv("LP32_KEEP_TFU_GLSL")) return;
    GLint length = 0;
    glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
    if (length <= 0 || length > 16 * 1024 * 1024) return;
    char *source = malloc((size_t)length);
    /* Leave room for the scalar D3D LOG helper as well as replacements. */
    char *fixed = malloc((size_t)length * 4 + 1024);
    if (!source || !fixed) { free(source); free(fixed); return; }
    glGetShaderSource(shader, length, NULL, source);
    if (!strstr(source, "// D3DPS_VERSION(")) { free(source); free(fixed); return; }
    const char *old_vector = "return mix(src2, src1, vec4(greaterThanEqual(src0, vec4(0.0))));";
    const char *new_vector = "return vec4(src0.x >= 0.0 ? src1.x : src2.x, src0.y >= 0.0 ? src1.y : src2.y, src0.z >= 0.0 ? src1.z : src2.z, src0.w >= 0.0 ? src1.w : src2.w);";
    const char *old_scalar = "return mix(src2, src1, float(src0 >= 0.0));";
    const char *new_scalar = "return src0 >= 0.0 ? src1 : src2;";
    const char *cursor = source;
    char *out = fixed;
    unsigned rsq = 0, cmp = 0, logarithm = 0, nrm = 0;
    int keep_cmp = getenv("LP32_KEEP_TFU_GLSL_CMP") != NULL;
    int keep_rsq = getenv("LP32_KEEP_TFU_GLSL_RSQ") != NULL;
    int repair_log = !getenv("LP32_KEEP_TFU_GLSL_LOG") &&
        !strstr(source, "lp32_d3d_log") &&
        strstr(source, "log2(") && strstr(source, "void main(");
    int repair_nrm = !getenv("LP32_KEEP_TFU_GLSL_NRM") &&
        !strstr(source, "lp32_d3d_nrm") &&
        strstr(source, "normalize(vec3(") && strstr(source, "void main(");
    /* D3D9 LOG specifies -FLT_MAX for zero, unlike GLSL's undefined log2(0).
       Preserve finite results (including negative inputs through abs).
       https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/log---ps */
    const char *log_helper =
        "float lp32_d3d_log(float x) { return x == 0.0 ? "
        "-3.402823466e+38 : log2(abs(x)); }\n";
    /* NRM uses FLT_MAX when the squared length is zero. In particular a
       missing mesh normal/tangent remains zero instead of contaminating
       lighting with normalize(vec3(0)) NaNs. Keep the ordinary GLSL path
       for nonzero vectors, including its existing rounding behavior.
       https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/nrm---ps */
    const char *nrm_helper =
        "vec3 lp32_d3d_nrm(vec3 v) { return dot(v, v) == 0.0 ? "
        "v * 3.402823466e+38 : normalize(v); }\n";
    while (*cursor) {
        if ((repair_log || repair_nrm) && !strncmp(cursor, "void main(", 10)) {
            if (repair_log) { memcpy(out, log_helper, strlen(log_helper)); out += strlen(log_helper); }
            if (repair_nrm) { memcpy(out, nrm_helper, strlen(nrm_helper)); out += strlen(nrm_helper); }
            memcpy(out, cursor, 10); out += 10; cursor += 10;
        } else if (repair_nrm && !strncmp(cursor, "normalize(vec3(", 15)) {
            const char *replacement = "lp32_d3d_nrm(vec3(";
            memcpy(out, replacement, strlen(replacement)); out += strlen(replacement);
            cursor += 15; ++nrm;
        } else if (repair_log && !strncmp(cursor, "log2(", 5)) {
            const char *replacement = "lp32_d3d_log(";
            memcpy(out, replacement, strlen(replacement)); out += strlen(replacement);
            cursor += 5; ++logarithm;
        } else if (!keep_cmp && !strncmp(cursor, old_vector, strlen(old_vector))) {
            memcpy(out, new_vector, strlen(new_vector)); out += strlen(new_vector);
            cursor += strlen(old_vector); ++cmp;
        } else if (!keep_cmp && !strncmp(cursor, old_scalar, strlen(old_scalar))) {
            memcpy(out, new_scalar, strlen(new_scalar)); out += strlen(new_scalar);
            cursor += strlen(old_scalar); ++cmp;
        } else if (!keep_rsq && !strncmp(cursor, "inversesqrt(", 12)) {
            const char *argument = cursor + 12, *end = argument;
            unsigned depth = 1;
            for (; *end && depth; ++end) {
                if (*end == '(') ++depth;
                else if (*end == ')') --depth;
            }
            if (depth) { *out++ = *cursor++; continue; }
            const char *prefix = "inversesqrt(abs(";
            size_t argument_length = (size_t)(end - argument - 1);
            memcpy(out, prefix, strlen(prefix)); out += strlen(prefix);
            memcpy(out, argument, argument_length); out += argument_length;
            *out++ = ')'; *out++ = ')'; cursor = end; ++rsq;
        } else *out++ = *cursor++;
    }
    *out = 0;
    if (rsq || cmp || logarithm || nrm) {
        const GLchar *text = fixed;
        glShaderSource(shader, 1, &text, NULL);
        if (getenv("LP32_DUMP_GLSL"))
            fprintf(stderr, "compat32: TFU GLSL shader %u repaired rsq=%u cmp=%u log=%u nrm=%u\n", shader, rsq, cmp, logarithm, nrm);
    }
    free(source); free(fixed);
}

/* Opt-in diagnostics for games using GLSL instead of ARB assembly. Read the
   driver's stored source so explicit lengths and multiple strings retain
   exactly the same meaning as the original glShaderSource call. */
static void dump_shader(GLuint shader, const char *directory)
{
    if (!directory || !*directory) return;
    GLint length = 0, type = 0;
    glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
    glGetShaderiv(shader, GL_SHADER_TYPE, &type);
    if (length <= 0 || length > 16 * 1024 * 1024) return;
    char *source = malloc((size_t)length);
    if (!source) return;
    GLsizei written = 0;
    glGetShaderSource(shader, length, &written, source);
    mkdir(directory, 0700);
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%u-%s.glsl", directory, shader,
                     type == GL_VERTEX_SHADER ? "vertex" : "fragment");
    if (n > 0 && (size_t)n < sizeof(path)) {
        FILE *file = fopen(path, "wb");
        if (file) { fwrite(source, 1, (size_t)written, file); fclose(file); }
    }
    free(source);
}

void gl_shader_bridge32_dump_program(uint32_t program, const char *directory)
{
    GLuint shaders[16]; GLsizei count = 0;
    glGetAttachedShaders(program, 16, &count, shaders);
    for (GLsizei i = 0; i < count; ++i) dump_shader(shaders[i], directory);
}

static void trace_shader_status(GLuint object, int program)
{
    int dump = getenv("LP32_DUMP_GLSL") != NULL;
    if (!tfu_compat && !dump) return;
    GLint success = 0;
    char log[4096] = {0};
    if (program) glGetProgramiv(object, GL_LINK_STATUS, &success);
    else glGetShaderiv(object, GL_COMPILE_STATUS, &success);
    if (!success) {
        if (program) glGetProgramInfoLog(object, sizeof(log), NULL, log);
        else glGetShaderInfoLog(object, sizeof(log), NULL, log);
        fprintf(stderr, "compat32: GLSL %s %u failed: %s\n", program ? "program" : "shader", object, log);
    }
    if (program && dump) {
        GLuint shaders[8]; GLsizei count = 0;
        glGetAttachedShaders(object, 8, &count, shaders);
        fprintf(stderr, "compat32: GLSL program %u link=%d shaders=", object, success);
        for (GLsizei i = 0; i < count; ++i) fprintf(stderr, "%s%u", i ? "," : "", shaders[i]);
        fputc('\n', stderr);
    }
}

/* Apple's ARB shader handles are host pointers. Use the equivalent core API
   throughout so both the ARB and core entry points share 32-bit GLuint names,
   as Source expects when it mixes glCreateShaderObjectARB and glDeleteShader. */
int gl_shader_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *r)
{
    if (*name == '_') ++name;
#define IS(s) (!strcmp(name, s))
#define PTR(i) ((void *)(uintptr_t)a[i])
    *r = 0;
    if (IS("glCreateShaderObjectARB") || IS("glCreateShader")) *r = glCreateShader(a[0]);
    else if (IS("glCreateProgramObjectARB") || IS("glCreateProgram")) *r = glCreateProgram();
    else if (IS("glCompileShaderARB") || IS("glCompileShader")) {
        uint64_t start = tfu_compat ? tfu_time_ns() : 0;
        glCompileShader(a[0]);
        if (tfu_compat) tfu_log_slow("shader-compile", a[0], start, tfu_time_ns());
        trace_shader_status(a[0], 0);
    }
    else if (IS("glAttachObjectARB") || IS("glAttachShader")) glAttachShader(a[0], a[1]);
    else if (IS("glDetachObjectARB") || IS("glDetachShader")) glDetachShader(a[0], a[1]);
    else if (IS("glLinkProgramARB") || IS("glLinkProgram")) {
        uint64_t start = tfu_compat ? tfu_time_ns() : 0;
        glLinkProgram(a[0]);
        if (tfu_compat) tfu_log_slow("program-link", a[0], start, tfu_time_ns());
        trace_shader_status(a[0], 1);
    }
    else if (IS("glUseProgramObjectARB") || IS("glUseProgram")) glUseProgram(a[0]);
    else if (IS("glValidateProgramARB") || IS("glValidateProgram")) glValidateProgram(a[0]);
    else if (IS("glDeleteShader")) glDeleteShader(a[0]);
    else if (IS("glDeleteProgram")) glDeleteProgram(a[0]);
    else if (IS("glDeleteObjectARB")) {
        if (glIsShader(a[0])) glDeleteShader(a[0]); else glDeleteProgram(a[0]);
    } else if (IS("glShaderSourceARB") || IS("glShaderSource")) {
        GLsizei count = (GLsizei)a[1];
        if (count <= 0) { glShaderSource(a[0], count, NULL, PTR(3)); return 1; }
        const uint32_t *guest = PTR(2);
        const GLchar **strings = calloc((size_t)count, sizeof(*strings));
        if (!strings) return 0;
        for (GLsizei i = 0; i < count; ++i) strings[i] = (const void *)(uintptr_t)guest[i];
        glShaderSource(a[0], count, strings, PTR(3));
        repair_tfu_shader(a[0]);
        dump_shader(a[0], getenv("LP32_DUMP_GLSL"));
        free(strings);
    } else if (IS("glGetObjectParameterivARB")) {
        GLint *out = PTR(2);
        if (a[1] == GL_OBJECT_TYPE_ARB) *out = glIsShader(a[0]) ? GL_SHADER_OBJECT_ARB : GL_PROGRAM_OBJECT_ARB;
        else if (glIsShader(a[0])) glGetShaderiv(a[0], a[1], out);
        else glGetProgramiv(a[0], a[1], out);
    } else if (IS("glGetShaderiv")) glGetShaderiv(a[0], a[1], PTR(2));
    else if (IS("glGetProgramiv")) glGetProgramiv(a[0], a[1], PTR(2));
    else if (IS("glGetInfoLogARB")) {
        if (glIsShader(a[0])) glGetShaderInfoLog(a[0], a[1], PTR(2), PTR(3));
        else glGetProgramInfoLog(a[0], a[1], PTR(2), PTR(3));
    } else if (IS("glGetShaderInfoLog")) glGetShaderInfoLog(a[0], a[1], PTR(2), PTR(3));
    else if (IS("glGetProgramInfoLog")) glGetProgramInfoLog(a[0], a[1], PTR(2), PTR(3));
    else if (IS("glGetUniformLocationARB") || IS("glGetUniformLocation")) *r = (uint32_t)glGetUniformLocation(a[0], PTR(1));
    else if (IS("glGetAttribLocationARB") || IS("glGetAttribLocation")) *r = (uint32_t)glGetAttribLocation(a[0], PTR(1));
    else if (IS("glBindAttribLocationARB") || IS("glBindAttribLocation")) glBindAttribLocation(a[0], a[1], PTR(2));
    else if (IS("glUniform1iARB") || IS("glUniform1i")) glUniform1i(a[0], a[1]);
    else if (IS("glUniform1fARB") || IS("glUniform1f")) {
        GLfloat value; memcpy(&value, a + 1, sizeof(value)); glUniform1f(a[0], value);
    } else if (IS("glUniform4fvARB") || IS("glUniform4fv")) glUniform4fv(a[0], a[1], PTR(2));
    else return 0;
    return 1;
#undef IS
#undef PTR
}
