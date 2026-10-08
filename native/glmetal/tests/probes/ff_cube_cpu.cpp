#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main
extern "C" {
#include "../../src/ffgen.h"
int glm_custom_borders = 0;
struct glm_texture *glm_unit_texture(struct glm_context *,int,int *) { return nullptr; }
bool glm_unit_border(struct glm_context *,int,int,float *,float *) { return false; }
bool glm_format_lookup(GLenum,struct glm_format_info *) { return false; }
void glm_mat4_multiply(float *,const float *,const float *) {}
bool glm_mat4_invert(float *,const float *) { return false; }
}
int main(int argc,char **argv)
{
    try {
        require(argc==2,"Usage: ff_cube_cpu ARTIFACT_DIRECTORY"); Artifacts artifacts(argv[1]);
        for(int i=0;i<4;++i){
            glm_ff_key key={};key.color_attachments=1;key.alpha_func=7;
            key.unit[0].target=4;key.unit[0].env_mode=GL_REPLACE;
            if(i==1){key.texgen_needs_eye=key.texgen_needs_normal=1;key.unit[0].gen[0]=key.unit[0].gen[1]=key.unit[0].gen[2]=4;}
            if(i==2){key.unit[7].target=4;key.unit[7].env_mode=GL_MODULATE;key.unit[1].target=2;key.unit[1].env_mode=GL_MODULATE;key.unit[1].border=1;}
            if(i==3)key.front_only=key.all_outputs=1;
            char *source=glm_ff_generate(&key);require(source,"FF generation failed");
            std::string msl(source);free(source);
            require(msl.find("glm_cube_faces0 [[texture(64)]]")!=std::string::npos,"FF face alias missing");
            require(msl.find("glm_cube_lod [[buffer(25)]]")!=std::string::npos,"FF metadata slot missing");
            require(msl.find("glm_cube_float_bias(tex0")!=std::string::npos,"FF shared cube call missing");
            if(i==2)require(msl.find("glm_cube_faces7 [[texture(71)]]")!=std::string::npos,"Highest FF cube alias missing");
            artifacts.write("ff_cube_"+std::to_string(i),msl.c_str());
        }
        require(artifacts.count==4,"Unexpected FF stage library count");
        std::cout<<"Four FF cube shader libraries generated\n";
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
