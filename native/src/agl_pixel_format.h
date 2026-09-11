#ifndef LP32_AGL_PIXEL_FORMAT_H
#define LP32_AGL_PIXEL_FORMAT_H
#include <stddef.h>

/* AGL_FULLSCREEN requests an obsolete exclusive drawable. COD4 presents in
   a Cocoa window instead. Preserve every other requirement, especially depth
   and stencil; otherwise the game's last-resort chooser silently omits them.
   AGL's legacy constants come from the system AGL BridgeSupport metadata. */
static inline size_t lp32_agl_window_attributes(const int *source, size_t limit,
                                                int *output, size_t capacity) {
    if (!source || !output) return 0;
    size_t used=0;
    for(size_t i=0;i<limit;){
        int attribute=source[i++],has_value=0;
        switch(attribute){
        case 0:
            if(used>=capacity)return 0;
            output[used++]=0;return used;
        case 2: case 3: case 7: case 8: case 9: case 10: case 11:
        case 12: case 13: case 14: case 15: case 16: case 17:
        case 50: case 55: case 56: case 70: case 82: case 84:
            has_value=1;break;
        case 54: continue; /* AGL_FULLSCREEN */
        case 1: case 4: case 5: case 6: case 51: case 52: case 53:
        case 57: case 58: case 59: case 60: case 61: case 71: case 72:
        case 73: case 74: case 75: case 76: case 78: case 80: case 81:
        case 83: case 90: case 91: case 96:
            break;
        default:return 0; /* Never guess the arity of an unfamiliar attribute. */
        }
        if(used>=capacity)return 0;
        output[used++]=attribute;
        if(has_value){if(i>=limit || used>=capacity)return 0;output[used++]=source[i++];}
    }
    return 0;
}
#endif
