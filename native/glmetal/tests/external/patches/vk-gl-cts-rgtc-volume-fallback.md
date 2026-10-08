# RGTC volume uploads

The packed-pixel rectangle test treats RGTC internal formats as invalid for
`TexImage3D`. That restriction belongs to `CompressedTexImage3D`, which receives
encoded compressed blocks. `TexImage3D` receives ordinary pixel data and can use
uncompressed storage when the requested compression format cannot represent the
target.

[OpenGL 4.1 core, section 3.8.3, printed page 196](https://registry.khronos.org/OpenGL/specs/gl/glspec41.core.pdf)
requires this fallback. The same text remains in
[OpenGL 4.6 core, section 8.5, printed page 206](https://registry.khronos.org/OpenGL/specs/gl/glspec46.core.pdf).
The patch removes only the four RGTC entries from the desktop `TexImage` volume
rejection. Depth and depth/stencil rejection remains. Pixel comparisons still run
for successful RGTC volume uploads.

The focused `core_rgtc_volume_fallback` cases check one- and two-slice volumes,
uncompressed storage, retained dimensions, and signed or unsigned pixel values.
Apple uses R8, RG8, R8_SNORM, or RG8_SNORM storage for these requests. GLMetal now
selects these formats for core-profile volume uploads instead of allocating BC4
or BC5 volume storage. Direct compressed upload restrictions are unchanged.
