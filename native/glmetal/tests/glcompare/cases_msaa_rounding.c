#include "glc_gl_core.h"
#include "glcompare.h"

static const char *const sample_patterns[] = {"quarter", "half", "three_quarters", "half_reversed", "half_alternating", "half_middle"};
GLC_CASE_VARIANTS(core_msaa_rounding, sample_patterns, .profile = GLC_CORE, .samples = 4)
{
    GLuint program = glc_program(
        "#version 410 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "gl_Position=vec4(p*2.0-1.0,0,1);}",
        "#version 410 core\nuniform int sample_index, high_mask;out vec4 color;"
        "void main(){int n=(int(gl_FragCoord.x)+64*int(gl_FragCoord.y))%255;"
        "float b=float(n+((high_mask&(1<<sample_index))!=0?1:0))/255.0;"
        "color=vec4(b);gl_SampleMask[0]=1<<sample_index;}", NULL);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(program);
    /* The last three cases preserve the average but change the sum order. */
    const int masks[] = {1, 3, 7, 12, 5, 6};
    glUniform1i(glGetUniformLocation(program, "high_mask"), masks[glc_variant]);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    for (int sample = 0; sample < 4; ++sample) {
        glUniform1i(glGetUniformLocation(program, "sample_index"), sample);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}
