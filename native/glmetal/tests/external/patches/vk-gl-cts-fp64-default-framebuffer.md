# FP64 uniform test framebuffer restoration

The FP64 maximum-uniform test renders its fragment cases into an integer texture, then switches to transform feedback for geometry, tessellation and vertex cases. Its fragment cleanup bound framebuffer zero. In the CTS FBO surface configuration, the render context supplies a nonzero default framebuffer; zero has no drawable attachment.

Restore `getDefaultFramebuffer()` at that stage boundary, as the packed-depth test already does. The patch applies identically to Apple and GLMetal. Uniform limits, shader sources, transform feedback and expected results remain unchanged. A new CTS binary requires a new Apple baseline.
