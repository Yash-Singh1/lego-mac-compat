/* Legacy profile: vertex arrays and buffers, ARB programs, GLSL 1.20,
   framebuffer objects, pixel operations and Apple extensions. */
#include "glc_gl_legacy.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "glcompare.h"

struct vertex {
    float position[3];
    unsigned char color[4];
    float texcoord[2];
};

static const struct vertex fan_vertices[] = {
    {{0, 0, 0}, {255, 255, 255, 255}, {0.5f, 0.5f}},
    {{-0.9f, -0.8f, 0}, {255, 0, 0, 255}, {0, 0}},
    {{0.85f, -0.9f, 0}, {0, 255, 0, 200}, {1, 0}},
    {{0.9f, 0.7f, 0}, {0, 0, 255, 150}, {1, 1}},
    {{-0.8f, 0.9f, 0}, {255, 255, 0, 100}, {0, 1}},
    {{-0.95f, 0.1f, 0}, {0, 255, 255, 255}, {0, 0.5f}},
};
static const unsigned short fan_indices[] = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5, 0, 5, 1};

static void set_client_arrays(const struct vertex *base)
{
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(struct vertex), &base->position);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(struct vertex), &base->color);
}

static const char *const array_sources[] = {"client_arrays", "vbo", "vbo_ibo", "draw_range_elements", "bgra_colors", "generic_attribs"};
GLC_CASE_VARIANTS(vertex_arrays, array_sources, .profile = GLC_LEGACY)
{
    glClear(GL_COLOR_BUFFER_BIT);
    glShadeModel(GL_SMOOTH);
    GLuint buffers[2];
    glGenBuffers(2, buffers);
    switch (glc_variant) {
    case 0:
        set_client_arrays(fan_vertices);
        glDrawElements(GL_TRIANGLES, 15, GL_UNSIGNED_SHORT, fan_indices);
        break;
    case 1:
        glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
        glBufferData(GL_ARRAY_BUFFER, sizeof fan_vertices, fan_vertices, GL_STATIC_DRAW);
        set_client_arrays(NULL);
        glDrawElements(GL_TRIANGLES, 15, GL_UNSIGNED_SHORT, fan_indices);
        break;
    case 2:
    case 3:
        glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
        glBufferData(GL_ARRAY_BUFFER, sizeof fan_vertices, fan_vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof fan_indices, fan_indices, GL_STATIC_DRAW);
        set_client_arrays(NULL);
        if (glc_variant == 2) glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_SHORT, (void *)(uintptr_t)6);
        else glDrawRangeElements(GL_TRIANGLES, 0, 5, 15, GL_UNSIGNED_SHORT, NULL);
        break;
    case 4:
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(struct vertex), fan_vertices[0].position);
        glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, sizeof(struct vertex), fan_vertices[0].color);
        glDrawArrays(GL_TRIANGLE_FAN, 1, 5);
        break;
    case 5: {
        /* Generic attributes 0 (position) and 3 (colour) through ARB
           programs, the way D3D-ported games feed their vertex programs. */
        glc_arb_program(GL_VERTEX_PROGRAM_ARB,
                        "!!ARBvp1.0\nMOV result.position, vertex.attrib[0];\n"
                        "MOV result.color, vertex.attrib[3];\nEND\n");
        glc_arb_program(GL_FRAGMENT_PROGRAM_ARB, "!!ARBfp1.0\nMOV result.color, fragment.color;\nEND\n");
        glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
        glBufferData(GL_ARRAY_BUFFER, sizeof fan_vertices, fan_vertices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void *)0);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(struct vertex), (void *)12);
        glDrawElements(GL_TRIANGLES, 15, GL_UNSIGNED_SHORT, fan_indices);
        break;
    }
    }
}

static const char *const index_types[] = {"ubyte", "ushort", "uint"};
GLC_CASE_VARIANTS(index_type, index_types, .profile = GLC_LEGACY)
{
    static const GLenum types[] = {GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT, GL_UNSIGNED_INT};
    unsigned char bytes[15];
    unsigned short shorts[15];
    unsigned ints[15];
    for (int i = 0; i < 15; ++i) bytes[i] = (unsigned char)fan_indices[i], shorts[i] = fan_indices[i], ints[i] = fan_indices[i];
    const void *lists[] = {bytes, shorts, ints};
    glClear(GL_COLOR_BUFFER_BIT);
    set_client_arrays(fan_vertices);
    glDrawElements(GL_TRIANGLES, 15, types[glc_variant], lists[glc_variant]);
}

GLC_CASE(buffer_sub_data_and_map, .profile = GLC_LEGACY)
{
    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof fan_vertices, NULL, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof fan_vertices, fan_vertices);
    struct vertex *mapped = glMapBuffer(GL_ARRAY_BUFFER, GL_READ_WRITE);
    if (!mapped) glc_fail("glMapBuffer returned NULL");
    mapped[3].color[0] = 255;
    mapped[3].position[1] = 0.95f;
    glUnmapBuffer(GL_ARRAY_BUFFER);
    glClear(GL_COLOR_BUFFER_BIT);
    set_client_arrays(NULL);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 6);
}

