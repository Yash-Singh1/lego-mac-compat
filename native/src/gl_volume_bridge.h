#ifndef GL_VOLUME_BRIDGE_H
#define GL_VOLUME_BRIDGE_H
#include <stdint.h>
void gl_volume_bridge32_enable_tfu_compat(void);
int gl_volume_bridge32_upload(const uint32_t *arguments);
#endif
