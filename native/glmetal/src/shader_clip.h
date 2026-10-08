#ifndef GLM_SHADER_CLIP_H
#define GLM_SHADER_CLIP_H

/* Reflection uses the linked interfaces, including the fragment interpolation
   qualifiers. Unsupported interfaces retain the ordinary hardware path. */
static glm_clip_result *glm_clip_reflect(const std::vector<uint32_t> &vertex,
                                       const std::vector<uint32_t> &fragment)
{
    using namespace spirv_cross;
    Compiler vs(vertex),fs(fragment);vs.update_active_builtins();fs.update_active_builtins();
    for(auto builtin:{spv::BuiltInVertexId,spv::BuiltInVertexIndex,spv::BuiltInInstanceId,spv::BuiltInInstanceIndex})
        if(vs.has_active_builtin(builtin,spv::StorageClassInput))return nullptr;
    if(!vs.has_active_builtin(spv::BuiltInClipDistance,spv::StorageClassOutput))return nullptr;
    if(fs.has_active_builtin(spv::BuiltInClipDistance,spv::StorageClassInput)||
       fs.has_active_builtin(spv::BuiltInCullDistance,spv::StorageClassInput))return nullptr;
    for(auto builtin:{spv::BuiltInLayer,spv::BuiltInViewportIndex,spv::BuiltInCullDistance})
        if(vs.has_active_builtin(builtin,spv::StorageClassOutput))return nullptr;
    auto vr=vs.get_shader_resources(),fr=fs.get_shader_resources(fs.get_active_interface_variables());
    int clips=0;
    for(auto &b:vr.builtin_outputs)if(b.builtin==spv::BuiltInClipDistance){
        const auto &t=vs.get_type(b.value_type_id);
        if(t.basetype!=SPIRType::Float||t.width!=32||t.array.size()!=1||!t.array_size_literal[0])return nullptr;
        clips=int(t.array[0]);
    }
    if(clips<1||clips>8)return nullptr;
    std::vector<glm_clip_varying> variables;std::vector<std::string> names;int stride=4+clips;
    for(auto &input:fr.stage_inputs){
        const auto &t=fs.get_type(input.type_id);
        if(t.columns!=1||!t.array.empty()||t.vecsize<1||t.vecsize>4||t.width!=32)return nullptr;
        int kind=t.basetype==SPIRType::Float?0:t.basetype==SPIRType::Int?1:t.basetype==SPIRType::UInt?2:-1;
        if(kind<0||fs.has_decoration(input.id,spv::DecorationComponent))return nullptr;
        const Resource *output=nullptr;
        for(auto &candidate:vr.stage_outputs)
            if(vs.get_decoration(candidate.id,spv::DecorationLocation)==fs.get_decoration(input.id,spv::DecorationLocation))output=&candidate;
        if(!output)return nullptr;
        const auto &ot=vs.get_type(output->type_id);
        if(ot.basetype!=t.basetype||ot.width!=t.width||ot.vecsize!=t.vecsize||ot.columns!=1||!ot.array.empty())return nullptr;
        // Interface blocks need member-level layout and are intentionally excluded.
        std::string name=vs.get_name(output->id);
        if(name.empty()||name!=fs.get_name(input.id))return nullptr;
        bool flat=fs.has_decoration(input.id,spv::DecorationFlat);
        if(kind&&!flat)return nullptr;
        if(fs.has_decoration(input.id,spv::DecorationSample)||fs.has_decoration(input.id,spv::DecorationCentroid))return nullptr;
        names.push_back(name);variables.push_back({nullptr,int(t.vecsize),kind,flat?1:fs.has_decoration(input.id,spv::DecorationNoPerspective)?2:0,stride});
        stride+=int(t.vecsize);
    }
    if(stride>256)return nullptr;
    auto *r=static_cast<glm_clip_result *>(calloc(1,sizeof(glm_clip_result)));
    r->clip_count=clips;r->vs_stride=stride;r->out_stride=stride+1;
    r->primitive_id=fs.has_active_builtin(spv::BuiltInPrimitiveId,spv::StorageClassInput);
    r->varying_count=int(variables.size());r->varyings=static_cast<glm_clip_varying *>(calloc(variables.size()+1,sizeof(glm_clip_varying)));
    std::copy(variables.begin(),variables.end(),r->varyings);
    for(size_t i=0;i<names.size();++i)r->varyings[i].name=copy(names[i]);
    for(auto &v:variables)r->flat_outputs|=v.interpolation==1;
    return r;
}

