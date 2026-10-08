/* ARB assembly programs to legacy GLSL (arb_translate.c). */
#ifndef GLM_ARB_TRANSLATE_H
#define GLM_ARB_TRANSLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct glm_arb_translation {
    char *glsl;             /* malloc'd; NULL on failure */
    char error[256];
    int error_position;     /* byte offset of the error */
    int env_count, local_count; /* vec4s of the GLMARB block's arrays */
    uint32_t attribs_used;  /* generic attributes read as arb_attribN */
};

bool glm_arb_translate(const char *source, size_t length, bool vertex, struct glm_arb_translation *out);

#endif
