#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>

GLC_CASE(core_feedback_stream_draw_counts, .profile = GLC_CORE)
{
    const char *vs = "#version 410 core\nvoid main(){gl_Position=vec4(0,0,0,1);}";
    const char *gs = "#version 410 core\nlayout(points)in;layout(points,max_vertices=5)out;"
        "layout(stream=1)out vec4 second;void main(){"
        "for(int i=0;i<2;++i){gl_Position=vec4(i+1,2,3,1);EmitStreamVertex(0);}"
        "for(int i=0;i<3;++i){second=vec4(i+10,4,5,1);EmitStreamVertex(1);}}";
    GLuint shaders[2] = {glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_GEOMETRY_SHADER)};
    glShaderSource(shaders[0], 1, &vs, NULL);
    glShaderSource(shaders[1], 1, &gs, NULL);
    glCompileShader(shaders[0]);
    glCompileShader(shaders[1]);
    GLuint capture = glCreateProgram();
    glAttachShader(capture, shaders[0]);
    glAttachShader(capture, shaders[1]);
    const char *varyings[] = {"gl_Position", "gl_NextBuffer", "second"};
    glTransformFeedbackVaryings(capture, 3, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(capture);
    GLint linked = 0;
    glGetProgramiv(capture, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048] = {0};
        glGetProgramInfoLog(capture, sizeof log, NULL, log);
        glc_fail("Stream capture link failed: %s", log);
    }
    GLuint object, buffers[2], vao, queries[5];
    glGenTransformFeedbacks(1, &object);
    glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, object);
    glGenBuffers(2, buffers);
    for (int i = 0; i < 2; ++i) {
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[i]);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 5 * 4 * sizeof(float), NULL, GL_STREAM_READ);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, i, buffers[i]);
    }
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenQueries(5, queries);
    glUseProgram(capture);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 0, queries[0]);
    glBeginQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 1, queries[1]);
    glBeginQueryIndexed(GL_PRIMITIVES_GENERATED, 0, queries[3]);
    glBeginQueryIndexed(GL_PRIMITIVES_GENERATED, 1, queries[4]);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 1);
    glEndTransformFeedback();
    glEndQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 0);
    glEndQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 1);
    glEndQueryIndexed(GL_PRIMITIVES_GENERATED, 0);
    glEndQueryIndexed(GL_PRIMITIVES_GENERATED, 1);
    GLuint generated[2] = {0};
    glGetQueryObjectuiv(queries[3], GL_QUERY_RESULT, generated);
    glGetQueryObjectuiv(queries[4], GL_QUERY_RESULT, generated + 1);
    if (generated[0] != 2 || generated[1] != 3)
        glc_fail("Generated stream counts %u/%u expected 2/3", generated[0], generated[1]);
    GLuint captured[2] = {0};
    glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, captured);
    glGetQueryObjectuiv(queries[1], GL_QUERY_RESULT, captured + 1);
    if (captured[0] != 2 || captured[1] != 3)
        glc_fail("Stream capture counts %u/%u expected 2/3", captured[0], captured[1]);
    GLuint draw = glc_program(vs, "#version 410 core\nout vec4 color;void main(){color=vec4(1);}", NULL);
    /* The completed object's counts must survive binding a different object. */
    glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);
    glBeginQuery(GL_PRIMITIVES_GENERATED, queries[2]);
    glDrawTransformFeedbackStream(GL_POINTS, object, 0);
    glDrawTransformFeedbackStream(GL_POINTS, object, 1);
    glEndQuery(GL_PRIMITIVES_GENERATED);
    GLuint total = 0;
    glGetQueryObjectuiv(queries[2], GL_QUERY_RESULT, &total);
    fprintf(stderr, "Feedback streams: captured=%u/%u drawn=%u\n", captured[0], captured[1], total);
    if (total != 5) glc_fail("Stream draw lost the nonzero stream's completed count");
    GLenum error = glGetError();
    if (error) glc_fail("Stream draw error %x", error);
    glDisable(GL_RASTERIZER_DISCARD);
    glDeleteQueries(5, queries);
    glDeleteTransformFeedbacks(1, &object);
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(draw);
    glDeleteProgram(capture);
    glDeleteShader(shaders[0]);
    glDeleteShader(shaders[1]);
    glClearColor(.25, .5, .75, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
