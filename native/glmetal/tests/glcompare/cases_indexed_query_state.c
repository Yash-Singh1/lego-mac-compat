#include "glc_gl_core.h"
#include "glcompare.h"

static void expect_error(GLenum expected, const char *operation)
{
    GLenum actual = glGetError();
    if (actual != expected) glc_fail("%s error %x, expected %x", operation, actual, expected);
}

GLC_CASE(core_indexed_query_state, .profile = GLC_CORE)
{
    GLuint q[6];
    glGenQueries(6, q);
    for (GLuint i = 0; i < 4; ++i) {
        glBeginQueryIndexed(GL_PRIMITIVES_GENERATED, i, q[i]);
        GLint current = 0;
        glGetQueryIndexediv(GL_PRIMITIVES_GENERATED, i, GL_CURRENT_QUERY, &current);
        if ((GLuint)current != q[i]) glc_fail("Stream %u lost its active query", i);
    }
    expect_error(GL_NO_ERROR, "Independent queries");
    glBeginQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 1, q[1]);
    expect_error(GL_INVALID_OPERATION, "Reuse active object");
    glBeginQueryIndexed(GL_PRIMITIVES_GENERATED, 1, q[4]);
    expect_error(GL_INVALID_OPERATION, "Replace active query");
    glBeginQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 1, 0);
    expect_error(GL_INVALID_OPERATION, "Zero object");
    GLint unused;
    glGetQueryIndexediv(GL_PRIMITIVES_GENERATED, 4, GL_CURRENT_QUERY, &unused);
    expect_error(GL_INVALID_VALUE, "Invalid stream");
    glGetQueryIndexediv(GL_PRIMITIVES_GENERATED, 1, GL_TEXTURE_2D, &unused);
    expect_error(GL_INVALID_ENUM, "Invalid query pname");
    for (GLuint i = 0; i < 4; ++i) {
        glEndQueryIndexed(GL_PRIMITIVES_GENERATED, i);
        GLint current = -1;
        glGetQueryIndexediv(GL_PRIMITIVES_GENERATED, i, GL_CURRENT_QUERY, &current);
        if (current) glc_fail("Stream %u still active after EndQuery", i);
        GLuint count = ~0u;
        glGetQueryObjectuiv(q[i], GL_QUERY_RESULT, &count);
        if (count) glc_fail("Empty stream %u counted output", i);
    }
    glEndQueryIndexed(GL_PRIMITIVES_GENERATED, 1);
    expect_error(GL_INVALID_OPERATION, "End inactive query");
    glBeginQueryIndexed(GL_PRIMITIVES_GENERATED, 1, q[4]);
    glQueryCounter(q[4], GL_TIMESTAMP);
    expect_error(GL_INVALID_OPERATION, "Timestamp on active indexed query");
    glDeleteQueries(1, &q[4]);
    if (glIsQuery(q[4])) glc_fail("Deleted active query still has a valid name");
    GLint current = -1;
    glGetQueryIndexediv(GL_PRIMITIVES_GENERATED, 1, GL_CURRENT_QUERY, &current);
    if ((GLuint)current != q[4]) glc_fail("Deletion ended the active indexed query");
    glBeginQueryIndexed(GL_PRIMITIVES_GENERATED, 1, q[5]);
    expect_error(GL_INVALID_OPERATION, "Replace deleted active query");
    glEndQueryIndexed(GL_PRIMITIVES_GENERATED, 1);
    glGetQueryIndexediv(GL_PRIMITIVES_GENERATED, 1, GL_CURRENT_QUERY, &current);
    if (current) glc_fail("Deleted active query remained after EndQuery");
    glGetQueryObjectuiv(q[4], GL_QUERY_RESULT, (GLuint *)&unused);
    expect_error(GL_INVALID_OPERATION, "Result from deleted query name");
    q[4] = 0;
    glDeleteQueries(6, q);
    expect_error(GL_NO_ERROR, "Query cleanup");
    glClearColor(.25, .5, .75, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
