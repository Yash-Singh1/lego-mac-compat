# Query object lifecycle

Run `sh tests/probes/run_query_lifecycle_cpu.sh` for an AddressSanitizer CPU test of the actual queries.c implementation. It covers deletion of active indexed and occlusion queries, object-name reuse while the deleted object remains active, QueryCounter rejection for active objects, empty nonzero streams without geometry shaders, and context cleanup of named and deferred objects.

GL4.1 core section 2.15, printed page 142, makes a deleted query name unused immediately while retaining its active object until EndQuery. Section 5.1.2, printed page 312, requires INVALID_OPERATION when QueryCounter uses an active object. Appendix D.1.3 describes the separate name and object lifetimes.

Official source: https://registry.khronos.org/OpenGL/specs/gl/glspec41.core.pdf

The ordinary indexed-query comparison case now asserts the correct deferred-deletion behavior. Apple's observed active-query retention matches this specification; it is not a reference discrepancy.
