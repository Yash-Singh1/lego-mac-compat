#include "glc_gl_legacy.h"
#include "glcompare.h"

#ifndef GL_TEXTURE_CUBE_MAP_SEAMLESS
#define GL_TEXTURE_CUBE_MAP_SEAMLESS 0x884F
#endif
static const char *const ff_cube_state_modes[] = {"state_switch", "lod_bias", "multitexture"};
GLC_CASE_VARIANTS(legacy_cube_sampling_state, ff_cube_state_modes, .profile = GLC_LEGACY)
{
    GLuint textures[2]; glGenTextures(2,textures);
    for(int unit=0;unit<(glc_variant==2?2:1);++unit){
        glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_CUBE_MAP,textures[unit]);
        for(int level=0,size=32;size;size>>=1,++level)for(int face=0;face<6;++face){
            unsigned char pixels[32*32*4];
            for(int y=0;y<size;++y)for(int x=0;x<size;++x){int at=(y*size+x)*4;
                pixels[at]=(unsigned char)(face*29+level*13+unit*17);
                pixels[at+1]=(unsigned char)(x*255/(size>1?size-1:1));
                pixels[at+2]=(unsigned char)(y*255/(size>1?size-1:1));pixels[at+3]=255;}
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,level,GL_RGBA8,size,size,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,glc_variant==1?GL_LINEAR_MIPMAP_LINEAR:GL_NEAREST);
        glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,glc_variant==1?GL_LINEAR:GL_NEAREST);
        glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,unit?GL_MODULATE:GL_REPLACE);
        glEnable(GL_TEXTURE_CUBE_MAP);
    }
    glActiveTexture(GL_TEXTURE0);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    for(int panel=0;panel<3;++panel){
        if(panel==2)glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);else glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
        for(int unit=0;unit<(glc_variant==2?2:1);++unit){
            glActiveTexture(GL_TEXTURE0+unit);
            glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_S,panel==1?GL_CLAMP_TO_EDGE:GL_REPEAT);
            glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_T,panel==1?GL_CLAMP_TO_EDGE:GL_REPEAT);
            if(glc_variant==1)glTexEnvf(GL_TEXTURE_FILTER_CONTROL,GL_TEXTURE_LOD_BIAS,panel==0?-.75f:panel==1?.75f:0);
        }
        const float left=-1+panel*(2.0f/3),right=-1+(panel+1)*(2.0f/3);
        glBegin(GL_QUADS);
        for(int vertex=0;vertex<4;++vertex){
            const float x=vertex==0||vertex==3?left:right,y=vertex<2?-1:1;
            const float z=vertex==0||vertex==3?-.8f:.8f;
            glMultiTexCoord3f(GL_TEXTURE0,.75f,.375f,z);
            if(glc_variant==2)glMultiTexCoord3f(GL_TEXTURE1,z,.375f,.75f);
            glVertex2f(x,y);
        }
        glEnd();
    }
    glActiveTexture(GL_TEXTURE0);glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    GLenum error=glGetError();if(error)glc_fail("FF cube state GL error %x",error);
    glDeleteTextures(2,textures);
}
