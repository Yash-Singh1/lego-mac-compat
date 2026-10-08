# Transform-feedback link requires a vertex-processing stage

The probe links a fragment-only program without feedback, requests capture of
its fragment output and requires a failed link, then clears the capture list
and requires a successful relink. Failed linkage must not raise an API error.
This isolates the first assertion in the CTS `linking_errors_test`.

Run providers sequentially after the parent builds the standalone library:

```sh
xcrun clang -mmacosx-version-min=11.0 -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/transform_feedback_link_validation.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libfeedback-link-validation.dylib
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider apple --cases /tmp/libfeedback-link-validation.dylib --out build/feedback-link-validation-apple
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libfeedback-link-validation.dylib --out build/feedback-link-validation-glmetal
```

The separate CTS `api_errors_test` currently fails while linking an isoline
TES. That is the existing unsupported tessellation feature and is not changed
by the fragment-only link validation fix.
