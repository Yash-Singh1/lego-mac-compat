#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main
int main(int argc, char **argv)
{
    try {
        require(argc == 2, "Expected artifact directory");
        initialize();
        Artifacts artifacts(argv[1]);
        for(int n=3;n<=4;++n) for(int mode=0;mode<3;++mode) {
            std::string type="dmat"+std::to_string(n);
            std::string source="#version 410 core\nuniform "+type+" a;flat out "+type+" result;";
            if(mode==1)source+=type+" f("+type+" x){return inverse(x);}";
            if(mode==2)source+="int calls=0;"+type+" next(){calls++;return a;}";
            source+="void main(){result="+(mode==0?std::string("inverse(a)"):mode==1?std::string("f(a)"):std::string("inverse(next())"))+";gl_Position=vec4(0);}";
            const char *varying="result";
            glm_compile_request request={};request.sources[GLM_STAGE_VERTEX]=source.c_str();request.feedback_varyings=&varying;request.feedback_count=1;request.feedback_interleaved=true;
            Result result;compile_uncached(&request,&result.value);
            require(result.value.ok,result.value.log?result.value.log:"Inverse compile failed");
            require(result.value.xfb[0].type==(unsigned)(n==3?0x8F47:0x8F48),"Inverse feedback lost Double type");
            require(result.value.xfb_stride[0]==n*n*2,"Inverse feedback stride incorrect");
            for(const char *stage:{result.value.msl[GLM_STAGE_VERTEX],result.value.msl_capture}) {
                require(stage&&strstr(stage,"glm_fp64_inverse("),"Exact inverse missing");
                require(strstr(stage,"cofactor_0_0"),"Cofactor helper missing");
            }
            std::string name="inverse"+std::to_string(n)+"_"+std::to_string(mode);
            artifacts.write(name+"_normal",result.value.msl[GLM_STAGE_VERTEX]);
            artifacts.write(name+"_capture",result.value.msl_capture);
            check_cache(result.value);
        }
        std::cout<<"Six exact inverse fixtures / twelve offline Metal stages\n";
        return 0;
    } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
