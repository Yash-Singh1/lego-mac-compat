#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>

static const char *const component_modes[] = {"texture_d24", "texture_d32f", "renderbuffer_d24", "renderbuffer_d32f"};
GLC_CASE_VARIANTS(core_depth_stencil_component_query, component_modes, .profile = GLC_CORE)
{
    GLint original;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    GLuint framebuffer, storage;
    GLenum format = glc_variant & 1 ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    if (glc_variant < 2) {
        glGenTextures(1, &storage);
        glBindTexture(GL_TEXTURE_2D, storage);
        glTexImage2D(GL_TEXTURE_2D, 0, format, 8, 8, 0, GL_DEPTH_STENCIL,
                     glc_variant & 1 ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV : GL_UNSIGNED_INT_24_8, NULL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, storage, 0);
    } else {
        glGenRenderbuffers(1, &storage);
        glBindRenderbuffer(GL_RENDERBUFFER, storage);
        glRenderbufferStorage(GL_RENDERBUFFER, format, 8, 8);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, storage);
    }
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Depth-stencil query framebuffer incomplete");
    GLint depth = -1, stencil = -1, combined = -1;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &depth);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &stencil);
    GLenum error = glGetError();
    if (error) glc_fail("Separate component query error %x", error);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &combined);
    error = glGetError();
    fprintf(stderr, "Depth-stencil %s: depth=%x stencil=%x combined_error=%x\n",
            component_modes[glc_variant], depth, stencil, error);
    /* GL4.1 section 6.1.13 returns information for the shared object.
       Later specifications require INVALID_OPERATION for this query. */
    if (error || combined != depth)
        glc_fail("GL4.1 combined component query: error=%x type=%x depth=%x", error, combined, depth);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    glDeleteFramebuffers(1, &framebuffer);
    if (glc_variant < 2) glDeleteTextures(1, &storage);
    else glDeleteRenderbuffers(1, &storage);
    glClearColor(depth == GL_FLOAT, stencil == GL_UNSIGNED_INT, .25f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