GLC_CASE(apple_flush_buffer_range, .profile = GLC_LEGACY)
{
    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof fan_vertices, fan_vertices, GL_STREAM_DRAW);
    glBufferParameteriAPPLE(GL_ARRAY_BUFFER, GL_BUFFER_FLUSHING_UNMAP_APPLE, GL_FALSE);
    glBufferParameteriAPPLE(GL_ARRAY_BUFFER, GL_BUFFER_SERIALIZED_MODIFY_APPLE, GL_FALSE);
    glClear(GL_COLOR_BUFFER_BIT);
    set_client_arrays(NULL);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 6);
    glFinish();
    struct vertex *mapped = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
    if (!mapped) glc_fail("glMapBuffer returned NULL");
    for (int i = 1; i < 6; ++i) mapped[i].position[0] *= 0.5f;
    glFlushMappedBufferRangeAPPLE(GL_ARRAY_BUFFER, 0, sizeof fan_vertices);
    glUnmapBuffer(GL_ARRAY_BUFFER);
    glDrawArrays(GL_LINE_LOOP, 1, 5);
}

GLC_CASE(apple_vertex_array_object, .profile = GLC_LEGACY)
{
    GLuint arrays[2];
    glGenVertexArraysAPPLE(2, arrays);
    glBindVertexArrayAPPLE(arrays[0]);
    set_client_arrays(fan_vertices);
    glBindVertexArrayAPPLE(arrays[1]);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(struct vertex), fan_vertices[0].position);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(0.3f, 0.3f, 0.3f);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 6);
    glBindVertexArrayAPPLE(arrays[0]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

GLC_CASE(apple_fence, .profile = GLC_LEGACY)
{
    GLuint fence;
    glGenFencesAPPLE(1, &fence);
    glClearColor(0.2f, 0.4f, 0.6f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glSetFenceAPPLE(fence);
    glFinishFenceAPPLE(fence);
    if (!glTestFenceAPPLE(fence)) glc_fail("fence not signalled after glFinishFenceAPPLE");
    glColor3f(1, 1, 1);
    glRectf(-0.5f, -0.5f, 0.5f, 0.5f);
}

/* ---- ARB programs -------------------------------------------------------- */

static const char *const arb_fp_names[] = {
    "mov", "mad_const", "tex", "txp", "kil", "lrp_cmp", "dp3_pow", "rsq_rcp", "precision_option", "fog_linear", "local_env_params",
};
static const char *const arb_fp_sources[] = {
    "!!ARBfp1.0\nMOV result.color, fragment.color;\nEND\n",
    "!!ARBfp1.0\nPARAM k = {0.5, 0.25, 2.0, 1.0};\nMAD result.color, fragment.color, k, k.yxwz;\nEND\n",
    "!!ARBfp1.0\nTEMP t;\nTEX t, fragment.texcoord[0], texture[0], 2D;\nMUL result.color, t, fragment.color;\nEND\n",
    "!!ARBfp1.0\nTEMP c;\nMOV c, fragment.texcoord[0];\nMOV c.w, 2.0;\nTXP result.color, c, texture[0], 2D;\nEND\n",
    "!!ARBfp1.0\nTEMP t;\nSUB t, fragment.color.g, 0.5;\nKIL t.x;\nMOV result.color, fragment.color;\nEND\n",
    "!!ARBfp1.0\nTEMP a, b;\nLRP a, fragment.color.r, {1,0,0,1}, {0,0,1,1};\nSUB b, fragment.color.g, 0.4;\n"
    "CMP result.color, b.x, a, fragment.color;\nEND\n",
    "!!ARBfp1.0\nTEMP d;\nDP3 d, fragment.color, {0.3, 0.6, 0.1, 0};\nPOW d.x, d.x, 3.0;\nMOV result.color, d.x;\n"
    "MOV result.color.w, 1.0;\nEND\n",
    "!!ARBfp1.0\nTEMP r;\nADD r, fragment.color, 0.5;\nRSQ r.x, r.x;\nRCP r.y, r.y;\nEX2 r.z, fragment.color.b;\n"
    "MUL result.color, r, 0.5;\nMOV result.color.w, 1.0;\nEND\n",
    "!!ARBfp1.0\nOPTION ARB_precision_hint_fastest;\nTEMP t;\nFRC t, fragment.texcoord[0];\n"
    "MOV result.color, t;\nMOV result.color.w, 1.0;\nEND\n",
    "!!ARBfp1.0\nOPTION ARB_fog_linear;\nMOV result.color, fragment.color;\nEND\n",
    "!!ARBfp1.0\nPARAM l = program.local[3];\nPARAM e = program.env[7];\nMAD result.color, fragment.color, l, e;\nEND\n",
};
GLC_CASE_VARIANTS(arb_fragment_program, arb_fp_names, .profile = GLC_LEGACY, .tolerance = 1)
{
    unsigned char pixels[8 * 8 * 4];
    glc_pattern_rgba8(pixels, 8, 8, 29);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glc_arb_program(GL_FRAGMENT_PROGRAM_ARB, arb_fp_sources[glc_variant]);
    glProgramLocalParameter4fARB(GL_FRAGMENT_PROGRAM_ARB, 3, 0.8f, 0.5f, 1.2f, 1);
    glProgramEnvParameter4fARB(GL_FRAGMENT_PROGRAM_ARB, 7, 0.1f, -0.2f, 0.05f, 0);
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 0);
    glFogf(GL_FOG_END, 2);
    const float fog_color[4] = {0.8f, 0.8f, 0.2f, 1};
    glFogfv(GL_FOG_COLOR, fog_color);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glColor4f(1, 0.2f, 0.1f, 1); glTexCoord2f(-0.5f, 0); glVertex3f(-0.9f, -0.9f, 0.9f);
    glColor4f(0.1f, 1, 0.3f, 0.8f); glTexCoord2f(2.5f, 0); glVertex3f(0.9f, -0.9f, 0.3f);
    glColor4f(0.2f, 0.3f, 1, 0.6f); glTexCoord2f(2.5f, 3); glVertex3f(0.9f, 0.9f, -0.3f);
    glColor4f(0.9f, 0.9f, 0.1f, 0.4f); glTexCoord2f(-0.5f, 3); glVertex3f(-0.9f, 0.9f, -0.9f);
    glEnd();
}

static const char *const arb_vp_names[] = {"transform_state", "lighting_math", "address_register", "texcoord_outputs"};
static const char *const arb_vp_sources[] = {
    "!!ARBvp1.0\nPARAM mvp[4] = { state.matrix.mvp };\nTEMP p;\nDP4 p.x, mvp[0], vertex.position;\n"
    "DP4 p.y, mvp[1], vertex.position;\nDP4 p.z, mvp[2], vertex.position;\nDP4 p.w, mvp[3], vertex.position;\n"
    "MOV result.position, p;\nMOV result.color, vertex.color;\nEND\n",
    "!!ARBvp1.0\nPARAM l = {0.3, 0.8, 0.5, 0};\nTEMP n, d;\nMOV result.position, vertex.position;\n"
    "DP3 d.x, vertex.normal, l;\nMAX d.x, d.x, 0.0;\nLIT d, d;\nMUL result.color, vertex.color, d.y;\n"
    "MOV result.color.w, 1.0;\nEND\n",
    "!!ARBvp1.0\nPARAM table[4] = { {1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,0,1} };\nADDRESS a;\nTEMP i;\n"
    "MUL i.x, vertex.color.x, 3.0;\nARL a.x, i.x;\nMOV result.color, table[a.x];\nMOV result.position, vertex.position;\nEND\n",
    "!!ARBvp1.0\nMOV result.position, vertex.position;\nMUL result.texcoord[2], vertex.position, 0.5;\n"
    "MOV result.texcoord[5], vertex.color;\nMOV result.color, {1,1,1,1};\nEND\n",
};
GLC_CASE_VARIANTS(arb_vertex_program, arb_vp_names, .profile = GLC_LEGACY, .tolerance = 1)
{
    glc_arb_program(GL_VERTEX_PROGRAM_ARB, arb_vp_sources[glc_variant]);
    if (glc_variant == 3)
        glc_arb_program(GL_FRAGMENT_PROGRAM_ARB,
                        "!!ARBfp1.0\nTEMP a;\nADD a, fragment.texcoord[2], 0.5;\n"
                        "MAD result.color, fragment.texcoord[5], 0.5, a;\nEND\n");
    glMatrixMode(GL_MODELVIEW);
    glRotatef(20, 0, 0, 1);
    glScalef(0.9f, 0.8f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLE_FAN);
    for (int i = 0; i < 6; ++i) {
        float a = (float)i / 6 * 6.2831853f;
        glColor3f((float)i / 5, 1 - (float)i / 5, 0.5f);
        glNormal3f(__builtin_cosf(a), __builtin_sinf(a), 0.5f);
        glVertex2f(0.9f * __builtin_cosf(a), 0.9f * __builtin_sinf(a));
    }
    glEnd();
}

/* ---- GLSL 1.20 ----------------------------------------------------------- */

static const char *const glsl_names[] = {"builtins", "varyings_uniforms", "texture2d", "discard_and_branch", "loops_arrays",
                                         "matrix_math", "no_version_directive", "crlf_source"};
GLC_CASE_VARIANTS(glsl120, glsl_names, .profile = GLC_LEGACY, .tolerance = 1)
{
    static const char *const vertex[] = {
        "#version 120\nvoid main(){ gl_Position = ftransform(); gl_FrontColor = gl_Color; gl_TexCoord[0] = gl_MultiTexCoord0; }",
    };
    static const char *const fragments[] = {
        "#version 120\nvoid main(){ gl_FragColor = gl_Color * vec4(1.0, 0.8, 0.6, 1.0); }",
        "#version 120\nuniform vec3 tint; uniform float amount;\n"
        "void main(){ gl_FragColor = vec4(mix(gl_Color.rgb, tint, amount), gl_Color.a); }",
        "#version 120\nuniform sampler2D tex;\nvoid main(){ gl_FragColor = texture2D(tex, gl_TexCoord[0].st * 2.0) * gl_Color; }",
        "#version 120\nvoid main(){ if (gl_Color.g > 0.6) discard; else if (gl_Color.r > 0.5) gl_FragColor = vec4(1.0);\n"
        " else gl_FragColor = gl_Color.bgra; }",
        "#version 120\nuniform float w[4];\nvoid main(){ float s = 0.0; for (int i = 0; i < 4; ++i) s += w[i] * gl_Color[i];\n"
        " gl_FragColor = vec4(fract(s * 3.0), s, 1.0 - s, 1.0); }",
        "#version 120\nuniform mat3 m;\nvoid main(){ gl_FragColor = vec4(clamp(m * gl_Color.rgb, 0.0, 1.0), 1.0); }",
        /* Apple accepts GLSL without #version (1.10). */
        "uniform vec3 tint;\nvoid main(){ gl_FragColor = vec4(gl_Color.rgb * tint, 1.0); }",
        "#version 120\r\nvoid main()\r\n{\r\n  gl_FragColor = gl_Color.gbra;\r\n}\r\n",
    };
    unsigned char pixels[8 * 8 * 4];
    glc_pattern_rgba8(pixels, 8, 8, 31);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint program = glc_program(vertex[0], fragments[glc_variant], NULL);
    glUniform3f(glGetUniformLocation(program, "tint"), 0.2f, 0.9f, 0.5f);
    glUniform1f(glGetUniformLocation(program, "amount"), 0.4f);
    glUniform1i(glGetUniformLocation(program, "tex"), 0);
    const float weights[4] = {0.5f, 0.3f, 0.15f, 0.05f};
    glUniform1fv(glGetUniformLocation(program, "w"), 4, weights);
    const float m[9] = {0.8f, 0.1f, 0.3f, 0.2f, 0.7f, 0.1f, 0.4f, 0.0f, 0.9f};
    glUniformMatrix3fv(glGetUniformLocation(program, "m"), 1, GL_FALSE, m);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glColor4f(1, 0.2f, 0.1f, 1); glTexCoord2f(0, 0); glVertex2f(-0.9f, -0.9f);
    glColor4f(0.1f, 1, 0.3f, 0.8f); glTexCoord2f(1, 0); glVertex2f(0.9f, -0.9f);
    glColor4f(0.2f, 0.3f, 1, 0.6f); glTexCoord2f(1, 1); glVertex2f(0.9f, 0.9f);
    glColor4f(0.9f, 0.9f, 0.1f, 0.4f); glTexCoord2f(0, 1); glVertex2f(-0.9f, 0.9f);
    glEnd();
}

GLC_CASE(glsl120_attributes, .profile = GLC_LEGACY)
{
    static const char *const attributes[] = {"position", "color", NULL};
    glc_program("#version 120\nattribute vec3 position; attribute vec4 color; varying vec4 v;\n"
                "void main(){ gl_Position = vec4(position, 1.0); v = color; }",
                "#version 120\nvarying vec4 v;\nvoid main(){ gl_FragColor = v; }", attributes);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), fan_vertices[0].position);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(struct vertex), fan_vertices[0].color);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 6);
}

