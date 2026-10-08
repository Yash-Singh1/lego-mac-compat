# Packed depth/stencil read error codes

The probe checks eleven invalid ReadPixels type combinations and one legal
packed type against a complete depth/stencil framebuffer. It expects
GL_INVALID_ENUM for DEPTH_STENCIL with types other than UNSIGNED_INT_24_8 and
FLOAT_32_UNSIGNED_INT_24_8_REV, as required by OpenGL 4.1 section 4.3.2.

This is a standalone strict test because the comparison suite uses Apple's
implementation as its image oracle. The CTS validate_errors tests also exercise
these combinations and keep their original assertions.

```sh
xcrun clang -mmacosx-version-min=11.0 -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/depth_stencil_read_errors.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libdepth-read-errors.dylib
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libdepth-read-errors.dylib --out build/depth-read-errors-fixed
```
