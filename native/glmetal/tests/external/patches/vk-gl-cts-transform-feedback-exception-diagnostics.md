# Transform feedback exception diagnostics

The iterate catch-all blocks convert exceptions to InternalError without recording their messages. This patch logs the active dEQP or standard exception, including its function and catch location. Unknown non-standard exceptions also receive a message. The existing verdict and cleanup remain unchanged.

The shader-building utility catch remains unchanged because it deliberately catches an integer exception after logging compilation or linking errors. Apply this patch after the GL4.1 transform-feedback dialect patch, as listed in glcts.sh.
