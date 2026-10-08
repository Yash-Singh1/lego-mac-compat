#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main

int main(int argc,char **argv)
{
    try {
        require(argc==2,"Usage: cube_array_cpu ARTIFACT_DIRECTORY"); initialize(); Artifacts artifacts(argv[1]);
        const char *expressions[]={"texture(t,vec4(d,1))","texture(t,vec4(d,1),.25)","textureLod(t,vec4(d,1),1)",
            "textureGrad(t,vec4(d,1),vec3(.01,0,0),vec3(0,.02,0))","vec4(textureQueryLod(t,d),0,1)",
            "lookup(t,vec4(d,1))","texture(ts[index],vec4(d,1))"};
        for(int i=0;i<7;++i){
            std::string fs="#version 410 core\nuniform samplerCubeArray t;uniform samplerCubeArray ts[2];uniform int index;out vec4 color;";
            if(i==5)fs+="vec4 lookup(samplerCubeArray s,vec4 c){return texture(s,c);}";
            fs+=std::string("void main(){vec3 d=normalize(vec3(gl_FragCoord.xy/64.0-.5,.6));color=")+expressions[i]+";}";
            glm_compile_request req={};req.sources[GLM_STAGE_VERTEX]="#version 410 core\nvoid main(){gl_Position=vec4(float(gl_VertexID),0,0,1);}";req.sources[GLM_STAGE_FRAGMENT]=fs.c_str();
            Result result;compile_uncached(&req,&result.value);require(result.value.ok,result.value.log?result.value.log:"Cube array compilation failed");
            const std::string msl=result.value.msl[GLM_STAGE_FRAGMENT];
            require(msl.find("texturecube_array<float>")!=std::string::npos,"Native cube array missing");
            require(msl.find("texture2d_array<float>")!=std::string::npos,"Hidden face array missing");
            require(msl.find("[[texture(64)")!=std::string::npos,"Face array binding missing");
            check_cache(result.value);
            artifacts.write("cube_array_"+std::to_string(i)+"-vertex",result.value.msl[GLM_STAGE_VERTEX]);
            artifacts.write("cube_array_"+std::to_string(i)+"-fragment",msl.c_str());
        }
        require(artifacts.count==14,"Unexpected cube array stage count");
        std::cout<<"Seven cube array compiler/cache fixtures passed; fourteen stages generated\n";
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