/* ---- framebuffers and pixels --------------------------------------------- */

GLC_CASE(render_to_texture, .profile = GLC_LEGACY)
{
    GLint screen = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &screen);
    GLuint texture, framebuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffersEXT(1, &framebuffer);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) glc_fail("incomplete FBO");
    glViewport(0, 0, 32, 32);
    glClearColor(0.1f, 0.1f, 0.4f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex2f(-1, -1);
    glColor3f(0, 1, 0); glVertex2f(1, -1);
    glColor3f(0, 0, 1); glVertex2f(0, 1);
    glEnd();
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (GLuint)screen);
    glViewport(0, 0, glc_width, glc_height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-0.8f, -0.9f);
    glTexCoord2f(1, 0); glVertex2f(0.9f, -0.6f);
    glTexCoord2f(1, 1); glVertex2f(0.7f, 0.9f);
    glTexCoord2f(0, 1); glVertex2f(-0.9f, 0.8f);
    glEnd();
}

GLC_CASE(draw_pixels, .profile = GLC_LEGACY)
{
    unsigned char pixels[20 * 12 * 4];
    glc_pattern_rgba8(pixels, 20, 12, 37);
    glClear(GL_COLOR_BUFFER_BIT);
    glRasterPos2f(-0.7f, -0.5f);
    glPixelZoom(2, 3);
    glDrawPixels(20, 12, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

GLC_CASE(read_pixels_formats, .profile = GLC_LEGACY)
{
    /* Draws a gradient, reads it back in several formats, and draws those
       readbacks as rows so conversion differences become visible. */
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glColor4f(0, 0, 0, 0); glVertex2f(-1, -1);
    glColor4f(1, 0.5f, 0, 1); glVertex2f(1, -1);
    glColor4f(1, 1, 1, 0.5f); glVertex2f(1, 1);
    glColor4f(0, 0.5f, 1, 0.25f); glVertex2f(-1, 1);
    glEnd();
    unsigned char bgra[64 * 4], rgb[64 * 3], lum[64];
    float floats[64 * 4];
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 10, 64, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, bgra);
    glReadPixels(0, 20, 64, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    glReadPixels(0, 30, 64, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
    glReadPixels(0, 40, 64, 1, GL_RGBA, GL_FLOAT, floats);
    unsigned char rows[4][64 * 4];
    for (int x = 0; x < 64; ++x) {
        rows[0][x * 4] = bgra[x * 4 + 2]; rows[0][x * 4 + 1] = bgra[x * 4 + 1]; rows[0][x * 4 + 2] = bgra[x * 4]; rows[0][x * 4 + 3] = bgra[x * 4 + 3];
        rows[1][x * 4] = rgb[x * 3]; rows[1][x * 4 + 1] = rgb[x * 3 + 1]; rows[1][x * 4 + 2] = rgb[x * 3 + 2]; rows[1][x * 4 + 3] = 255;
        rows[2][x * 4] = rows[2][x * 4 + 1] = rows[2][x * 4 + 2] = lum[x]; rows[2][x * 4 + 3] = 255;
        for (int c = 0; c < 4; ++c) rows[3][x * 4 + c] = (unsigned char)(floats[x * 4 + c] * 255.0f + 0.5f);
    }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glPixelZoom(1, 8);
    for (int r = 0; r < 4; ++r) {
        glWindowPos2i(0, r * 16);
        glDrawPixels(64, 1, GL_RGBA, GL_UNSIGNED_BYTE, rows[r]);
    }
}

GLC_CASE(push_pop_attrib, .profile = GLC_LEGACY)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glColor3f(0.4f, 0.2f, 0.1f);
    glRectf(-0.9f, -0.9f, 0.3f, 0.3f);
    glRectf(-0.3f, -0.3f, 0.9f, 0.9f);
    glPopAttrib();
    /* Blending is off again and the colour is back to the default white. */
    glRectf(-0.2f, 0.4f, 0.2f, 0.8f);
}

/* ---- rectangle textures through programs --------------------------------- */

static const char *const rect_program_names[] = {"glsl_linear", "glsl_fetch", "arb", "arb_txp"};
GLC_CASE_VARIANTS(program_rectangle, rect_program_names, .profile = GLC_LEGACY, .tolerance = 1)
{
    enum { kW = 24, kH = 10 };
    unsigned char pixels[kW * kH * 4];
    glc_pattern_rgba8(pixels, kW, kH, 23);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, texture);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLint filter = glc_variant == 0 ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (glc_variant < 2) {
        const char *vs = "#version 120\nvoid main(){ gl_Position = ftransform(); gl_TexCoord[0] = gl_MultiTexCoord0; }";
        const char *fs = glc_variant == 0
                             ? "#version 120\n#extension GL_ARB_texture_rectangle : enable\nuniform sampler2DRect t;\n"
                               "void main(){ gl_FragColor = texture2DRect(t, gl_TexCoord[0].xy); }"
                             : "#version 120\n#extension GL_ARB_texture_rectangle : enable\n#extension GL_EXT_gpu_shader4 : enable\n"
                               "uniform sampler2DRect t;\n"
                               "void main(){ gl_FragColor = texelFetch2DRect(t, ivec2(gl_TexCoord[0].xy)); }";
        GLuint program = glc_program(vs, fs, NULL);
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "t"), 0);
    } else {
        glc_arb_program(GL_FRAGMENT_PROGRAM_ARB,
                        glc_variant == 2 ? "!!ARBfp1.0\nTEX result.color, fragment.texcoord[0], texture[0], RECT;\nEND\n"
                                         : "!!ARBfp1.0\nTXP result.color, fragment.texcoord[0], texture[0], RECT;\nEND\n");
    }
    glClear(GL_COLOR_BUFFER_BIT);
    float q = glc_variant == 3 ? 2.0f : 1.0f;
    glBegin(GL_QUADS);
    glTexCoord4f(-2 * q, -1 * q, 0, q); glVertex2f(-0.9f, -0.9f);
    glTexCoord4f((kW + 2) * q, -1 * q, 0, q); glVertex2f(0.9f, -0.9f);
    glTexCoord4f((kW + 2) * q, (kH + 1) * q, 0, q); glVertex2f(0.9f, 0.9f);
    glTexCoord4f(-2 * q, (kH + 1) * q, 0, q); glVertex2f(-0.9f, 0.9f);
    glEnd();
}