static std::string glm_clip_type(const glm_clip_varying &v)
{
    if(v.components==1)return v.kind==0?"float":v.kind==1?"int":"uint";
    return std::string(v.kind==0?"vec":v.kind==1?"ivec":"uvec")+std::to_string(v.components);
}
static std::string glm_clip_kernel(const glm_clip_result &r)
{
    std::ostringstream k;
    k<<"#version 430\nlayout(local_size_x=1) in;\n"
      <<"layout(std430,set=1,binding=24) readonly buffer GLMGsIn {float inputWords[];};\n"
      <<"layout(std430,set=1,binding=25) writeonly buffer GLMGsVertices {float outputWords[];};\n"
      <<"layout(std430,set=1,binding=26) writeonly buffer GLMGsIndices {uint outputIndices[];};\n"
      <<"layout(std140,set=1,binding=27) uniform GLMGsInfo {uvec4 info;};\n"
      <<"struct V {float words["<<r.vs_stride<<"];};\n"
      <<"void main(){uint tri=gl_GlobalInvocationID.x;if(tri>=info.x)return;V a[11],b[11];int count=3;bool fallback=false;uint shift=((info.z+tri)&1u)==0u?(info.w&3u):((info.w>>2u)&3u);\n"
      <<"for(int v=0;v<3;++v){for(int c=0;c<"<<r.vs_stride<<";++c)a[v].words[c]=inputWords[(tri*3+((uint(v)+shift)%3u))*"<<r.vs_stride<<"u+uint(c)];"
      <<"if(!(a[v].words[3]>0))fallback=true;for(int c=0;c<"<<4+r.clip_count<<";++c)if(isnan(a[v].words[c])||isinf(a[v].words[c]))fallback=true;}\n"
      <<"if(!fallback)for(int plane=0;plane<"<<r.clip_count<<";++plane){if((info.y&(1u<<uint(plane)))==0u)continue;int n=0;\n"
      <<"for(int i=0;i<count;++i){int j=(i+1)%count;float da=a[i].words[4+plane],db=a[j].words[4+plane];bool ia=da>=0,ib=db>=0;"
      <<"if(ia){if(n>=11){fallback=true;break;}b[n++]=a[i];}"
      <<"if(ia!=ib){float denominator=da-db;float t=da/denominator;float w=a[i].words[3]+t*(a[j].words[3]-a[i].words[3]);"
      <<"if(isnan(denominator)||isinf(denominator)||isnan(t)||isinf(t)||t<0||t>1||!(w>0)||n>=11){fallback=true;break;}V v;"
      <<"for(int c=0;c<"<<r.vs_stride<<";++c)v.words[c]=a[i].words[c]+t*(a[j].words[c]-a[i].words[c]);\n";
    for(int i=0;i<r.varying_count;++i){const auto &v=r.varyings[i];
        if(v.interpolation==2)k<<"float s"<<i<<"=t*a[j].words[3]/w;\n";
        for(int c=0;c<v.components;++c){int offset=v.offset+c;
            if(v.interpolation==1)k<<"v.words["<<offset<<"]=inputWords[tri*3u*"<<r.vs_stride<<"u+"<<offset<<"u];\n";
            else if(v.interpolation==2)k<<"v.words["<<offset<<"]=a[i].words["<<offset<<"]+s"<<i<<"*(a[j].words["<<offset<<"]-a[i].words["<<offset<<"]);\n";
        }
    }
    k<<"b[n++]=v;} }if(fallback)break;count=n;for(int v=0;v<count;++v)a[v]=b[v];if(count==0)break;}\n"
      <<"if(fallback){count=3;for(int v=0;v<3;++v)for(int c=0;c<"<<r.vs_stride<<";++c)a[v].words[c]=inputWords[(tri*3u+((uint(v)+shift)%3u))*"<<r.vs_stride<<"u+uint(c)];}\n"
      <<"if(tri==0u){for(int c=0;c<"<<r.out_stride<<";++c)outputWords[c]=0;outputWords[0]=2;outputWords[1]=2;outputWords[2]=2;outputWords[3]=1;}\n"
      <<"for(int v=0;v<count;++v){uint o=(1u+tri*11u+uint(v))*"<<r.out_stride<<"u;for(int c=0;c<"<<r.vs_stride<<";++c)outputWords[o+uint(c)]=a[v].words[c];\n"
      <<"if(!fallback)for(int c=0;c<"<<r.clip_count<<";++c)outputWords[o+4u+uint(c)]=1;\n";
    for(int i=0;i<r.varying_count;++i){const auto &v=r.varyings[i];if(v.interpolation==1)
        for(int c=0;c<v.components;++c)k<<"outputWords[o+"<<v.offset+c<<"u]=inputWords[tri*3u*"<<r.vs_stride<<"u+"<<v.offset+c<<"u];\n";}
    k<<"outputWords[o+"<<r.vs_stride<<"u]=uintBitsToFloat(info.z+tri);}\n"
      <<"for(int i=0;i<27;++i)outputIndices[tri*27u+uint(i)]=0u;for(int v=1;v+1<count;++v){uint o=tri*27u+uint(v-1)*3u,base=1u+tri*11u;outputIndices[o]=base;outputIndices[o+1u]=base+uint(v);outputIndices[o+2u]=base+uint(v+1);}}\n";
    return k.str();
}
static std::string glm_clip_pull(const glm_clip_result &r)
{
    std::ostringstream v;v<<"#version 430\n#define GLM_NO_POINT_SIZE\n#define GLM_RAW_VERTEX_ID\n"
        <<"layout(std430,set=1,binding=25) readonly buffer GLMGsVertices {float words[];};\n";
    for(int i=0;i<r.varying_count;++i){const auto &a=r.varyings[i];v<<(a.interpolation==1?"flat ":a.interpolation==2?"noperspective ":"")<<"out "<<glm_clip_type(a)<<" "<<a.name<<";\n";}
    if(r.primitive_id)v<<"flat out int glm_clip_primitive;\n";
    v<<"void main(){uint i=uint(gl_VertexID)*"<<r.out_stride<<"u;gl_Position="<<read_value("words","i","vec4",0)<<";\n";
    for(int c=0;c<r.clip_count;++c)v<<"gl_ClipDistance["<<c<<"]=words[i+"<<4+c<<"u];\n";
    for(int c=0;c<r.varying_count;++c){const auto &a=r.varyings[c];v<<a.name<<"="<<read_value("words","i",glm_clip_type(a),a.offset)<<";\n";}
    if(r.primitive_id)v<<"glm_clip_primitive=floatBitsToInt(words[i+"<<r.vs_stride<<"u]);\n";
    v<<"}\n";return v.str();
}
static void glm_clip_emulation(const glm_compile_request *req,glm_compile_result *result,bool allow_fp64)
{
    auto *r=result->clip;if(!r)return;
    std::vector<std::string> names={"gl_Position"};
    for(int i=0;i<r->clip_count;++i)names.push_back("gl_ClipDistance["+std::to_string(i)+"]");
    for(int i=0;i<r->varying_count;++i)names.push_back(r->varyings[i].name);
    std::vector<const char *> pointers;for(auto &n:names)pointers.push_back(n.c_str());
    glm_compile_request request=*req;request.feedback_varyings=pointers.data();request.feedback_count=int(pointers.size());request.feedback_interleaved=true;
    glm_compile_result capture={};compile_program(&request,&capture,true,allow_fp64);
    if(capture.ok&&capture.xfb_stride[0]==r->vs_stride&&uniform_storage_compatible(*result,capture)){r->vs_capture=capture.msl[GLM_STAGE_VERTEX];capture.msl[GLM_STAGE_VERTEX]=nullptr;}
    glm_compile_result_free(&capture);
    if(!r->vs_capture)return;
    std::string kernel=glm_clip_kernel(*r),pull=glm_clip_pull(*r),fragment=req->sources[GLM_STAGE_FRAGMENT];
    if(r->primitive_id){const char *from[]={"gl_PrimitiveID"},*to[]={"glm_clip_primitive"};char *text=glm_glsl_rename(fragment.c_str(),from,to,1);fragment=text;free(text);
        std::string profile;int version=take_version(fragment,&profile);fragment="#version "+std::to_string(std::max(version,430))+"\nflat in int glm_clip_primitive;\n"+fragment;}
    request={};request.sources[GLM_STAGE_COMPUTE]=kernel.c_str();r->kernel=static_cast<glm_compile_result *>(calloc(1,sizeof(glm_compile_result)));compile_program(&request,r->kernel,false,false);
    request=*req;for(auto &s:request.sources)s=nullptr;request.sources[GLM_STAGE_VERTEX]=pull.c_str();request.sources[GLM_STAGE_FRAGMENT]=fragment.c_str();request.feedback_count=0;request.attribute_count=0;
    r->pull=static_cast<glm_compile_result *>(calloc(1,sizeof(glm_compile_result)));compile_program(&request,r->pull,false,allow_fp64);
    if(!uniform_storage_compatible(*result,*r->pull))r->pull->ok=false;
    // Reflection on generated pull stages is not a second emulation request.
    if(r->pull->clip){auto *nested=r->pull->clip;r->pull->clip=nullptr;for(int i=0;i<nested->varying_count;++i)free(nested->varyings[i].name);free(nested->varyings);free(nested);}
}
#endif
