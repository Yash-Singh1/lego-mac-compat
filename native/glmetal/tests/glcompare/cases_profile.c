/* Profile-specific dispatch, including calls with pointers the rejected
 * entry point must never read or write. Run through the public GL ABI. */
#include "glc_gl_legacy.h"
#include <limits.h>

void *glc_lookup(const char *name);

static void expect_error(const char *call, GLenum expected)
{
    GLenum got = glGetError();
    if (got != expected) glc_fail("%s: error 0x%x, expected 0x%x", call, got, expected);
}

static void invalid_state_values(void)
{
    const struct { const char *name; GLenum query; } setters[] = {
        {"glDepthFunc", GL_DEPTH_FUNC}, {"glCullFace", GL_CULL_FACE_MODE},
        {"glFrontFace", GL_FRONT_FACE}, {"glLogicOp", GL_LOGIC_OP_MODE},
        {"glBlendEquation", GL_BLEND_EQUATION_RGB},
    };
    for (size_t i = 0; i < sizeof setters / sizeof *setters; ++i) {
        GLint before, after;
        glGetIntegerv(setters[i].query, &before);
        void (*set)(GLenum) = glc_lookup(setters[i].name);
        set(0);
        expect_error(setters[i].name, GL_INVALID_ENUM);
        glGetIntegerv(setters[i].query, &after);
        if (before != after) glc_fail("%s changed state on error", setters[i].name);
    }
    glPointSize(0);
    expect_error("glPointSize", GL_INVALID_VALUE);
    glLineWidth(-1);
    expect_error("glLineWidth", GL_INVALID_VALUE);
    glStencilFunc(GL_ZERO, 1, 0xff);
    expect_error("glStencilFunc", GL_INVALID_ENUM);
    glPolygonMode(GL_FRONT_AND_BACK, GL_ZERO);
    expect_error("glPolygonMode", GL_INVALID_ENUM);
    GLint mode[2];
    glGetIntegerv(GL_POLYGON_MODE, mode);
    if (mode[0] != GL_FILL || mode[1] != GL_FILL) glc_fail("invalid polygon mode changed state");
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(profile_core_invalid_state, .profile = GLC_CORE) { invalid_state_values(); }
GLC_CASE(profile_legacy_invalid_state, .profile = GLC_LEGACY)
{
    invalid_state_values();
    glAlphaFunc(GL_ZERO, 0.5f);
    expect_error("glAlphaFunc", GL_INVALID_ENUM);
    glShadeModel(GL_ZERO);
    expect_error("glShadeModel", GL_INVALID_ENUM);
}

GLC_CASE(profile_core_dispatch, .profile = GLC_CORE)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    expect_error("core texture binding", GL_NO_ERROR);
    GLint value = 0x1357;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, &value);
    expect_error("core depth texture mode query", GL_INVALID_ENUM);
    if (value != 0x1357) glc_fail("invalid texture query wrote its output");
    glMatrixMode(GL_MODELVIEW);
    expect_error("glMatrixMode", GL_INVALID_OPERATION);
    glLoadMatrixf(NULL);
    expect_error("glLoadMatrixf", GL_INVALID_OPERATION);
    glColor4fv(NULL);
    expect_error("glColor4fv", GL_INVALID_OPERATION);
    glNewList(1, GL_COMPILE);
    expect_error("glNewList", GL_INVALID_OPERATION);
    glCallLists(1, GL_UNSIGNED_INT, NULL);
    expect_error("glCallLists", GL_INVALID_OPERATION);
    glDeleteLists(1, INT_MAX);
    expect_error("glDeleteLists", GL_INVALID_OPERATION);
    if (glGenLists(1)) glc_fail("core glGenLists returned a name");
    expect_error("glGenLists", GL_INVALID_OPERATION);
    GLuint untouched = 0x1357;
    glGenVertexArraysAPPLE(1, &untouched);
    expect_error("glGenVertexArraysAPPLE", GL_INVALID_OPERATION);
    if (untouched != 0x1357) glc_fail("rejected query wrote its output");

    void (*gen)(GLsizei, GLuint *) = glc_lookup("glGenVertexArrays");
    void (*bind)(GLuint) = glc_lookup("glBindVertexArray");
    void (*del)(GLsizei, const GLuint *) = glc_lookup("glDeleteVertexArrays");
    GLuint vao;
    gen(1, &vao);
    bind(vao);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(0);
    glEnableVertexAttribArrayARB(0);
    glDisableVertexAttribArrayARB(0);
    expect_error("core attribute array with a VAO", GL_NO_ERROR);
    del(1, &vao);
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(profile_legacy_dispatch, .profile = GLC_LEGACY)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    expect_error("legacy texture binding", GL_NO_ERROR);
    GLint value = 0x1357;
    glGetTexParameteriv(GL_TEXTURE_2D, 0x8e42 /* GL_TEXTURE_SWIZZLE_R */, &value);
    expect_error("legacy texture swizzle query", GL_INVALID_ENUM);
    if (value != 0x1357) glc_fail("invalid texture query wrote its output");
    void (*gen)(GLsizei, GLuint *) = glc_lookup("glGenVertexArrays");
    void (*clear)(GLenum, GLint, const GLfloat *) = glc_lookup("glClearBufferfv");
    const GLubyte *(*stringi)(GLenum, GLuint) = glc_lookup("glGetStringi");
    GLuint untouched = 0x1357;
    gen(1, &untouched);
    expect_error("glGenVertexArrays", GL_INVALID_OPERATION);
    if (untouched != 0x1357) glc_fail("rejected query wrote its output");
    clear(GL_COLOR, 0, NULL);
    expect_error("glClearBufferfv", GL_INVALID_OPERATION);
    if (stringi(GL_EXTENSIONS, 0)) glc_fail("legacy glGetStringi returned a string");
    expect_error("glGetStringi", GL_INVALID_OPERATION);

    GLuint vao;
    glGenVertexArraysAPPLE(1, &vao);
    glBindVertexArrayAPPLE(vao);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(0);
    glDeleteVertexArraysAPPLE(1, &vao);
    expect_error("legacy APPLE vertex arrays", GL_NO_ERROR);

    /* The first error must survive a later profile rejection even when the
     * first call was queued on GLMetal's worker. */
    glEnable(0xffffffffu);
    gen(1, &untouched);
    expect_error("queued error before profile rejection", GL_INVALID_ENUM);
    while (glGetError()) {}
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(profile_display_list_large_delete, .profile = GLC_LEGACY)
{
    GLuint first = glGenLists(3);
    if (!first) glc_fail("glGenLists failed");
    glNewList(first, GL_COMPILE);
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEndList();
    glDeleteLists(first + 1, INT_MAX);
    if (!glIsList(first) || glIsList(first + 1) || glIsList(first + 2))
        glc_fail("large range deleted the wrong lists");
    glDeleteLists(UINT_MAX - 1, INT_MAX);
    if (!glIsList(first)) glc_fail("overflowing range deleted a low list name");
    glCallList(first);
    glDeleteLists(first, 1);
    expect_error("large display-list deletion", GL_NO_ERROR);
}
