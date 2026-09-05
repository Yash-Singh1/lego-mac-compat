#include "arb_program_guard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect_contains(const char *output, const char *needle)
{
    if (strstr(output, needle)) return 0;
    fprintf(stderr, "missing transformed text: %s\n", needle);
    return 1;
}

int main(void)
{
    static const char source[] =
        "!!ARBfp1.0\n"
        "OPTION ARB_draw_buffers;\n"
        "TEMP R0;\n"
        "RSQ R0.x, R0.y;\n"
        "RCP R0.y, R0.y;\n"
        "RCP R0.z, -R0.w;\n"
        "END\n";
    size_t output_size = 0;
    size_t rsq_count = 0;
    size_t rcp_count = 0;
    char *output = arb_program_guard_undefined_math(
        source, sizeof(source) - 1, &output_size, &rsq_count, &rcp_count);
    if (!output) {
        fprintf(stderr, "guard unexpectedly returned NULL\n");
        return 1;
    }

    int failures = 0;
    if (rsq_count != 1 || rcp_count != 2) {
        fprintf(stderr, "unexpected counts rsq=%zu rcp=%zu\n",
                rsq_count, rcp_count);
        ++failures;
    }
    if (output_size != strlen(output)) {
        fprintf(stderr, "output size mismatch %zu != %zu\n",
                output_size, strlen(output));
        ++failures;
    }
    const char *option = strstr(output, "OPTION ARB_draw_buffers;\n");
    const char *parameter = strstr(output, "PARAM lp32_math_guard");
    if (!option || !parameter || parameter < option) {
        fprintf(stderr, "declarations were not inserted after OPTION\n");
        ++failures;
    }
    failures += expect_contains(
        output,
        "ABS lp32_rcp_scratch.x, R0.y;\n"
        "MAX lp32_rcp_scratch.x, lp32_rcp_scratch.x, lp32_math_guard.x;\n"
        "RSQ R0.x, lp32_rcp_scratch.x;\n");
    failures += expect_contains(
        output,
        "MOV lp32_rcp_scratch.x, R0.y;\n"
        "SLT lp32_rcp_scratch.y, lp32_rcp_scratch.x, "
            "lp32_math_guard.w;\n"
        "ABS lp32_rcp_scratch.x, lp32_rcp_scratch.x;\n"
        "MAX lp32_rcp_scratch.x, lp32_rcp_scratch.x, lp32_math_guard.x;\n"
        "RCP lp32_rcp_scratch.x, lp32_rcp_scratch.x;\n"
        "MAD lp32_rcp_scratch.y, lp32_rcp_scratch.y, "
            "lp32_math_guard.y, lp32_math_guard.z;\n"
        "MUL R0.y, lp32_rcp_scratch.x, lp32_rcp_scratch.y;\n");
    failures += expect_contains(output,
                               "MOV lp32_rcp_scratch.x, -R0.w;\n");
    free(output);

    /* SM2 vertex programs compute RCP/RSQ straight into write-only result
       registers; the guarded form must never read the destination back. */
    static const char result_destination[] =
        "!!ARBvp1.0\n"
        "TEMP R0;\n"
        "RCP result.texcoord[1].z, R0.x;\n"
        "RSQ result.texcoord[0].w, R0.y;\n"
        "END\n";
    output = arb_program_guard_undefined_math(
        result_destination, sizeof(result_destination) - 1, &output_size,
        &rsq_count, &rcp_count);
    if (!output) {
        fprintf(stderr, "guard unexpectedly returned NULL\n");
        return 1;
    }
    for (const char *cursor = output; (cursor = strstr(cursor, "result."));
         ++cursor) {
        const char *line = cursor;
        while (line > output && line[-1] != '\n') --line;
        const char *comma = strchr(line, ',');
        if (comma && comma < cursor) {
            fprintf(stderr, "result register read back: %.*s\n",
                    (int)(strchr(cursor, '\n') - line), line);
            ++failures;
        }
    }
    failures += expect_contains(output,
                               "RSQ result.texcoord[0].w, lp32_rcp_scratch.x;\n");
    failures += expect_contains(
        output,
        "MUL result.texcoord[1].z, lp32_rcp_scratch.x, lp32_rcp_scratch.y;\n");
    free(output);

    static const char unchanged[] =
        "!!ARBvp1.0\n"
        "TEMP R0;\n"
        "MOV R0, vertex.attrib[0];\n"
        "END\n";
    output_size = 0;
    rsq_count = 99;
    rcp_count = 99;
    output = arb_program_guard_undefined_math(
        unchanged, sizeof(unchanged) - 1, &output_size,
        &rsq_count, &rcp_count);
    if (output || output_size != sizeof(unchanged) - 1 || rsq_count ||
        rcp_count) {
        fprintf(stderr, "instruction-free program should be unchanged\n");
        free(output);
        ++failures;
    }

    printf("arb-program-guard %s\n", failures ? "FAIL" : "PASS");
    /* The SM2 water samples its RGBA reflection target with SHADOW2D, which
       Apple's GL resolves to black; the target keyword is made plain. */
    static const char shadow_targets[] =
        "!!ARBfp1.0\n"
        "OPTION ARB_fragment_program_shadow;\n"
        "TEMP R0;\n"
        "TXP R0.xyz, fragment.texcoord[0], texture[3], SHADOW2D;\n"
        "TEX R0.w, fragment.texcoord[1], texture[2],\tSHADOWRECT;\n"
        "TEX result.color, fragment.texcoord[2], texture[0], 2D;\n"
        "END\n";
    size_t rewrites = 0;
    output = arb_program_plain_shadow_targets(
        shadow_targets, sizeof(shadow_targets) - 1, &output_size, &rewrites);
    if (!output) {
        fprintf(stderr, "shadow rewrite unexpectedly returned NULL\n");
        return 1;
    }
    if (rewrites != 2) {
        fprintf(stderr, "expected 2 shadow rewrites, got %zu\n", rewrites);
        ++failures;
    }
    if (strstr(output, "SHADOW2D") || strstr(output, "SHADOWRECT")) {
        fprintf(stderr, "shadow target survived:\n%s\n", output);
        ++failures;
    }
    failures += expect_contains(
        output, "TXP R0.xyz, fragment.texcoord[0], texture[3], 2D;\n");
    failures += expect_contains(
        output, "TEX R0.w, fragment.texcoord[1], texture[2],\tRECT;\n");
    failures += expect_contains(output, "OPTION ARB_fragment_program_shadow;\n");
    free(output);

    /* Programs without shadow targets, and vertex programs, are left alone. */
    output = arb_program_plain_shadow_targets(
        result_destination, sizeof(result_destination) - 1, &output_size,
        &rewrites);
    if (output || rewrites) {
        fprintf(stderr, "vertex program unexpectedly rewritten\n");
        ++failures;
        free(output);
    }

    return failures ? 1 : 0;
}