static const char *const raster_names[] = {"bitmap_font", "copy_pixels", "window_pos_blend", "pixel_transfer_depth",
                                           "raster_invalid"};
GLC_CASE_VARIANTS(raster_ops, raster_names, .profile = GLC_LEGACY)
{
    glClearColor(0.1f, 0.1f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    unsigned char pixels[16 * 16 * 4];
    glc_pattern_rgba8(pixels, 16, 16, 11);
    switch (glc_variant) {
    case 0: {
        /* An 8x10 glyph drawn three times, advancing the raster position. */
        static const GLubyte glyph[10] = {0xC3, 0xC3, 0xC3, 0xFF, 0xFF, 0xC3, 0xC3, 0x66, 0x3C, 0x18};
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glColor3f(1, 0.8f, 0.2f);
        glRasterPos2f(-0.8f, -0.2f);
        for (int i = 0; i < 3; ++i) {
            if (i == 2) glColor3f(0, 1, 0); /* current colour after glRasterPos: no effect */
            glBitmap(8, 10, 1, 2, 12, 3, glyph);
        }
        glColor3f(0.3f, 0.6f, 1);
        glRasterPos2f(-0.8f, 0.4f);
        glPixelStorei(GL_UNPACK_LSB_FIRST, GL_TRUE);
        glBitmap(8, 10, 0, 0, 0, 0, glyph);
        glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
        break;
    }
    case 1:
        glRasterPos2f(-0.9f, -0.9f);
        glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glRasterPos2f(0.1f, 0.1f);
        glPixelZoom(1.5f, -1);
        glCopyPixels(2, 3, 12, 10, GL_COLOR);
        glPixelZoom(1, 1);
        break;
    case 2:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glWindowPos2i(10, 20);
        glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glWindowPos2f(30.5f, 5);
        glDrawPixels(16, 16, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        glDisable(GL_BLEND);
        break;
    case 3:
        /* Depth test against the raster position's depth; colour scale. */
        glEnable(GL_DEPTH_TEST);
        glColor3f(0.2f, 0.2f, 0.9f);
        glBegin(GL_QUADS);
        glVertex3f(-1, -1, 0); glVertex3f(0, -1, 0); glVertex3f(0, 1, 0); glVertex3f(-1, 1, 0);
        glEnd();
        glPixelTransferf(GL_RED_SCALE, 0.5f);
        glPixelTransferf(GL_BLUE_BIAS, 0.25f);
        glRasterPos3f(-0.5f, -0.25f, 0.5f);
        glPixelZoom(4, 2);
        glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glRasterPos3f(-0.5f, 0.5f, -0.5f);
        glDrawPixels(16, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glPixelZoom(1, 1);
        glPixelTransferf(GL_RED_SCALE, 1);
        glPixelTransferf(GL_BLUE_BIAS, 0);
        glDisable(GL_DEPTH_TEST);
        break;
    case 4: {
        /* Outside the view volume: nothing is drawn. */
        glRasterPos2f(2, 0);
        GLint valid = 1;
        glGetIntegerv(GL_CURRENT_RASTER_POSITION_VALID, &valid);
        glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glRasterPos2f(0, 0);
        GLfloat position[4];
        glGetFloatv(GL_CURRENT_RASTER_POSITION, position);
        glColor3f(valid ? 1.0f : 0.0f, position[0] / 64.0f, position[1] / 64.0f);
        glRectf(-0.3f, -0.3f, 0.3f, 0.3f);
        break;
    }
    }
}

static const char *const list_names[] = {"geometry_and_state", "nested_and_matrices", "call_lists_base",
                                         "compile_and_execute", "arrays_dereferenced", "bind_in_list"};
GLC_CASE_VARIANTS(display_lists, list_names, .profile = GLC_LEGACY)
{
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint base = glGenLists(4);
    switch (glc_variant) {
    case 0:
        glNewList(base, GL_COMPILE);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0); glVertex2f(-0.8f, -0.8f);
        glColor3f(0, 1, 0); glVertex2f(0.8f, -0.8f);
        glColor3f(0, 0, 1); glVertex2f(0, 0.8f);
        glEnd();
        glColor3f(1, 1, 0);
        glEndList();
        glColor3f(0.2f, 0.2f, 0.2f); /* compile only: the list's colours did not apply */
        glRectf(-1, 0.85f, -0.6f, 1);
        glCallList(base);
        glRectf(0.6f, 0.85f, 1, 1);  /* the list left the colour yellow */
        break;
    case 1:
        glNewList(base + 1, GL_COMPILE);
        glColor3f(0.9f, 0.5f, 0.1f);
        glRectf(-0.2f, -0.2f, 0.2f, 0.2f);
        glEndList();
        glNewList(base, GL_COMPILE);
        glPushMatrix();
        glTranslatef(-0.5f, 0.4f, 0);
        glCallList(base + 1);
        glTranslatef(0.8f, -0.1f, 0);
        glRotatef(30, 0, 0, 1);
        glScalef(1.5f, 0.5f, 1);
        glCallList(base + 1);
        glPopMatrix();
        glEndList();
        glCallList(base);
        glTranslatef(0, -0.9f, 0);
        glCallList(base);
        glLoadIdentity();
        break;
    case 2: {
        /* A "font": list per character, drawn with glCallLists and a base. */
        for (int i = 0; i < 4; ++i) {
            glNewList(base + (GLuint)i, GL_COMPILE);
            glColor3f(0.25f * (float)(i + 1), 1 - 0.2f * (float)i, 0.5f);
            glRectf(0, 0, 0.15f, 0.1f + 0.1f * (float)i);
            glTranslatef(0.2f, 0, 0);
            glEndList();
        }
        glListBase(base);
        static const GLubyte text[] = {0, 1, 2, 3, 3, 2, 1, 0, 2};
        glTranslatef(-0.9f, -0.5f, 0);
        glCallLists(sizeof text, GL_UNSIGNED_BYTE, text);
        glLoadIdentity();
        static const GLushort more[] = {1, 3};
        glListBase(base - 1);
        glTranslatef(-0.9f, 0.4f, 0);
        glCallLists(2, GL_UNSIGNED_SHORT, more);
        glLoadIdentity();
        glListBase(0);
        break;
    }
    case 3:
        glNewList(base, GL_COMPILE_AND_EXECUTE);
        glColor3f(0.2f, 0.8f, 0.9f);
        glRectf(-0.9f, -0.9f, -0.1f, -0.1f);
        glEndList();
        glTranslatef(1, 1, 0);
        glCallList(base);
        glLoadIdentity();
        break;
    case 4: {
        float positions[] = {-0.8f, -0.8f, 0.8f, -0.8f, 0.8f, 0.8f, -0.8f, 0.8f};
        const float colors[] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1};
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(2, GL_FLOAT, 0, positions);
        glColorPointer(3, GL_FLOAT, 0, colors);
        glNewList(base, GL_COMPILE);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glEndList();
        /* Changing the arrays afterwards does not change the list. */
        for (int i = 0; i < 8; ++i) positions[i] *= 0.3f;
        glCallList(base);
        glColorPointer(3, GL_FLOAT, 0, colors + 3);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 3);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        break;
    }
    case 5: {
        /* A texture bound inside a list: bindings and queries follow. */
        GLuint textures[2];
        glGenTextures(2, textures);
        unsigned char pixels[4 * 8 * 4];
        glc_pattern_rgba8(pixels, 4, 8, 3);
        for (int t = 0; t < 2; ++t) {
            glBindTexture(GL_TEXTURE_2D, textures[t]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels + t * 16);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        glNewList(base, GL_COMPILE);
        glBindTexture(GL_TEXTURE_2D, textures[1]);
        glEndList();
        glCallList(base);
        GLint bound = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        glEnable(GL_TEXTURE_2D);
        glColor3f(1, 1, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(-0.8f, -0.8f);
        glTexCoord2f(1, 0); glVertex2f(0.8f, -0.8f);
        glTexCoord2f(1, 1); glVertex2f(0.8f, 0.8f);
        glTexCoord2f(0, 1); glVertex2f(-0.8f, 0.8f);
        glEnd();
        glDisable(GL_TEXTURE_2D);
        glColor3f(bound == (GLint)textures[1] ? 0.0f : 1.0f, glIsList(base) ? 1.0f : 0.0f, glIsList(base + 3) ? 1.0f : 0.0f);
        glRectf(0.85f, 0.85f, 1, 1);
        break;
    }
    }
    glDeleteLists(base, 4);
}

/* gl_ClipVertex against user clip planes (Source's togl writes it in every
   translated vertex shader). The plane is given in eye space. */
GLC_CASE(glsl120_clip_vertex, .profile = GLC_LEGACY, .tolerance = 1)
{
    static const char *vertex =
        "#version 120\nuniform vec4 vc[2];\nattribute vec4 v0;\n"
        "void main(){ vec4 p = gl_ModelViewMatrix * gl_Vertex; gl_ClipVertex = p;\n"
        " gl_Position = gl_ProjectionMatrix * p; gl_FrontColor = gl_Color * vc[1] + vc[0]; }";
    static const char *fragment = "#version 120\nvoid main(){ gl_FragColor = gl_Color; }";
    GLuint program = glc_program(vertex, fragment, NULL);
    const float vc[8] = {0.1f, 0.0f, 0.2f, 0.0f, 0.9f, 1.0f, 0.8f, 1.0f};
    glUniform4fv(glGetUniformLocation(program, "vc"), 2, vc);
    const double plane0[4] = {1, 1, 0, 0.2}, plane1[4] = {-0.3, 1, 0, 0.5};
    glClipPlane(GL_CLIP_PLANE0, plane0);
    glClipPlane(GL_CLIP_PLANE1, plane1);
    glEnable(GL_CLIP_PLANE0);
    glEnable(GL_CLIP_PLANE1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glColor3f(1, 0.2f, 0.1f); glVertex2f(-0.9f, -0.9f);
    glColor3f(0.1f, 1, 0.3f); glVertex2f(0.9f, -0.9f);
    glColor3f(0.2f, 0.3f, 1); glVertex2f(0.9f, 0.9f);
    glColor3f(0.9f, 0.9f, 0.1f); glVertex2f(-0.9f, 0.9f);
    glEnd();
    glDisable(GL_CLIP_PLANE0);
    glDisable(GL_CLIP_PLANE1);
}

/* Occlusion queries the way Source's HDR histogram uses them: legacy
   context, ARB entry points, colour writes off, a shader discarding
   fragments outside a band, results polled for availability. The counts
   are drawn as colours. */
static const char *const occlusion_names[] = {"plain", "discard_band", "color_mask_off", "depth_tested", "polled",
                                              "alpha_test_glsl"};
GLC_CASE_VARIANTS(legacy_occlusion_query, occlusion_names, .profile = GLC_LEGACY)
{
    static const char *vertex = "#version 120\nvoid main(){ gl_Position = ftransform(); gl_TexCoord[0] = gl_MultiTexCoord0; }";
    static const char *fragment =
        "#version 120\nuniform vec2 band;\nvoid main(){ float l = gl_TexCoord[0].x;\n"
        " if (l < band.x || l >= band.y) discard; gl_FragColor = vec4(l, 0.5, 0.2, l); }";
    GLuint program = glc_program(vertex, fragment, NULL);
    glUniform2f(glGetUniformLocation(program, "band"), glc_variant == 1 ? 0.25f : 0.0f, glc_variant == 1 ? 0.6f : 2.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (glc_variant == 3) {
        /* A nearer quad over the left half hides those fragments. */
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glUseProgram(0);
        glColor3f(0.1f, 0.1f, 0.4f);
        glBegin(GL_QUADS);
        glVertex3f(-1, -1, -0.5f); glVertex3f(0, -1, -0.5f); glVertex3f(0, 1, -0.5f); glVertex3f(-1, 1, -0.5f);
        glEnd();
        glUseProgram(program);
    }
    if (glc_variant == 2) glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    if (glc_variant == 5) {
        /* The fixed-function alpha test still applies after a GLSL shader. */
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.4f);
    }
    GLuint query;
    glGenQueriesARB(1, &query);
    glBeginQueryARB(GL_SAMPLES_PASSED_ARB, query);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(-0.8f, -0.8f, 0.2f);
    glTexCoord2f(1, 0); glVertex3f(0.8f, -0.8f, 0.2f);
    glTexCoord2f(1, 1); glVertex3f(0.8f, 0.8f, 0.2f);
    glTexCoord2f(0, 1); glVertex3f(-0.8f, 0.8f, 0.2f);
    glEnd();
    glEndQueryARB(GL_SAMPLES_PASSED_ARB);
    glDisable(GL_ALPHA_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    GLuint samples = 0;
    GLint available = 0;
    if (glc_variant == 4) {
        glFlush();
        for (int i = 0; i < 2000000 && !available; ++i) glGetQueryObjectivARB(query, GL_QUERY_RESULT_AVAILABLE_ARB, &available);
    }
    glGetQueryObjectuivARB(query, GL_QUERY_RESULT_ARB, &samples);
    glDeleteQueriesARB(1, &query);
    glUseProgram(0);
    /* Count as colour bars: low byte, next byte, availability. */
    glColor3f((float)(samples & 0xff) / 255, (float)((samples >> 8) & 0xff) / 255, glc_variant == 4 ? (float)available : 0.5f);
    glRectf(-1, -1, 1, -0.85f);
}

/* The fixed-function alpha test after a GLSL or ARB fragment program. */
static const char *const alpha_program_names[] = {"glsl_greater", "glsl_less", "glsl_equal", "glsl_never", "glsl_two_refs",
                                                  "arb_gequal"};
GLC_CASE_VARIANTS(legacy_alpha_test_program, alpha_program_names, .profile = GLC_LEGACY)
{
    static const GLenum funcs[] = {GL_GREATER, GL_LESS, GL_EQUAL, GL_NEVER, GL_GREATER, GL_GEQUAL};
    if (glc_variant == 5) {
        glEnable(GL_FRAGMENT_PROGRAM_ARB);
        glc_arb_program(GL_FRAGMENT_PROGRAM_ARB, "!!ARBfp1.0\nMOV result.color, fragment.color;\nEND\n");
    } else {
        glc_program("#version 120\nvoid main(){ gl_Position = ftransform(); gl_FrontColor = gl_Color; }",
                    "#version 120\nvoid main(){ gl_FragColor = gl_Color.bgra; }", NULL);
    }
    glClearColor(0.1f, 0.2f, 0.3f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(funcs[glc_variant], glc_variant == 2 ? 1.0f : 0.5f);
    for (int pass = 0; pass < (glc_variant == 4 ? 2 : 1); ++pass) {
        if (pass) glAlphaFunc(GL_GREATER, 0.8f);
        float y = pass ? 0.05f : -0.9f;
        /* Alpha (blue for GLSL, which swaps) runs 0..1 across the quad. */
        glBegin(GL_QUADS);
        glColor4f(0.9f, 0.4f, 0.0f, 0.0f); glVertex2f(-0.9f, y);
        glColor4f(0.2f, 0.8f, 1.0f, 1.0f); glVertex2f(0.9f, y);
        glColor4f(0.2f, 0.8f, 1.0f, 1.0f); glVertex2f(0.9f, y + 0.85f);
        glColor4f(0.9f, 0.4f, 0.0f, 0.0f); glVertex2f(-0.9f, y + 0.85f);
        glEnd();
    }
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    glClearColor(0, 0, 0, 0);
}
