# Packed depth/stencil component query version check

The patch changes only VerifyParametersTest's component-type assertion. Desktop contexts below GL4.4 must return GL_NO_ERROR and the same component type as the depth attachment of the shared object. GL4.4 and later retain the original GL_INVALID_OPERATION assertion. The ES branch remains unchanged.

The official [GL4.3 specification](https://registry.khronos.org/OpenGL/specs/gl/glspec43.core.pdf), section9.2.3, printed page261, permits the shared-object query and restricts integer components to color buffers. The official [GL4.4 specification](https://registry.khronos.org/OpenGL/specs/gl/glspec44.core.pdf), section9.2.3, printed pages277–278, prohibits the combined component-type query and specifies GL_INVALID_OPERATION. AppendixG.3, printed page684, records the October18,2013 correction under bugs9170 and10357. [GL4.5](https://registry.khronos.org/OpenGL/specs/gl/glspec45.core.pdf), section9.2.3, printed page297, retains the prohibition.

This patch does not change the driver, other attachment queries, expected attachment sizes, or ES behavior. Apply from the pinned VK-GL-CTS root after the independent default-framebuffer fixture patch.
