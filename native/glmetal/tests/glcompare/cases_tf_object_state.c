#include "glc_gl_core.h"
#include "glcompare.h"

GLC_CASE(core_tf_object_capture_state, .profile = GLC_CORE)
{
    GLuint program = glc_program(
        "#version 410 core\nuniform float base;out float value;void main(){gl_Position=vec4(0,0,0,1);value=base+float(gl_VertexID);}",
        "#version 410 core\nout vec4 color;void main(){color=vec4(1);}", NULL);
    const char *varying = "value"; glTransformFeedbackVaryings(program, 1, &varying, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program); GLint linked; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) glc_fail("TF object state link failed");
    glUseProgram(program);
    GLuint objects[3] = {0}, buffers[3], vao, queries[2];
    glGenTransformFeedbacks(2, objects+1); glGenBuffers(3, buffers);
    glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenQueries(2, queries);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, queries[0]);
    for (int i = 0; i < 3; ++i) {
        glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, objects[i]);
        GLint active, paused; glGetIntegerv(GL_TRANSFORM_FEEDBACK_BUFFER_ACTIVE, &active); glGetIntegerv(GL_TRANSFORM_FEEDBACK_BUFFER_PAUSED, &paused);
        if (active || paused) glc_fail("Fresh TF object inherited another capture");
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[i]);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 4*sizeof(float), NULL, GL_STREAM_READ);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffers[i]);
        glUniform1f(glGetUniformLocation(program,"base"), (float)(10*i));
        glBeginTransformFeedback(GL_POINTS); glDrawArrays(GL_POINTS, 0, i+1); glPauseTransformFeedback();
    }
    for (int i = 2; i >= 0; --i) {
        glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, objects[i]);
        GLint active, paused; glGetIntegerv(GL_TRANSFORM_FEEDBACK_BUFFER_ACTIVE, &active); glGetIntegerv(GL_TRANSFORM_FEEDBACK_BUFFER_PAUSED, &paused);
        if (!active || !paused) glc_fail("TF object lost paused active state");
        GLint bound; glGetIntegeri_v(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, 0, &bound);
        if ((GLuint)bound != buffers[i]) glc_fail("TF object lost buffer ownership");
        glUniform1f(glGetUniformLocation(program,"base"), (float)(10*i+5));
        glResumeTransformFeedback(); glDrawArrays(GL_POINTS, 0, 1); glEndTransformFeedback();
        glGetIntegerv(GL_TRANSFORM_FEEDBACK_BUFFER_ACTIVE, &active); glGetIntegerv(GL_TRANSFORM_FEEDBACK_BUFFER_PAUSED, &paused);
        if (active || paused) glc_fail("Ended TF object retained active state");
    }
    glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    GLuint count; glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &count);
    if (count != 9) glc_fail("Interleaved captures changed primitive count %u",count);
    for (int i = 0; i < 3; ++i) {
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[i]); float values[4];
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof values, values);
        for (int j = 0; j < i+1; ++j) if (values[j] != 10*i+j) glc_fail("TF object %d original value %d corrupted",i,j);
        if (values[i+1] != 10*i+5) glc_fail("TF object %d resumed at wrong offset",i);
    }
    glBeginQuery(GL_PRIMITIVES_GENERATED, queries[1]);
    for (int i = 1; i < 3; ++i) glDrawTransformFeedback(GL_POINTS, objects[i]);
    glEndQuery(GL_PRIMITIVES_GENERATED); glGetQueryObjectuiv(queries[1], GL_QUERY_RESULT, &count);
    if (count != 7) glc_fail("TF object completed counts changed %u",count);
    glDisable(GL_RASTERIZER_DISCARD);
    glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);
    GLenum error = glGetError(); if (error) glc_fail("TF object state GL error %x",error);
    glDeleteQueries(2,queries); glDeleteTransformFeedbacks(2,objects+1); glDeleteBuffers(3,buffers);
    glDeleteVertexArrays(1,&vao); glDeleteProgram(program);
}
