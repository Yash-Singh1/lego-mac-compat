// GLSL to MSL: legacy-source preparation, glslang (GLSL -> SPIR-V) and
// SPIRV-Cross (SPIR-V -> MSL), with the reflection GL's program queries need.
#include "shader_compiler.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_cross/GLSL.std.450.h>
#include <glslang/MachineIndependent/localintermediate.h>
#include <glslang/MachineIndependent/iomapper.h>
#include <spirv_cross/spirv_msl.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <regex>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::once_flag glslang_once;
void initialize() { std::call_once(glslang_once, [] { glslang::InitializeProcess(); }); }

char *copy(const std::string &s)
{
    char *out = static_cast<char *>(malloc(s.size() + 1));
    memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

EShLanguage language(glm_stage stage)
{
    switch (stage) {
    case GLM_STAGE_VERTEX: return EShLangVertex;
    case GLM_STAGE_FRAGMENT: return EShLangFragment;
    case GLM_STAGE_GEOMETRY: return EShLangGeometry;
    case GLM_STAGE_TESS_CONTROL: return EShLangTessControl;
    case GLM_STAGE_TESS_EVALUATION: return EShLangTessEvaluation;
    case GLM_STAGE_COMPUTE: return EShLangCompute;
    default: return EShLangVertex;
    }
}

// ---- legacy state block ----------------------------------------------------

const char legacy_block_glsl[] =
    "struct glm_LightSourceParameters { vec4 ambient; vec4 diffuse; vec4 specular; vec4 position; vec4 halfVector;\n"
    "  vec3 spotDirection; float spotExponent; float spotCutoff; float spotCosCutoff; float constantAttenuation;\n"
    "  float linearAttenuation; float quadraticAttenuation; };\n"
    "struct glm_MaterialParameters { vec4 emission; vec4 ambient; vec4 diffuse; vec4 specular; float shininess; };\n"
    "struct glm_LightProducts { vec4 ambient; vec4 diffuse; vec4 specular; };\n"
    "struct glm_LightModelParameters { vec4 ambient; };\n"
    "struct glm_LightModelProducts { vec4 sceneColor; };\n"
    "struct glm_FogParameters { vec4 color; float density; float start; float end; float scale; };\n"
    "struct glm_DepthRangeParameters { float znear; float zfar; float diff; };\n"
    "struct glm_PointParameters { float size; float sizeMin; float sizeMax; float fadeThresholdSize;\n"
    "  float distanceConstantAttenuation; float distanceLinearAttenuation; float distanceQuadraticAttenuation; };\n"
    "layout(std140, set = 1, binding = 14) uniform GLMLegacy {\n"
    "  mat4 glm_ModelViewMatrix, glm_ProjectionMatrix, glm_ModelViewProjectionMatrix;\n"
    "  mat4 glm_ModelViewMatrixInverse, glm_ProjectionMatrixInverse, glm_ModelViewProjectionMatrixInverse;\n"
    "  mat4 glm_ModelViewMatrixTranspose, glm_ProjectionMatrixTranspose, glm_ModelViewProjectionMatrixTranspose;\n"
    "  mat4 glm_ModelViewMatrixInverseTranspose, glm_ProjectionMatrixInverseTranspose,\n"
    "       glm_ModelViewProjectionMatrixInverseTranspose;\n"
    "  mat4 glm_TextureMatrix[8], glm_TextureMatrixInverse[8], glm_TextureMatrixTranspose[8],\n"
    "       glm_TextureMatrixInverseTranspose[8];\n"
    "  mat3 glm_NormalMatrix;\n"
    "  vec4 glm_ClipPlane[8];\n"
    "  vec4 glm_ClipPlaneEnabled[2];\n"
    "  vec4 glm_TextureEnvColor[8];\n"
    "  vec4 glm_EyePlaneS[8], glm_EyePlaneT[8], glm_EyePlaneR[8], glm_EyePlaneQ[8];\n"
    "  vec4 glm_ObjectPlaneS[8], glm_ObjectPlaneT[8], glm_ObjectPlaneR[8], glm_ObjectPlaneQ[8];\n"
    "  glm_LightModelParameters glm_LightModel;\n"
    "  glm_LightModelProducts glm_FrontLightModelProduct, glm_BackLightModelProduct;\n"
    "  glm_MaterialParameters glm_FrontMaterial, glm_BackMaterial;\n"
    "  glm_LightSourceParameters glm_LightSource[8];\n"
    "  glm_LightProducts glm_FrontLightProduct[8], glm_BackLightProduct[8];\n"
    "  glm_FogParameters glm_Fog;\n"
    "  glm_DepthRangeParameters glm_DepthRange;\n"
    "  glm_PointParameters glm_Point;\n"
    "  float glm_NormalScale;\n"
    "  float glm_AlphaRef;\n"
    "  float glm_TwoSide;\n"
    "};\n";

// Legacy built-in names rewritten to glm_ ones (either stage).
const std::set<std::string> legacy_state = {
    "gl_ModelViewMatrix", "gl_ProjectionMatrix", "gl_ModelViewProjectionMatrix", "gl_ModelViewMatrixInverse",
    "gl_ProjectionMatrixInverse", "gl_ModelViewProjectionMatrixInverse", "gl_ModelViewMatrixTranspose",
    "gl_ProjectionMatrixTranspose", "gl_ModelViewProjectionMatrixTranspose", "gl_ModelViewMatrixInverseTranspose",
    "gl_ProjectionMatrixInverseTranspose", "gl_ModelViewProjectionMatrixInverseTranspose", "gl_TextureMatrix",
    "gl_TextureMatrixInverse", "gl_TextureMatrixTranspose", "gl_TextureMatrixInverseTranspose", "gl_NormalMatrix",
    "gl_ClipPlane", "gl_TextureEnvColor", "gl_EyePlaneS", "gl_EyePlaneT", "gl_EyePlaneR", "gl_EyePlaneQ",
    "gl_ObjectPlaneS", "gl_ObjectPlaneT", "gl_ObjectPlaneR", "gl_ObjectPlaneQ", "gl_LightModel",
    "gl_FrontLightModelProduct", "gl_BackLightModelProduct", "gl_FrontMaterial", "gl_BackMaterial", "gl_LightSource",
    "gl_FrontLightProduct", "gl_BackLightProduct", "gl_Fog", "gl_DepthRange", "gl_Point", "gl_NormalScale",
};

struct SubroutineUniform {
    std::string name;
    int location = 0, array_size = 1;
    std::vector<int> compatible;
};

// A uniform's initializer (GLSL 1.20+), taken out of the source by
// take_uniform_initializers.
struct UniformInit {
    std::string name, type, array, expr;
};

struct Prepared {
    std::string text;
    std::vector<UniformInit> uniform_inits; // their values: initial_globals
    std::string init_defs;                  // #defines, structs and consts they may use
    std::vector<std::pair<std::string, int>> subroutines; // name, index
    std::vector<SubroutineUniform> subroutine_uniforms;
    uint32_t legacy_parts = 0;
    bool legacy = false;
    bool writes_frag_color = false;
    bool writes_frag_data = false;
};

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string normalize_newlines(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\r') {
            out += '\n';
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
        } else {
            out += in[i];
        }
    }
    return out;
}

// Finds "#version N [profile]"; blanks it so line numbers stay put.
int take_version(std::string &text, std::string *profile)
{
    size_t i = 0;
    while (i < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[i]))) { ++i; continue; }
        if (text.compare(i, 2, "//") == 0) { while (i < text.size() && text[i] != '\n') ++i; continue; }
        if (text.compare(i, 2, "/*") == 0) {
            size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 2;
            continue;
        }
        break;
    }
    if (text.compare(i, 1, "#") != 0) return 110;
    size_t j = i + 1;
    while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
    if (text.compare(j, 7, "version") != 0) return 110;
    size_t end = text.find('\n', j);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(j + 7, end - j - 7);
    // Comments end the directive.
    line = std::regex_replace(line, std::regex(R"(/\*.*?\*/)"), " ");
    if (size_t c = line.find("//"); c != std::string::npos) line.erase(c);
    if (size_t c = line.find("/*"); c != std::string::npos) line.erase(c);
    int version = std::atoi(line.c_str());
    std::istringstream words(line);
    std::string number, prof;
    words >> number >> prof;
    if (profile) *profile = prof;
    for (size_t k = i; k < end; ++k) text[k] = ' ';
    return version ? version : 110;
}

/* Match the required desktop core varying query without changing ES or
   compatibility parsing. Capture preparation may raise the language version,
   so select resources from the original source, not the generated wrapper. */
const TBuiltInResource *shader_resources(const char *source)
{
    static const TBuiltInResource core = [] {
        TBuiltInResource resources = *GetDefaultResources();
        resources.maxVaryingVectors = resources.maxVaryingComponents / 4;
        return resources;
    }();
    std::string text = normalize_newlines(source ? source : ""), profile;
    int version = take_version(text, &profile);
    return version >= 140 && profile != "es" && profile != "compatibility" ? &core : GetDefaultResources();
}

struct Rewrite {
    std::set<std::string> used;
};

// Rewrites identifiers outside comments. `stage` decides attribute/varying.
std::string rewrite_legacy(const std::string &in, glm_stage stage, Rewrite &r)
{
    std::string out;
    out.reserve(in.size() + 256);
    size_t i = 0;
    std::string previous, before_previous;
    while (i < in.size()) {
        char c = in[i];
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            size_t end = in.find('\n', i);
            if (end == std::string::npos) end = in.size();
            out.append(in, i, end - i);
            i = end;
            continue;
        }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            size_t end = in.find("*/", i + 2);
            end = end == std::string::npos ? in.size() : end + 2;
            // Keep newlines so line numbers in logs still match.
            for (size_t k = i; k < end; ++k) out += in[k] == '\n' ? '\n' : ' ';
            i = end;
            continue;
        }
        if (!ident_start(c)) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                before_previous = previous;
                previous = std::string(1, c);
            }
            out += c;
            ++i;
            continue;
        }
        size_t start = i;
        while (i < in.size() && ident_char(in[i])) ++i;
        std::string word = in.substr(start, i - start);
        std::string replacement = word;
        if (word == "attribute" && stage == GLM_STAGE_VERTEX) replacement = "in";
        else if (word == "varying") {
            // EXT_gpu_shader4's "varying out" declares a fragment output.
            size_t k = i;
            while (k < in.size() && std::isspace(static_cast<unsigned char>(in[k]))) ++k;
            bool out_follows = in.compare(k, 3, "out") == 0 && (k + 3 >= in.size() || !ident_char(in[k + 3]));
            replacement = out_follows ? "" : stage == GLM_STAGE_VERTEX ? "out" : "in";
        }
        else if (word == "main") replacement = "glm_user_main";
        else if (word == "ftransform") { replacement = "glm_ftransform"; r.used.insert("gl_ModelViewProjectionMatrix"); r.used.insert("gl_Vertex"); }
        else if (word == "gl_MaxTextureCoords" || word == "gl_MaxLights" || word == "gl_MaxClipPlanes" ||
                 word == "gl_MaxTextureUnits" || word == "gl_MaxClipDistances")
            replacement = "8";
        else if ((word == "near" || word == "far") && previous == "." && before_previous == "glm_DepthRange")
            replacement = word == "near" ? "znear" : "zfar";
        else if (word == "gl_Color" || word == "gl_SecondaryColor") {
            r.used.insert(word);
            replacement = (word == "gl_Color" ? "glm_Color" : "glm_SecondaryColor") +
                          std::string(stage == GLM_STAGE_VERTEX ? "_in" : "_sel");
        } else if (word.rfind("gl_", 0) == 0) {
            static const std::set<std::string> io = {
                "gl_Vertex", "gl_Normal", "gl_FogCoord", "gl_MultiTexCoord0", "gl_MultiTexCoord1", "gl_MultiTexCoord2",
                "gl_MultiTexCoord3", "gl_MultiTexCoord4", "gl_MultiTexCoord5", "gl_MultiTexCoord6", "gl_MultiTexCoord7",
                "gl_FrontColor", "gl_BackColor", "gl_FrontSecondaryColor", "gl_BackSecondaryColor", "gl_TexCoord",
                "gl_FogFragCoord", "gl_ClipVertex", "gl_FragColor", "gl_FragData"};
            if (io.count(word) || legacy_state.count(word)) {
                r.used.insert(word);
                replacement = "glm_" + word.substr(3);
            }
        }
        before_previous = previous;
        previous = replacement;
        out += replacement;
    }
    return out;
}

const char texture_defines[] =
    "#define texture1D(s, c) texture(s, vec2(c, 0.5))\n"
    "#define texture1DProj(s, c) textureProj(s, vec3((c).x, 0.5, (c).w))\n"
    "#define texture1DLod(s, c, l) textureLod(s, vec2(c, 0.5), l)\n"
    "#define texture2D texture\n#define texture2DProj textureProj\n#define texture2DLod textureLod\n"
    "#define texture2DProjLod textureProjLod\n#define texture2DRect texture\n#define texture2DRectProj textureProj\n"
    "#define texture3D texture\n#define texture3DProj textureProj\n#define texture3DLod textureLod\n"
    "#define textureCube texture\n#define textureCubeLod textureLod\n"
    // Comparisons read as luminance (Apple's default GL_DEPTH_TEXTURE_MODE).
    "#define glm_lum(r) vec4(vec3(r), 1.0)\n"
    "#define shadow2D(s, c) glm_lum(texture(s, c))\n#define shadow2DProj(s, c) glm_lum(textureProj(s, c))\n"
    "#define shadow2DRect(s, c) glm_lum(texture(s, c))\n#define shadow2DLod(s, c, l) glm_lum(textureLod(s, c, l))\n"
    "#define shadow2DProjLod(s, c, l) glm_lum(textureProjLod(s, c, l))\n"
    "#define shadow1D(s, c) glm_lum(texture(s, vec3((c).x, 0.5, (c).z)))\n"
    "#define shadow1DProj(s, c) glm_lum(textureProj(s, vec4((c).x, 0.5, (c).z, (c).w)))\n"
    "#define shadow1DLod(s, c, l) glm_lum(textureLod(s, vec3((c).x, 0.5, (c).z), l))\n"
    "#define sampler1D sampler2D\n#define sampler1DShadow sampler2DShadow\n"
    // EXT_gpu_shader4 spellings.
    "#define texelFetch1D(s, c, l) texelFetch(s, ivec2(c, 0), l)\n#define texelFetch2D texelFetch\n"
    "#define texelFetch3D texelFetch\n#define texelFetch2DRect texelFetch\n#define texelFetch2DArray texelFetch\n"
    "#define texelFetch1DArray texelFetch\n#define texelFetchBuffer texelFetch\n"
    "#define textureSize1D(s, l) textureSize(s, l).x\n#define textureSize2D textureSize\n#define textureSize3D textureSize\n"
    "#define textureSizeCube textureSize\n#define textureSize2DRect textureSize\n#define textureSize2DArray textureSize\n"
    "#define textureSizeBuffer textureSize\n#define texture2DArray texture\n#define texture2DArrayLod textureLod\n"
    "#define texture1DArray texture\n#define shadow2DArray(s, c) glm_lum(texture(s, c))\n"
    "#define texture2DGrad textureGrad\n#define texture2DProjGrad textureProjGrad\n#define texture3DGrad textureGrad\n"
    "#define textureCubeGrad textureGrad\n#define texture2DRectGrad textureGrad\n#define texture2DLodOffset textureLodOffset\n"
    "#define texture2DOffset textureOffset\n#define shadow2DRectProj(s, c) glm_lum(textureProj(s, c))\n"
    "#define shadowCube(s, c) glm_lum(texture(s, c))\n";

// Locations a variable of `type` occupies (matrices: one per column).
int location_size(const std::string &type, int array)
{
    int per = 1;
    if (type.size() >= 4 && (type.compare(0, 3, "mat") == 0 || type.compare(0, 4, "dmat") == 0)) {
        char columns = type[type.find("mat") + 3];
        per = columns >= '2' && columns <= '4' ? columns - '0' : 1;
    }
    return per * (array > 0 ? array : 1);
}

// Gives every top-level `storage` declaration an explicit location: bound
// names at their bound location, the rest at the lowest free ones, as GL's
// linker does. Declarations naming several variables are split.
std::string assign_locations(const std::string &text, const char *storage, const std::map<std::string, int> &bound,
                             std::set<int> reserved, std::map<std::string, int> *assigned = nullptr)
{
    // Bound locations may carry a dual-source index: GLM_OUTPUT_INDEX1 | location.
    for (auto &b : bound) reserved.insert(b.second & 0xffff);
    // Explicit layout(location = N) declarations keep their slots (the
    // regex scan is slow: only when there are any).
    if (text.find("location") != std::string::npos) {
        static const std::regex explicit_location(
            R"(layout\s*\(([^)]*)\)\s*((?:\w+\s+)*?)(in|out)\s+(\w+)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?)");
        static const std::regex location_value(R"(location\s*=\s*(\d+))");
        for (std::sregex_iterator it(text.begin(), text.end(), explicit_location), end; it != end; ++it) {
            if ((*it)[3] != storage) continue;
            std::string qualifiers = (*it)[1];
            std::smatch value;
            if (!std::regex_search(qualifiers, value, location_value)) continue;
            int base = std::atoi(value[1].str().c_str());
            int size = location_size((*it)[4], (*it)[6].matched ? std::atoi((*it)[6].str().c_str()) : 0);
            for (int k = 0; k < size; ++k) reserved.insert(base + k);
        }
    }
    std::string out;
    size_t i = 0;
    int depth = 0, parentheses = 0;
    bool line_start = true;
    auto next_free = [&](int size) {
        for (int base = 0;; ++base) {
            bool ok = true;
            for (int k = 0; k < size; ++k) ok = ok && !reserved.count(base + k);
            if (ok) {
                for (int k = 0; k < size; ++k) reserved.insert(base + k);
                return base;
            }
        }
    };
    while (i < text.size()) {
        char c = text[i];
        if (c == '#' && line_start) {
            size_t end = text.find('\n', i);
            end = end == std::string::npos ? text.size() : end;
            out.append(text, i, end - i);
            i = end;
            continue;
        }
        if (c == '\n') line_start = true;
        else if (!std::isspace(static_cast<unsigned char>(c))) line_start = false;
        if (c == '/' && i + 1 < text.size() && (text[i + 1] == '/' || text[i + 1] == '*')) {
            size_t end = text[i + 1] == '/' ? text.find('\n', i) : text.find("*/", i + 2);
            end = end == std::string::npos ? text.size() : (text[i + 1] == '/' ? end : end + 2);
            out.append(text, i, end - i);
            i = end;
            continue;
        }
        if (c == '(') ++parentheses;
        if (c == ')') --parentheses;
        if (c == '{') ++depth;
        if (c == '}') --depth;
        if (depth == 0 && parentheses == 0 && text.compare(i, 6, "layout") == 0 && (i == 0 || !ident_char(text[i - 1])) &&
            (i + 6 >= text.size() || !ident_char(text[i + 6]))) {
            // Already qualified: copy the declaration unchanged.
            size_t end = text.find(';', i);
            end = end == std::string::npos ? text.size() : end + 1;
            out.append(text, i, end - i);
            i = end;
            continue;
        }
        if (depth == 0 && parentheses == 0 && ident_start(c) && (i == 0 || !ident_char(text[i - 1]))) {
            // Look at the whole statement starting here.
            size_t end = text.find(';', i);
            if (end == std::string::npos) { out.append(text, i, std::string::npos); break; }
            std::string statement = text.substr(i, end - i);
            std::vector<std::string> words;
            {
                std::string word;
                for (char ch : statement) {
                    if (ident_char(ch) || ch == '[' || ch == ']' || ch == ',') {
                        if (ch == ',' || ch == '[' || ch == ']') {
                            if (!word.empty()) words.push_back(word);
                            words.push_back(std::string(1, ch));
                            word.clear();
                        } else word += ch;
                    } else if (!word.empty()) {
                        words.push_back(word);
                        word.clear();
                    }
                }
                if (!word.empty()) words.push_back(word);
            }
            static const std::set<std::string> qualifiers = {"flat", "smooth", "noperspective", "centroid", "highp",
                                                             "mediump", "lowp", "invariant", "sample"};
            size_t w = 0;
            std::string prefix;
            while (w < words.size() && qualifiers.count(words[w])) prefix += words[w++] + " ";
            bool candidate = statement.find('{') == std::string::npos && statement.find('(') == std::string::npos &&
                             w + 2 < words.size() + 1 && w < words.size() && words[w] == storage;
            // Qualifiers may also follow the storage: "in highp vec4 a;".
            size_t t = w + 1;
            std::string after;
            while (t < words.size() && qualifiers.count(words[t])) after += words[t++] + " ";
            if (candidate && t + 1 <= words.size() && t < words.size()) {
                std::string type = words[t];
                std::string rewritten;
                size_t k = t + 1;
                bool ok = true;
                // GLSL 1.20 array types: "varying mat2[3] m;" (size after the type).
                int type_array = 0;
                if (k + 2 < words.size() && words[k] == "[" && words[k + 2] == "]") {
                    type_array = std::atoi(words[k + 1].c_str());
                    k += 3;
                    if (type_array <= 0) ok = false;
                }
                while (ok && k < words.size()) {
                    std::string name = words[k++];
                    int array = type_array;
                    if (k < words.size() && words[k] == "[") {
                        if (k + 2 < words.size() && words[k + 2] == "]") { array = std::atoi(words[k + 1].c_str()); k += 3; }
                        else { ok = false; break; }
                    }
                    if (name.rfind("glm_", 0) == 0) { ok = false; break; }
                    auto found = bound.find(name);
                    int location = found != bound.end() ? found->second : next_free(location_size(type, array));
                    if (assigned) (*assigned)[name] = location;
                    std::string index = location & GLM_OUTPUT_INDEX1 ? ", index=1" : "";
                    location &= 0xffff;
                    rewritten += "layout(location=" + std::to_string(location) + index + ") " + prefix + storage + " " + after + type +
                                 " " + name + (array ? "[" + std::to_string(array) + "]" : "") + ";";
                    if (k < words.size() && words[k] == ",") ++k;
                }
                if (ok && !rewritten.empty()) {
                    out += rewritten;
                    i = end + 1;
                    continue;
                }
            }
            // Not a declaration we rewrite: copy the identifier and go on.
            size_t start = i;
            while (i < text.size() && ident_char(text[i])) ++i;
            out.append(text, start, i - start);
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// Legacy GLSL (1.10/1.20) becomes GLSL 4.10 with declared stand-ins for the
// removed built-ins and a wrapper main that fills in what GL does around
// the user's main.
struct Bindings {
    std::map<std::string, int> attributes, outputs;
    uint32_t uint_inputs = 0, int_inputs = 0; // glm_compile_request's
    std::map<std::string, int> border;        // glm_compile_request's border_samplers: name -> base slot
    // Transform feedback capture (vertex stage): varyings in order.
    std::vector<std::string> feedback;
    bool interleaved = true, capture = false, fp64 = false;
    // User varyings of legacy shaders, after the fixed legacy ones; filled
    // by the vertex stage and read by the fragment stage.
    std::map<std::string, int> varyings;
};

// Transform feedback: code that writes the captured varyings into storage
// buffers (set 1, bindings 17..20) at (first + vertex) * stride.
const char xfb_extension[] = "#extension GL_ARB_shader_storage_buffer_object : enable\n";

std::string xfb_expression(const std::string &name)
{
    static const std::map<std::string, std::string> legacy = {
        {"gl_FrontColor", "glm_FrontColor"}, {"gl_BackColor", "glm_BackColor"},
        {"gl_FrontSecondaryColor", "glm_FrontSecondaryColor"}, {"gl_BackSecondaryColor", "glm_BackSecondaryColor"},
        {"gl_FogFragCoord", "glm_FogFragCoord"}, {"gl_ClipVertex", "glm_ClipVertex"}, {"gl_TexCoord", "glm_TexCoord"}};
    size_t bracket = name.find('[');
    std::string base = name.substr(0, bracket), rest = bracket == std::string::npos ? "" : name.substr(bracket);
    auto found = legacy.find(base);
    return (found != legacy.end() ? found->second : base) + rest;
}

std::string xfb_code(const Bindings &b, const std::string &body)
{
    int buffers = b.interleaved ? 1 : static_cast<int>(b.feedback.size());
    if (b.interleaved)
        buffers += static_cast<int>(std::count(b.feedback.begin(), b.feedback.end(), "gl_NextBuffer"));
    std::ostringstream out;
    for (int k = 0; k < buffers && k < 4; ++k) {
        out << "layout(std430, set = 1, binding = " << 17 + k << ") buffer GLMXfb" << k << " { float glm_xfb" << k
            << "[]; };\n";
        const char *types[] = {"float", "vec2", "vec3", "vec4"};
        for (int n = 0; n < 4; ++n) {
            out << "void glm_xfb_put" << k << "(inout uint i, " << types[n] << " v) {";
            for (int c = 0; c < n + 1; ++c) out << " glm_xfb" << k << "[i++] = v" << (n ? std::string(".") + "xyzw"[c] : "") << ";";
            out << " }\n";
        }
        if (b.fp64) {
            const char *doubles[] = {"double", "dvec2", "dvec3", "dvec4"};
            for (int n = 0; n < 4; ++n) {
                out << "void glm_xfb_put" << k << "(inout uint i, " << doubles[n] << " v) { i = (i + 1u) & ~1u;";
                for (int c = 0; c < n + 1; ++c)
                    out << " { uvec2 words = unpackDouble2x32(v" << (n ? std::string(".") + "xyzw"[c] : "")
                        << "); glm_xfb" << k << "[i++] = uintBitsToFloat(words.x); glm_xfb" << k
                        << "[i++] = uintBitsToFloat(words.y); }";
                out << " }\n";
            }
            for (int cols = 2; cols <= 4; ++cols)
                for (int rows = 2; rows <= 4; ++rows) {
                    std::string type = "dmat" + std::to_string(cols) + (cols == rows ? "" : "x" + std::to_string(rows));
                    out << "void glm_xfb_put" << k << "(inout uint i, " << type << " v) {";
                    for (int c = 0; c < cols; ++c) out << " glm_xfb_put" << k << "(i, v[" << c << "]);";
                    out << " }\n";
                }
        }
        const char *ints[] = {"int", "ivec2", "ivec3", "ivec4", "uint", "uvec2", "uvec3", "uvec4"};
        for (int n = 0; n < 8; ++n) {
            bool u = n >= 4;
            int size = n % 4 + 1;
            out << "void glm_xfb_put" << k << "(inout uint i, " << ints[n] << " v) {";
            for (int c = 0; c < size; ++c)
                out << " glm_xfb" << k << "[i++] = " << (u ? "uintBitsToFloat" : "intBitsToFloat") << "(v"
                    << (size > 1 ? std::string(".") + "xyzw"[c] : "") << ");";
            out << " }\n";
        }
        for (int cols = 2; cols <= 4; ++cols)
            for (int rows = 2; rows <= 4; ++rows) {
                std::string type = cols == rows ? "mat" + std::to_string(cols)
                                                : "mat" + std::to_string(cols) + "x" + std::to_string(rows);
                out << "void glm_xfb_put" << k << "(inout uint i, " << type << " v) {";
                for (int c = 0; c < cols; ++c) out << " glm_xfb_put" << k << "(i, v[" << c << "]);";
                out << " }\n";
            }
    }
    out << "layout(std140, set = 1, binding = 21) uniform GLMXfbInfo { uint glm_xfb_first; uint glm_xfb_stride[4]; };\n";
    out << "void glm_xfb_capture() {\n  uint vertex = glm_xfb_first + uint(gl_VertexID);\n";
    if (b.interleaved) out << "  uint i = vertex * glm_xfb_stride[0];\n";
    int interleaved_buffer = 0;
    for (size_t v = 0; v < b.feedback.size(); ++v) {
        const std::string &name = b.feedback[v];
        if (b.interleaved && name == "gl_NextBuffer") {
            ++interleaved_buffer;
            out << "  i = vertex * glm_xfb_stride[" << interleaved_buffer << "];\n";
            continue;
        }
        int k = b.interleaved ? interleaved_buffer : static_cast<int>(v);
        if (!b.interleaved) out << "  { uint i = vertex * glm_xfb_stride[" << k << "];\n";
        if (name.rfind("gl_SkipComponents", 0) == 0) {
            out << "  i += " << std::atoi(name.c_str() + 17) << "u;\n";
        } else if (name != "gl_NextBuffer") {
            std::string expr = xfb_expression(name);
            bool whole_array = name.find('[') == std::string::npos &&
                               std::regex_search(body, std::regex("\\b" + expr + "\\s*\\[")) &&
                               expr != "gl_Position";
            if (expr == "glm_TexCoord" || whole_array)
                out << "  for (int k = 0; k < " << expr << ".length(); ++k) glm_xfb_put" << k << "(i, " << expr << "[k]);\n";
            else
                out << "  glm_xfb_put" << k << "(i, " << expr << ");\n";
        }
        if (!b.interleaved) out << "  }\n";
    }
    out << "}\n";
    return out.str();
}

std::string rename_words(const std::string &text, const std::map<std::string, std::string> &renames);
std::string strip_comments(const std::string &in);

// The identifiers a parameter list declares ("in vec3 a, float b[2]" -> a, b).
std::vector<std::string> parameter_names(const std::string &params)
{
    std::vector<std::string> names;
    std::string trimmed = std::regex_replace(params, std::regex(R"(\[[^\]]*\])"), "");
    std::stringstream list(trimmed);
    std::string part;
    while (std::getline(list, part, ',')) {
        std::smatch m;
        if (std::regex_search(part, m, std::regex(R"((\w+)\s*$)")) && m[1] != "void") names.push_back(m[1]);
    }
    return names;
}

// ARB_shader_subroutine to plain GLSL: subroutine uniforms become uint
// uniforms naming the function, calls go through generated switches.
std::string lower_subroutines(const std::string &source, Prepared &p)
{
    if (source.find("subroutine") == std::string::npos) return source;
    std::string text = strip_comments(source);
    text = std::regex_replace(text, std::regex(R"(#\s*extension\s+GL_ARB_shader_subroutine[^\n]*)"), "");
    struct Type { std::string ret, params; };
    std::map<std::string, Type> types;
    std::regex type_decl(R"(\bsubroutine\s+(\w+)\s+(\w+)\s*\(([^)]*)\)\s*;)");
    for (std::sregex_iterator it(text.begin(), text.end(), type_decl), end; it != end; ++it)
        types[(*it)[2]] = {(*it)[1], (*it)[3]};
    text = std::regex_replace(text, type_decl, "");
    // Keep signatures long enough to validate even unused implementations.
    // Lowering away the subroutine types otherwise loses this check.
    auto signature = [](const std::string &params) {
        std::string result, part;
        std::stringstream parts(params);
        while (std::getline(parts, part, ',')) {
            part = std::regex_replace(part, std::regex(R"(\b(const|in|lowp|mediump|highp)\b)"), "");
            std::smatch m;
            if (std::regex_match(part, m, std::regex(R"(\s*((?:(?:out|inout)\s+)?\w+)(?:\s+\w+)?(\s*(?:\[[^\]]*\]\s*)*)\s*)")))
                part = m[1].str() + m[2].str();
            part = std::regex_replace(part, std::regex(R"(\s)"), "");
            if (part == "void" || part.empty()) continue;
            result += part + ";";
        }
        return result;
    };
    // Functions: `[layout(index = N)] subroutine(A, B) ret name(params)`.
    struct Function { std::string name; std::vector<std::string> types; int index; };
    std::vector<Function> functions;
    std::regex function_decl(R"((?:layout\s*\(\s*index\s*=\s*(\d+)\s*\)\s*)?\bsubroutine\s*\(([^)]*)\)\s*(\w+)\s+(\w+)\s*\(([^)]*)\))");
    std::set<int> used;
    for (std::sregex_iterator it(text.begin(), text.end(), function_decl), end; it != end; ++it)
        if ((*it)[1].matched) used.insert(std::stoi((*it)[1]));
    int next = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), function_decl), end; it != end; ++it) {
        Function f;
        f.name = (*it)[4];
        std::stringstream list((*it)[2].str());
        std::string type;
        while (std::getline(list, type, ',')) f.types.push_back(std::regex_replace(type, std::regex(R"(\s)"), ""));
        for (const auto &name : f.types) {
            auto declared = types.find(name);
            if (declared == types.end() || declared->second.ret != (*it)[3] ||
                signature(declared->second.params) != signature((*it)[5]))
                return "#error subroutine implementation does not match its declared type\n";
        }
        if ((*it)[1].matched) f.index = std::stoi((*it)[1]);
        else {
            while (used.count(next)) ++next;
            f.index = next++;
        }
        functions.push_back(f);
        p.subroutines.push_back({f.name, f.index});
    }
    text = std::regex_replace(text, function_decl, "$3 $4($5)");
    // Uniforms.
    std::regex uniform_decl(R"((?:layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*)?\bsubroutine\s+uniform\s+(\w+)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;)");
    std::map<std::string, std::string> uniform_types;
    std::string dispatchers, out;
    int location = 0;
    std::smatch m;
    std::string rest = text;
    while (std::regex_search(rest, m, uniform_decl)) {
        out += m.prefix();
        std::string type = m[2], name = m[3];
        int size = m[4].matched ? std::stoi(m[4]) : 1;
        uniform_types[name] = type;
        SubroutineUniform u;
        u.name = name;
        u.location = m[1].matched ? std::stoi(m[1]) : location;
        u.array_size = size;
        location = u.location + size;
        for (auto &f : functions)
            if (std::find(f.types.begin(), f.types.end(), type) != f.types.end()) u.compatible.push_back(f.index);
        p.subroutine_uniforms.push_back(u);
        const Type &t = types[type];
        std::string index_param = m[4].matched ? "int glm_i" : "";
        std::string params = t.params;
        if (std::regex_match(params, std::regex(R"(\s*(void)?\s*)"))) params = "";
        // Type declarations may omit parameter names. The dispatcher's
        // definition needs names to forward those arguments to implementations.
        std::stringstream parts(params);
        std::string part, named_params;
        int parameter = 0;
        while (std::getline(parts, part, ',')) {
            std::smatch parameter_match;
            if (std::regex_match(part, parameter_match,
                    std::regex(R"(\s*((?:(?:const|in|out|inout|lowp|mediump|highp)\s+)*\w+)(?:\s+(\w+))?(\s*(?:\[[^\]]*\]\s*)*)\s*)"))) {
                std::string name = parameter_match[2].matched ? parameter_match[2].str()
                                                              : "glm_arg_" + std::to_string(parameter);
                part = parameter_match[1].str() + " " + name + parameter_match[3].str();
            }
            named_params += (named_params.empty() ? "" : ", ") + part;
            ++parameter;
        }
        params = named_params;
        std::string all = index_param + (!index_param.empty() && !params.empty() ? ", " : "") + params;
        std::string selector = "glm_sub_" + name + (m[4].matched ? "[glm_i]" : "");
        out += "uniform uint glm_sub_" + name + (m[4].matched ? "[" + std::to_string(size) + "]" : "") + ";\n" + t.ret +
               " glm_call_" + name + "(" + all + ");\n";
        std::string args;
        for (auto &n : parameter_names(params)) args += (args.empty() ? "" : ", ") + n;
        std::ostringstream d;
        d << t.ret << " glm_call_" << name << "(" << all << ") {\n  switch (" << selector << ") {\n";
        std::string first;
        for (auto &f : functions) {
            if (std::find(f.types.begin(), f.types.end(), type) == f.types.end()) continue;
            if (first.empty()) first = f.name;
            d << "  case " << f.index << "u: " << (t.ret == "void" ? "" : "return ") << f.name << "(" << args << ");"
              << (t.ret == "void" ? " return;" : "") << "\n";
        }
        d << "  }\n";
        if (!first.empty()) d << "  " << (t.ret == "void" ? "" : "return ") << first << "(" << args << ");\n";
        d << "}\n";
        dispatchers += d.str();
        rest = m.suffix();
    }
    out += rest;
    // Calls: name(...) and name[i](...).
    std::string result;
    for (size_t i = 0; i < out.size();) {
        if (ident_start(out[i]) && (i == 0 || !ident_char(out[i - 1]))) {
            size_t start = i;
            while (i < out.size() && ident_char(out[i])) ++i;
            std::string word = out.substr(start, i - start);
            size_t prev = result.find_last_not_of(" \t\n");
            bool member = prev != std::string::npos && result[prev] == '.';
            if (member || !uniform_types.count(word)) {
                result += word;
                continue;
            }
            size_t j = i;
            while (j < out.size() && std::isspace(static_cast<unsigned char>(out[j]))) ++j;
            std::string index;
            if (j < out.size() && out[j] == '[') {
                int depth = 0;
                size_t k = j;
                for (; k < out.size(); ++k) {
                    if (out[k] == '[') ++depth;
                    else if (out[k] == ']' && --depth == 0) break;
                }
                index = out.substr(j + 1, k - j - 1);
                j = k + 1;
                while (j < out.size() && std::isspace(static_cast<unsigned char>(out[j]))) ++j;
            }
            if (j < out.size() && out[j] == '(') {
                size_t close = j + 1;
                while (close < out.size() && std::isspace(static_cast<unsigned char>(out[close]))) ++close;
                bool empty = close < out.size() && out[close] == ')';
                result += "glm_call_" + word + "(" + index + (index.empty() || empty ? "" : ", ");
                i = j + 1;
                continue;
            }
            result += word;
            continue;
        }
        result += out[i++];
    }
    return result + "\n" + dispatchers;
}

// Metal has no 64-bit floats: double types become float ones (with the
// LF literal suffixes and the fp64 extension line dropped).
std::string without_doubles(const std::string &text)
{
    if (text.find("double") == std::string::npos && text.find("dvec") == std::string::npos &&
        text.find("dmat") == std::string::npos && text.find("lf") == std::string::npos &&
        text.find("LF") == std::string::npos)
        return text;
    std::string out = std::regex_replace(text, std::regex(R"(#\s*extension\s+GL_ARB_gpu_shader_fp64[^\n]*)"), "");
    out = std::regex_replace(out, std::regex(R"(\b([0-9]+\.?[0-9]*(?:[eE][-+]?[0-9]+)?|\.[0-9]+(?:[eE][-+]?[0-9]+)?)(lf|LF)\b)"), "$1");
    static const std::map<std::string, std::string> renames = {
        {"double", "float"}, {"dvec2", "vec2"}, {"dvec3", "vec3"}, {"dvec4", "vec4"}, {"dmat2", "mat2"}, {"dmat3", "mat3"},
        {"dmat4", "mat4"}, {"dmat2x2", "mat2"}, {"dmat3x3", "mat3"}, {"dmat4x4", "mat4"}, {"dmat2x3", "mat2x3"},
        {"dmat2x4", "mat2x4"}, {"dmat3x2", "mat3x2"}, {"dmat3x4", "mat3x4"}, {"dmat4x2", "mat4x2"}, {"dmat4x3", "mat4x3"}};
    return rename_words(out, renames);
}

// Rows are rendered bottom-up (Y flipped), so Metal's point_coord has its
// origin at GL's lower left: GL_UPPER_LEFT (the default) flips it.
const char *const point_coord_code =
    "layout(std140, set = 1, binding = 15) uniform GLMPoint { float glm_point_size; float glm_program_point_size; float glm_point_upper; uint glm_vertex_id_base; uint glm_vertex_id_offset; uint glm_vertex_id_mapped; };\n"
    "vec2 glm_point_coord() { return vec2(gl_PointCoord.x, glm_point_upper > 0.5 ? 1.0 - gl_PointCoord.y : gl_PointCoord.y); }\n";

std::string point_coord_flip(const std::string &text)
{
    return rename_words(text, {{"gl_PointCoord", "glm_point_coord()"}});
}

std::vector<UniformInit> take_uniform_initializers(std::string &text, std::string &defs);

// Arrays of matrices as stage inputs/outputs (MSL has none): flat arrays of
// their column vectors, copied to/from a private array of the matrices
// around the shader's main. Locations are unchanged (one per column).
std::string flatten_matrix_array_io(const std::string &text, glm_stage stage)
{
    const char *storage = stage == GLM_STAGE_VERTEX ? "out" : "in";
    if (text.find("mat") == std::string::npos) return text;
    static const std::regex declaration(
        R"(((?:layout\s*\([^)]*\)\s*)?)((?:\w+\s+)*?)\b(in|out)\s+(mat([234])(?:x([234]))?)\s+(\w+)\s*\[\s*(\d+)\s*\]\s*;)");
    std::string out, copies_in, copies_out;
    size_t last = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), declaration), end; it != end; ++it) {
        const std::smatch &m = *it;
        if (m[3] != storage || m[7].str().rfind("glm_", 0) == 0) continue;
        int columns = std::atoi(m[5].str().c_str()), rows = m[6].matched ? std::atoi(m[6].str().c_str()) : columns;
        int count = std::atoi(m[8].str().c_str());
        std::string name = m[7], flat = "glm_flat_" + name;
        out.append(text, last, static_cast<size_t>(m.position(0)) - last);
        out += m[1].str() + m[2].str() + storage + " vec" + std::to_string(rows) + " " + flat + "[" +
               std::to_string(count * columns) + "];\n" + m[4].str() + " " + name + "[" + std::to_string(count) + "];";
        last = static_cast<size_t>(m.position(0) + m.length(0));
        for (int k = 0; k < count; ++k)
            for (int c = 0; c < columns; ++c) {
                std::string element = name + "[" + std::to_string(k) + "][" + std::to_string(c) + "]";
                std::string slot = flat + "[" + std::to_string(k * columns + c) + "]";
                if (stage == GLM_STAGE_VERTEX) copies_out += "  " + slot + " = " + element + ";\n";
                else copies_in += "  " + element + " = " + slot + ";\n";
            }
    }
    if (copies_in.empty() && copies_out.empty()) return text;
    out.append(text, last, std::string::npos);
    static const std::regex main_definition(R"(\bvoid\s+main\s*\()");
    out = std::regex_replace(out, main_definition, "void glm_matio_main(");
    return out + "\nvoid main() {\n" + copies_in + "  glm_matio_main();\n" + copies_out + "}\n";
}

Prepared prepare_stage(const char *source, glm_stage stage, Bindings *bindings);

// Words glslang reserves for Vulkan GLSL (separate textures and samplers,
// subpass inputs) that are ordinary identifiers in OpenGL's GLSL: renamed
// glm_kw_<word> unless called (texture1D() is a legacy built-in), and the
// prefix dropped again from every name the program reports.
const char *const kw_prefix = "glm_kw_";
std::string rename_vulkan_keywords(const std::string &text)
{
    static const std::set<std::string> words = [] {
        std::set<std::string> w = {"sampler", "samplerShadow"};
        for (const char *p : {"", "i", "u"}) {
            for (const char *t : {"texture1D", "texture2D", "texture3D", "textureCube", "texture1DArray", "texture2DArray",
                                  "textureCubeArray", "texture2DRect", "textureBuffer", "texture2DMS", "texture2DMSArray"})
                w.insert(std::string(p) + t);
            w.insert(std::string(p) + "subpassInput");
            w.insert(std::string(p) + "subpassInputMS");
        }
        return w;
    }();
    if (text.find("sampler") == std::string::npos && text.find("texture") == std::string::npos &&
        text.find("subpassInput") == std::string::npos)
        return text;
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (ident_start(text[i]) && (i == 0 || !ident_char(text[i - 1]))) {
            size_t start = i;
            while (i < text.size() && ident_char(text[i])) ++i;
            std::string word = text.substr(start, i - start);
            size_t next = text.find_first_not_of(" \t\n", i);
            bool call = next != std::string::npos && text[next] == '(';
            size_t prev = out.find_last_not_of(" \t\n");
            bool member = prev != std::string::npos && out[prev] == '.';
            out += words.count(word) && !call && !member ? kw_prefix + word : word;
            continue;
        }
        out += text[i++];
    }
    return out;
}

// Drops kw_prefix from a reported name ("glm_kw_sampler[0]" -> "sampler[0]").
void strip_kw_prefix(char *name)
{
    if (!name) return;
    size_t n = strlen(kw_prefix);
    for (char *p = strstr(name, kw_prefix); p; p = strstr(p, kw_prefix)) memmove(p, p + n, strlen(p + n) + 1);
}

void strip_kw_prefixes(glm_compile_result *r)
{
    for (int i = 0; i < r->uniform_count; ++i) strip_kw_prefix(r->uniforms[i].name);
    for (int i = 0; i < r->block_count; ++i) strip_kw_prefix(r->blocks[i].name);
    for (int i = 0; i < r->block_uniform_count; ++i) strip_kw_prefix(r->block_uniforms[i].name);
    for (int i = 0; i < r->attribute_count; ++i) strip_kw_prefix(r->attributes[i].name);
    for (int i = 0; i < r->output_count; ++i) strip_kw_prefix(r->outputs[i].name);
    for (int i = 0; i < r->xfb_count; ++i) strip_kw_prefix(r->xfb[i].name);
    for (int i = 0; i < r->subroutine_count; ++i) strip_kw_prefix(r->subroutines[i].name);
    for (int i = 0; i < r->subroutine_uniform_count; ++i) strip_kw_prefix(r->subroutine_uniforms[i].name);
    if (r->gs) {
        if (r->gs->kernel) strip_kw_prefixes(r->gs->kernel);
        if (r->gs->pull) strip_kw_prefixes(r->gs->pull);
    }
    if (r->tess) {
        if (r->tess->kernel) strip_kw_prefixes(r->tess->kernel);
        if (r->tess->eval) strip_kw_prefixes(r->tess->eval);
    }
    if (r->clip) {
        if (r->clip->kernel) strip_kw_prefixes(r->clip->kernel);
        if (r->clip->pull) strip_kw_prefixes(r->clip->pull);
    }
}

Prepared prepare(const char *source, glm_stage stage, Bindings *bindings = nullptr)
{
    Prepared p = prepare_stage(source, stage, bindings);
    if (stage == GLM_STAGE_VERTEX || stage == GLM_STAGE_FRAGMENT) p.text = flatten_matrix_array_io(p.text, stage);
    return p;
}

Prepared prepare_stage(const char *source, glm_stage stage, Bindings *bindings)
{
    Prepared p;
    if (stage == GLM_STAGE_COMPUTE) { // generated from geometry/tessellation shaders
        std::string text = source ? source : "";
        if (!bindings || !bindings->fp64) text = without_doubles(text);
        p.text = lower_subroutines(text, p);
        return p;
    }
    std::string text = rename_vulkan_keywords(normalize_newlines(source ? source : ""));
    if (!bindings || !bindings->fp64) text = without_doubles(text);
    text = lower_subroutines(text, p);
    std::string profile;
    int version = take_version(text, &profile);
    if (version >= 120) p.uniform_inits = take_uniform_initializers(text, p.init_defs);
    p.legacy = version < 140 || profile == "compatibility";
    if (!p.legacy) {
        // Vertex shaders still get the point-size wrapper.
        const bool capture = stage == GLM_STAGE_VERTEX && bindings && bindings->capture && !bindings->feedback.empty();
        // SSBO capture is an internal lowering. The ordinary variant retains
        // the user's language version and validates the original program.
        const int internal_version = capture ? std::max(version, 430) : version;
        std::string header = "#version " + std::to_string(internal_version) + (profile.empty() ? "" : " " + profile) + "\n";
        if (version < 330) header += "#extension GL_ARB_explicit_attrib_location : enable\n";
        if (version < 420) header += "#extension GL_ARB_shading_language_420pack : enable\n";
        if (capture) header += xfb_extension;
        if (stage == GLM_STAGE_VERTEX) {
            Rewrite r;
            std::string body;
            // Rename main only.
            size_t i = 0;
            while (i < text.size()) {
                if (ident_start(text[i]) && (i == 0 || !ident_char(text[i - 1]))) {
                    size_t start = i;
                    while (i < text.size() && ident_char(text[i])) ++i;
                    std::string word = text.substr(start, i - start);
                    body += word == "main" ? "glm_user_main" : word;
                    continue;
                }
                body += text[i++];
            }
            const auto raw_body=strip_comments(body);
            bool vertex_id=body.find("#define GLM_RAW_VERTEX_ID")==std::string::npos &&
                rename_words(raw_body,{{"gl_VertexID","glm_user_vertex_id"}})!=raw_body;
            if(vertex_id)body=rename_words(body,{{"gl_VertexID","glm_user_vertex_id"}});
            body = assign_locations(body, "in", bindings ? bindings->attributes : std::map<std::string, int>(), {});
            // Integer-fed float inputs: an integer attribute and a float copy.
            std::string conversions;
            if (bindings && (bindings->uint_inputs | bindings->int_inputs)) {
                std::regex input(R"(layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*in\s+(float|vec[234])\s+(\w+)\s*;)");
                std::string out;
                std::smatch m;
                std::string rest = body;
                while (std::regex_search(rest, m, input)) {
                    out += m.prefix();
                    int location = std::stoi(m[1]);
                    bool is_uint = location < 32 && ((bindings->uint_inputs >> location) & 1);
                    bool is_int = location < 32 && ((bindings->int_inputs >> location) & 1);
                    if (is_uint || is_int) {
                        std::string type = m[2], name = m[3];
                        std::string itype = type == "float" ? (is_uint ? "uint" : "int") : (is_uint ? "u" : "i") + type;
                        out += "layout(location=" + m[1].str() + ") in " + itype + " glm_int_" + name + "; " + type + " " + name + ";";
                        conversions += "  " + name + " = " + type + "(glm_int_" + name + ");\n";
                    } else {
                        out += m[0];
                    }
                    rest = m.suffix();
                }
                body = out + rest;
            }
            // A redeclared gl_PerVertex (separate shader objects) needs
            // gl_PointSize for the point size written below.
            {
                std::smatch m;
                std::regex per_vertex(R"(out\s+gl_PerVertex\s*\{)");
                if (std::regex_search(body, m, per_vertex)) {
                    size_t open = static_cast<size_t>(m.position(0) + m.length(0));
                    size_t close = body.find('}', open);
                    if (close != std::string::npos && body.substr(open, close - open).find("gl_PointSize") == std::string::npos)
                        body.insert(close, " float gl_PointSize; ");
                }
            }
            // Generated stages drawing lines or triangles with a pipeline
            // input topology must not write a point size.
            bool point_size = body.find("#define GLM_NO_POINT_SIZE") == std::string::npos;
            p.text = header +
                     "layout(std140, set = 1, binding = 15) uniform GLMPoint { float glm_point_size; float glm_program_point_size; float glm_point_upper; uint glm_vertex_id_base; uint glm_vertex_id_offset; uint glm_vertex_id_mapped; };\n" +
                     (vertex_id ? "#define glm_user_vertex_id (int(uint(gl_VertexID) + glm_vertex_id_base))\n" : "") + "#line 1\n" + body +
                     (capture ? xfb_code(*bindings, body) : std::string()) +
                     "void main() {\n" + conversions +
                     "  glm_user_main();\n" +
                     (point_size ? "  gl_PointSize = glm_program_point_size > 0.5 ? gl_PointSize : glm_point_size;\n" : "") +
                     (capture ? "  glm_xfb_capture();\n" : "") + "}\n";
            return p;
        }
        if (stage == GLM_STAGE_FRAGMENT)
            text = assign_locations(text, "out", bindings ? bindings->outputs : std::map<std::string, int>(), {});
        // interpolateAtOffset from derivatives, as Apple's implementation
        // does: Metal's interpolate_at_offset rounds offsets to 1/16 pixel.
        if (stage == GLM_STAGE_FRAGMENT && text.find("interpolateAtOffset") != std::string::npos) {
            text = rename_words(text, {{"interpolateAtOffset", "glm_interpolate_at_offset"}});
            for (const char *type : {"float", "vec2", "vec3", "vec4"})
                header += std::string(type) + " glm_interpolate_at_offset(" + type + " v, vec2 o) { return v + dFdx(v) * o.x + dFdy(v) * o.y; }\n";
        }
        if (stage == GLM_STAGE_FRAGMENT && text.find("gl_PointCoord") != std::string::npos) {
            text = point_coord_flip(text);
            header += point_coord_code;
        }
        p.text = header + "#line 1\n" + text;
        return p;
    }
    Rewrite r;
    std::string body = rewrite_legacy(text, stage, r);
    const auto raw_body=strip_comments(body);
    bool vertex_id=stage==GLM_STAGE_VERTEX &&
        rename_words(raw_body,{{"gl_VertexID","glm_user_vertex_id"}})!=raw_body;
    if(vertex_id)body=rename_words(body,{{"gl_VertexID","glm_user_vertex_id"}});
    if (stage == GLM_STAGE_VERTEX) {
        // User attributes avoid the slots of the legacy inputs in use.
        std::set<int> reserved;
        if (r.used.count("gl_Vertex")) reserved.insert(0);
        if (r.used.count("gl_Normal")) reserved.insert(2);
        if (r.used.count("gl_Color")) reserved.insert(3);
        if (r.used.count("gl_SecondaryColor")) reserved.insert(4);
        if (r.used.count("gl_FogCoord")) reserved.insert(5);
        for (int u = 0; u < 8; ++u)
            if (r.used.count("gl_MultiTexCoord" + std::to_string(u))) reserved.insert(8 + u);
        body = assign_locations(body, "in", bindings ? bindings->attributes : std::map<std::string, int>(), reserved);
    }
    {
        std::set<int> fixed;
        for (int l = 0; l < GLM_VARYING_USER; ++l) fixed.insert(l);
        std::map<std::string, int> none, recorded;
        const auto &bound = bindings && stage != GLM_STAGE_VERTEX ? bindings->varyings : none;
        if (stage == GLM_STAGE_VERTEX || stage == GLM_STAGE_FRAGMENT)
            body = assign_locations(body, stage == GLM_STAGE_VERTEX ? "out" : "in", bound, fixed, &recorded);
        if (bindings && stage == GLM_STAGE_VERTEX) bindings->varyings = recorded;
        // User fragment outputs ("varying out", EXT_gpu_shader4).
        if (stage == GLM_STAGE_FRAGMENT)
            body = assign_locations(body, "out", bindings ? bindings->outputs : none, {});
    }
    std::ostringstream pre;
    bool capture = stage == GLM_STAGE_VERTEX && bindings && bindings->capture && !bindings->feedback.empty();
    pre << "#version 410\n#extension GL_ARB_shading_language_420pack : enable\n" << (capture ? xfb_extension : "")
        << texture_defines;
    bool state = false;
    for (const auto &name : r.used) {
        if (!legacy_state.count(name)) continue;
        state = true;
        if (name.find("TextureMatrix") != std::string::npos) p.legacy_parts |= GLM_LEGACY_TEXTURE;
        else if (name.find("Matrix") != std::string::npos && name != "gl_NormalMatrix") p.legacy_parts |= GLM_LEGACY_MATRICES;
        else if (name == "gl_NormalMatrix" || name == "gl_NormalScale") p.legacy_parts |= GLM_LEGACY_NORMAL;
        else if (name == "gl_ClipPlane") p.legacy_parts |= GLM_LEGACY_CLIP;
        else if (name == "gl_TextureEnvColor") p.legacy_parts |= GLM_LEGACY_TEXENV;
        else if (name.find("Plane") != std::string::npos) p.legacy_parts |= GLM_LEGACY_TEXGEN;
        else if (name == "gl_Fog") p.legacy_parts |= GLM_LEGACY_FOG;
        else if (name == "gl_DepthRange") p.legacy_parts |= GLM_LEGACY_DEPTH;
        else if (name == "gl_Point") p.legacy_parts |= GLM_LEGACY_POINT;
        else p.legacy_parts |= GLM_LEGACY_LIGHTS;
    }
    if (r.used.count("gl_ClipVertex")) {
        state = true;
        p.legacy_parts |= GLM_LEGACY_CLIP;
    }
    // gl_Color / gl_SecondaryColor in a fragment shader: the back colours on
    // back faces with GL_VERTEX_PROGRAM_TWO_SIDE (glm_TwoSide).
    bool reads_colors = stage == GLM_STAGE_FRAGMENT && (r.used.count("gl_Color") || r.used.count("gl_SecondaryColor"));
    if (reads_colors) {
        state = true;
        p.legacy_parts |= GLM_LEGACY_TWO_SIDE;
    }
    if (state) pre << legacy_block_glsl;
    // Default point size: the small GLMPoint block, not the legacy state.
    if (stage == GLM_STAGE_VERTEX)
        pre << "layout(std140, set = 1, binding = 15) uniform GLMPoint { float glm_point_size; float glm_program_point_size; float glm_point_upper; uint glm_vertex_id_base; uint glm_vertex_id_offset; uint glm_vertex_id_mapped; };\n";
    if (stage == GLM_STAGE_VERTEX) {
        auto use = [&](const char *name) { return r.used.count(name) != 0; };
        if (use("gl_Vertex")) pre << "layout(location=0) in vec4 glm_Vertex;\n";
        if (use("gl_Normal")) pre << "layout(location=2) in vec3 glm_Normal;\n";
        if (use("gl_Color")) pre << "layout(location=3) in vec4 glm_Color_in;\n";
        if (use("gl_SecondaryColor")) pre << "layout(location=4) in vec4 glm_SecondaryColor_in;\n";
        if (use("gl_FogCoord")) pre << "layout(location=5) in float glm_FogCoord;\n";
        for (int u = 0; u < 8; ++u)
            if (use(("gl_MultiTexCoord" + std::to_string(u)).c_str()))
                pre << "layout(location=" << 8 + u << ") in vec4 glm_MultiTexCoord" << u << ";\n";
        // Every legacy varying exists so any fragment stage can pair with it.
        pre << "layout(location=0) out vec4 glm_FrontColor;\nlayout(location=1) out vec4 glm_FrontSecondaryColor;\n"
               "layout(location=2) out vec4 glm_BackColor;\nlayout(location=3) out vec4 glm_BackSecondaryColor;\n"
               "layout(location=4) out float glm_FogFragCoord;\nlayout(location=5) out vec4 glm_TexCoord[8];\n"
               "vec4 glm_ClipVertex;\n";
        if (use("gl_Vertex") && use("gl_ModelViewProjectionMatrix")) pre << "vec4 glm_ftransform() { return glm_ModelViewProjectionMatrix * glm_Vertex; }\n";
        if(vertex_id)pre << "#define glm_user_vertex_id (int(uint(gl_VertexID) + glm_vertex_id_base))\n";
        pre << "#line 1\n" << body << "\n";
        if (capture) pre << xfb_code(*bindings, body);
        pre << "void main() {\n";
        pre << "  glm_FrontColor = vec4(0.0); glm_FrontSecondaryColor = vec4(0.0); glm_BackColor = vec4(0.0);\n"
               "  glm_BackSecondaryColor = vec4(0.0); glm_FogFragCoord = 0.0;\n"
               "  for (int i = 0; i < 8; ++i) glm_TexCoord[i] = vec4(0.0, 0.0, 0.0, 1.0);\n"
               "  glm_ClipVertex = vec4(0.0);\n"
               "  gl_PointSize = glm_point_size;\n"
               "  glm_user_main();\n";
        if (r.used.count("gl_ClipVertex"))
            // Constant indices: gl_ClipDistance is unsized until indexed so.
            for (int i = 0; i < 8; ++i)
                pre << "  gl_ClipDistance[" << i << "] = glm_ClipPlaneEnabled[" << i / 4 << "][" << i % 4
                    << "] > 0.0 ? dot(glm_ClipPlane[" << i << "], glm_ClipVertex) : 1.0;\n";
        if (capture) pre << "  glm_xfb_capture();\n";
        pre << "}\n";
    } else if (stage == GLM_STAGE_FRAGMENT) {
        pre << "layout(location=0) in vec4 glm_Color;\nlayout(location=1) in vec4 glm_SecondaryColor;\n"
               "layout(location=4) in float glm_FogFragCoord;\nlayout(location=5) in vec4 glm_TexCoord[8];\n";
        if (reads_colors)
            pre << "layout(location=2) in vec4 glm_BackColor_in;\nlayout(location=3) in vec4 glm_BackSecondaryColor_in;\n"
                   "vec4 glm_Color_sel, glm_SecondaryColor_sel;\n";
        p.writes_frag_color = r.used.count("gl_FragColor") != 0;
        p.writes_frag_data = r.used.count("gl_FragData") != 0;
        if (p.writes_frag_data) pre << "layout(location=0) out vec4 glm_FragData[8];\n";
        else pre << "layout(location=0) out vec4 glm_FragColor;\n";
        if (body.find("gl_PointCoord") != std::string::npos) {
            body = point_coord_flip(body);
            pre << point_coord_code;
        }
        pre << "#line 1\n" << body << "\nvoid main() {"
            << (reads_colors ? " bool glm_back = glm_TwoSide != 0.0 && !gl_FrontFacing;"
                               " glm_Color_sel = glm_back ? glm_BackColor_in : glm_Color;"
                               " glm_SecondaryColor_sel = glm_back ? glm_BackSecondaryColor_in : glm_SecondaryColor;"
                             : "")
            << " glm_user_main(); }\n";
    } else {
        pre << "#line 1\n" << body << "\nvoid main() { glm_user_main(); }\n";
    }
    p.text = pre.str();
    return p;
}

void configure(glslang::TShader &shader, EShLanguage lang)
{
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    shader.setEnvInputVulkanRulesRelaxed();
    shader.setGlobalUniformBlockName("GLMGlobals");
    shader.setGlobalUniformSet(0);
    shader.setGlobalUniformBinding(0);
    shader.setAutoMapLocations(true);
    shader.setAutoMapBindings(true);
}

const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | EShMsgRelaxedErrors);

// ---- uniform initializers ---------------------------------------------------
// GLSL 1.20+ lets a uniform have an initializer (its value until the
// application sets one). Loose uniforms become members of GLMGlobals, which
// drops initializers, so they are taken out here and their values computed by
// glslang (initial_globals below).
// Removes "= expr" from global uniform declarations in `text`, recording
// them, and collects what the expressions may refer to (#defines, structs,
// global consts) in `defs`.
std::vector<UniformInit> take_uniform_initializers(std::string &text, std::string &defs)
{
    std::vector<UniformInit> inits;
    if (text.find("uniform") == std::string::npos || text.find('=') == std::string::npos) return inits;
    std::string out;
    size_t i = 0;
    int depth = 0;
    bool line_start = true;
    while (i < text.size()) {
        char c = text[i];
        if (c == '#' && line_start) {
            size_t end = text.find('\n', i);
            end = end == std::string::npos ? text.size() : end;
            std::string line = text.substr(i, end - i);
            if (line.find("define") != std::string::npos) defs += line + "\n";
            out += line;
            i = end;
            continue;
        }
        if (c == '\n') line_start = true;
        else if (!std::isspace(static_cast<unsigned char>(c))) line_start = false;
        if (c == '/' && i + 1 < text.size() && (text[i + 1] == '/' || text[i + 1] == '*')) {
            size_t end = text[i + 1] == '/' ? text.find('\n', i) : text.find("*/", i + 2);
            end = end == std::string::npos ? text.size() : (text[i + 1] == '/' ? end : end + 2);
            out.append(text, i, end - i);
            i = end;
            continue;
        }
        if (c == '{') ++depth;
        if (c == '}') --depth;
        if (depth == 0 && ident_start(c) && (i == 0 || !ident_char(text[i - 1]))) {
            size_t word_end = i;
            while (word_end < text.size() && ident_char(text[word_end])) ++word_end;
            std::string word = text.substr(i, word_end - i);
            if (word == "uniform" || word == "const" || word == "struct") {
                // The statement up to its ';' at depth 0 (struct bodies included).
                size_t end = i;
                int d = 0;
                while (end < text.size() && !(text[end] == ';' && d == 0)) {
                    if (text[end] == '{' || text[end] == '(') ++d;
                    if (text[end] == '}' || text[end] == ')') --d;
                    ++end;
                }
                if (end >= text.size()) { out.append(text, i, std::string::npos); break; }
                std::string statement = text.substr(i, end - i);
                if (word != "uniform") {
                    defs += statement + ";\n";
                } else if (statement.find('{') == std::string::npos) {
                    size_t eq = statement.find('=');
                    if (eq != std::string::npos) {
                        std::string left = statement.substr(0, eq), expr = statement.substr(eq + 1);
                        while (!left.empty() && std::isspace(static_cast<unsigned char>(left.back()))) left.pop_back();
                        UniformInit u;
                        size_t bracket = left.size();
                        if (!left.empty() && left.back() == ']') {
                            bracket = left.rfind('[');
                            u.array = left.substr(bracket);
                            left = left.substr(0, bracket);
                            while (!left.empty() && std::isspace(static_cast<unsigned char>(left.back()))) left.pop_back();
                        }
                        size_t name_start = left.size();
                        while (name_start > 0 && ident_char(left[name_start - 1])) --name_start;
                        u.name = left.substr(name_start);
                        u.type = left.substr(7, name_start - 7); // after "uniform"
                        u.expr = expr;
                        if (!u.name.empty()) {
                            inits.push_back(u);
                            out += left + u.array + ";";
                            i = end + 1;
                            continue;
                        }
                    }
                }
                out.append(text, i, end + 1 - i);
                i = end + 1;
                continue;
            }
            out.append(text, i, word_end - i);
            i = word_end;
            continue;
        }
        out += c;
        ++i;
    }
    text = out;
    return inits;
}

// Folds the initializers with glslang: a compute shader writes every
// component's 32 bits to a buffer word, and the stored constants are read
// back from the SPIR-V. Values land at the uniforms' offsets in `globals`.
void initial_globals(const std::vector<UniformInit> &inits, const std::string &defs,
                     const std::map<std::string, glm_uniform_info> &uniforms, std::vector<uint8_t> &globals,
                     const TBuiltInResource *resources)
{
    std::ostringstream src, body;
    src << "#version 450\nlayout(local_size_x = 1) in;\n" << defs;
    struct Slot { int offset; };
    std::vector<Slot> slots;
    for (size_t k = 0; k < inits.size(); ++k) {
        const UniformInit &u = inits[k];
        std::string var = "glm_init_" + std::to_string(k);
        src << "const " << u.type << " " << var << u.array << " = " << u.expr << ";\n";
        for (const auto &entry : uniforms) {
            const std::string &name = entry.first;
            const glm_uniform_info &info = entry.second;
            if (info.offset < 0 || info.type == 0) continue;
            std::string path;
            if (name == u.name) path = var;
            else if (name.compare(0, u.name.size(), u.name) == 0 && (name[u.name.size()] == '.' || name[u.name.size()] == '['))
                path = var + name.substr(u.name.size());
            else continue;
            int rows = 1, columns = 1;
            uint32_t t = info.type;
            int kind = 0; // float, int, uint, bool
            switch (t) {
            case 0x1406: break;                       case 0x8B50: rows = 2; break;
            case 0x8B51: rows = 3; break;             case 0x8B52: rows = 4; break;
            case 0x1404: kind = 1; break;             case 0x8B53: kind = 1; rows = 2; break;
            case 0x8B54: kind = 1; rows = 3; break;   case 0x8B55: kind = 1; rows = 4; break;
            case 0x1405: kind = 2; break;             case 0x8DC6: kind = 2; rows = 2; break;
            case 0x8DC7: kind = 2; rows = 3; break;   case 0x8DC8: kind = 2; rows = 4; break;
            case 0x8B56: kind = 3; break;             case 0x8B57: kind = 3; rows = 2; break;
            case 0x8B58: kind = 3; rows = 3; break;   case 0x8B59: kind = 3; rows = 4; break;
            case 0x8B5A: columns = rows = 2; break;   case 0x8B5B: columns = rows = 3; break;
            case 0x8B5C: columns = rows = 4; break;
            case 0x8B65: columns = 2; rows = 3; break; case 0x8B66: columns = 2; rows = 4; break;
            case 0x8B67: columns = 3; rows = 2; break; case 0x8B68: columns = 3; rows = 4; break;
            case 0x8B69: columns = 4; rows = 2; break; case 0x8B6A: columns = 4; rows = 3; break;
            default: continue;
            }
            int elements = info.array_size > 1 ? info.array_size : 1;
            for (int e = 0; e < elements; ++e) {
                std::string element = info.array_size > 1 ? path + "[" + std::to_string(e) + "]" : path;
                for (int col = 0; col < columns; ++col)
                    for (int row = 0; row < rows; ++row) {
                        std::string comp = element;
                        if (columns > 1) comp += "[" + std::to_string(col) + "][" + std::to_string(row) + "]";
                        else if (rows > 1) comp += "[" + std::to_string(row) + "]";
                        std::string word = kind == 0 ? "floatBitsToUint(" + comp + ")"
                                         : kind == 1 ? "uint(" + comp + ")"
                                         : kind == 2 ? "(" + comp + ")"
                                                     : "((" + comp + ") ? 1u : 0u)";
                        int offset = info.offset + e * info.array_stride + (columns > 1 ? col * info.matrix_stride : 0) + row * 4;
                        body << "  glm_out.w[" << slots.size() << "] = " << word << ";\n";
                        slots.push_back({offset});
                    }
            }
        }
    }
    if (slots.empty()) return;
    src << "layout(std430, set = 0, binding = 0) buffer GLMInitial { uint w[]; } glm_out;\nvoid main() {\n" << body.str() << "}\n";
    std::string text = src.str();
    glslang::TShader shader(EShLangCompute);
    const char *strings = text.c_str();
    shader.setStrings(&strings, 1);
    configure(shader, EShLangCompute);
    if (!shader.parse(resources, 450, false, messages)) return;
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) return;
    std::vector<uint32_t> spirv;
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv, &options);
    // Constants (and bitcasts of them), access chains to glm_out.w[index], stores.
    std::map<uint32_t, uint32_t> values, chains;
    for (size_t at = 5; at < spirv.size();) {
        uint32_t count = spirv[at] >> 16, op = spirv[at] & 0xffff;
        if (count == 0 || at + count > spirv.size()) break;
        const uint32_t *w = &spirv[at];
        if (op == 43 /* OpConstant */ && count >= 4) values[w[2]] = w[3];
        else if (op == 124 /* OpBitcast */ && count >= 4 && values.count(w[3])) values[w[2]] = values[w[3]];
        else if (op == 65 /* OpAccessChain */ && count == 6 && values.count(w[5])) chains[w[2]] = values[w[5]];
        else if (op == 62 /* OpStore */ && count >= 3 && chains.count(w[1]) && values.count(w[2])) {
            uint32_t index = chains[w[1]];
            if (index < slots.size() && slots[index].offset >= 0 &&
                static_cast<size_t>(slots[index].offset) + 4 <= globals.size())
                memcpy(globals.data() + slots[index].offset, &values[w[2]], 4);
        }
        at += count;
    }
}


} // namespace

extern "C" bool glm_shader_check_known(glm_stage stage, const char *source);
extern "C" bool glm_remote_compile(const glm_compile_request *req, glm_compile_result *res);
extern "C" bool glm_remote_check(glm_stage stage, const char *source, bool *ok, char **log);
extern "C" void glm_shader_check_remember(glm_stage stage, const char *source);

static bool shader_check_uncached(glm_stage stage, const char *source, char **log);

/* Directives Apple's preprocessor rejects (build/probes/shaderprobe): tokens
   after #else, #endif and #ifdef / #ifndef NAME, and #if / #elif
   expressions that do not parse. Its info log, or empty. */
static std::string directive_errors(const std::string &source)
{
    std::string out;
    std::set<std::string> function_macros;
    std::map<std::string, std::string> object_macros;
    // Check the expanded expression. A macro may intentionally supply only
    // part of a parenthesized expression, completed at its use site.
    std::function<std::string(const std::string &, std::set<std::string>)> expand;
    expand = [&](const std::string &expression, std::set<std::string> active) {
        std::string result;
        bool defined_operand = false;
        for (size_t pos = 0; pos < expression.size();) {
            if (!ident_start(expression[pos])) {
                result += expression[pos++];
                continue;
            }
            size_t begin = pos++;
            while (pos < expression.size() && ident_char(expression[pos])) ++pos;
            std::string word = expression.substr(begin, pos - begin);
            auto macro = object_macros.find(word);
            if (defined_operand) {
                result += word;
                defined_operand = false;
            } else if (word == "defined") {
                result += word;
                defined_operand = true;
            } else if (macro != object_macros.end() && !active.count(word) && active.size() < 64 && result.size() < 65536) {
                auto nested = active;
                nested.insert(word);
                result += expand(macro->second, nested);
            } else {
                result += word;
            }
        }
        return result;
    };
    bool in_comment = false;
    // Open conditionals: whether a branch is known taken (then later #elif
    // expressions are not evaluated) and whether this one is known skipped.
    struct Group { bool taken, skipping, outer_skipping; };
    std::vector<Group> groups;
    auto skipping = [&] { return !groups.empty() && groups.back().skipping; };
    std::istringstream lines(source);
    std::string raw;
    for (int number = 1; std::getline(lines, raw); ++number) {
        // The line without comments.
        std::string line;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (in_comment) {
                if (raw.compare(i, 2, "*/") == 0) in_comment = false, ++i, line += ' ';
                continue;
            }
            if (raw.compare(i, 2, "//") == 0) break;
            if (raw.compare(i, 2, "/*") == 0) { in_comment = true; ++i; continue; }
            line += raw[i];
        }
        size_t i = line.find_first_not_of(" \t\r");
        if (i == std::string::npos || line[i] != '#') continue;
        i = line.find_first_not_of(" \t", i + 1);
        if (i == std::string::npos) continue;
        size_t e = i;
        while (e < line.size() && (std::isalnum((unsigned char)line[e]) || line[e] == '_')) ++e;
        std::string directive = line.substr(i, e - i), rest = line.substr(e);
        std::string where = "ERROR: 0:" + std::to_string(number) + ": '' : syntax error: ";
        auto trailing = [&](const std::string &label) {
            out += where + "unexpected tokens following " + label + " preprocessor directive - expected a newline\n";
        };
        auto blank = [](const std::string &t) { return t.find_first_not_of(" \t\r") == std::string::npos; };
        if (directive == "define") {
            std::smatch m;
            if (!skipping() && std::regex_match(rest, m, std::regex(R"(^\s+([A-Za-z_]\w*)(.*))"))) {
                std::string name = m[1], replacement = m[2];
                object_macros.erase(name);
                function_macros.erase(name);
                if (!replacement.empty() && replacement[0] == '(') function_macros.insert(name);
                else object_macros[name] = replacement;
            }
        } else if (directive == "undef") {
            std::smatch m;
            if (!skipping() && std::regex_match(rest, m, std::regex(R"(^\s+([A-Za-z_]\w*)\s*)"))) {
                object_macros.erase(m[1]);
                function_macros.erase(m[1]);
            }
        } else if (directive == "else" || directive == "endif") {
            if (!blank(rest)) trailing("#" + directive);
            if (directive == "endif" && !groups.empty()) groups.pop_back();
            else if (!groups.empty()) groups.back().skipping = groups.back().outer_skipping || groups.back().taken;
        } else if (directive == "ifdef" || directive == "ifndef") {
            std::smatch m;
            if (!skipping() && std::regex_match(rest, m, std::regex(R"(\s+[A-Za-z_]\w*(.*))")) && !blank(m[1].str())) trailing("#ifdef");
            groups.push_back({false, skipping(), skipping()});
        } else if ((directive == "if" || directive == "elif") &&
                   (directive == "if" ? skipping() : groups.empty() || groups.back().outer_skipping || groups.back().taken)) {
            // Not evaluated.
            if (directive == "if") groups.push_back({true, true, true});
            else if (!groups.empty()) groups.back().skipping = true;
        } else if (directive == "if" || directive == "elif") {
            rest = expand(rest, {});
            // A literal decides the branch; otherwise it is not known.
            std::smatch lit;
            bool known = std::regex_match(rest, lit, std::regex(R"(\s*(\d+)\s*)"));
            bool value = known && std::stol(lit[1]) != 0;
            if (directive == "if") groups.push_back({false, false, skipping()});
            Group &g = groups.back();
            g.skipping = known && !value;
            g.taken = g.taken || !known || value;
            // Operands and operators alternate; parentheses balance.
            bool operand = true, junk = false, bad = false, stray = false;
            int depth = 0;
            size_t k = 0;
            auto next = [&](std::string &tok) {
                while (k < rest.size() && std::isspace((unsigned char)rest[k])) ++k;
                if (k >= rest.size()) return false;
                size_t b = k;
                if (std::isalnum((unsigned char)rest[k]) || rest[k] == '_') {
                    while (k < rest.size() && (std::isalnum((unsigned char)rest[k]) || rest[k] == '_' || rest[k] == '.')) ++k;
                } else {
                    static const char *two[] = {"||", "&&", "==", "!=", "<=", ">=", "<<", ">>"};
                    k += 1;
                    for (const char *t : two)
                        if (rest.compare(b, 2, t) == 0) k = b + 2;
                }
                tok = rest.substr(b, k - b);
                return true;
            };
            std::string tok;
            while (!junk && !bad && next(tok)) {
                bool word = std::isalnum((unsigned char)tok[0]) || tok[0] == '_';
                if (word) {
                    if (!operand) { junk = true; break; }
                    operand = false;
                    if (tok == "defined") {
                        std::string t;
                        if (!next(t)) { bad = true; break; }
                        if (t == "(") {
                            if (!next(t) || !next(t) || t != ")") bad = true;
                        }
                    } else if (function_macros.count(tok)) {
                        size_t save = k;
                        std::string t;
                        if (next(t) && t == "(") {
                            for (int d = 1; d && next(t);) d += t == "(" ? 1 : t == ")" ? -1 : 0;
                        } else {
                            k = save;
                        }
                    }
                } else if (tok == "(") {
                    if (!operand) { junk = true; break; }
                    ++depth;
                } else if (tok == ")") {
                    if (operand) bad = true;
                    else if (!depth) { stray = true; break; }
                    --depth;
                } else if (operand && (tok == "!" || tok == "~" || tok == "-" || tok == "+")) {
                } else if (std::string("||&&==!=<=>=<<>>+-*/%<>&|^").find(tok) != std::string::npos && tok.size() <= 2) {
                    if (operand) bad = true;
                    operand = true;
                } else {
                    operand = false; // something else: not judged
                    depth = 0;
                    break;
                }
            }
            if (junk) {
                out += where + "incorrect preprocessor directive\n";
                trailing("#if");
            } else if (stray) {
                trailing("#if");
            } else if (bad) {
                out += where + "incorrect preprocessor directive\n";
            } else if (operand || depth) {
                // Found at the newline: reported on the next line.
                out += "ERROR: 0:" + std::to_string(number + 1) + ": '' : syntax error: incorrect preprocessor directive\n";
            }
        }
    }
    return out;
}

/* The GLSL versions Apple's compiler takes per profile (measured with
   build/probes/shaderprobe): legacy 1.10 and 1.20 (and no #version);
   core 1.00, 1.40, 1.50, 3.30, 4.00 and 4.10, "core" or no profile. Returns
   its info log for any other, else NULL. */
extern "C" char *glm_shader_version_error(const char *source, bool core)
{
    std::string text = normalize_newlines(source ? source : ""), profile;
    std::string scan = text;
    int version = take_version(scan, &profile);
    size_t at = text.find("#");
    bool has_version = version != 110 || std::regex_search(text, std::regex(R"(^\s*#\s*version)"));
    int line = 1;
    if (has_version && at != std::string::npos)
        for (size_t i = 0; i < at; ++i) line += text[i] == '\n';
    std::string where = "ERROR: 0:" + std::to_string(line) + ": '' : ";
    // The version line: a number, then at most a profile.
    if (has_version) {
        std::smatch m;
        std::string vline = text.substr(at, text.find('\n', at) - at);
        vline = std::regex_replace(vline, std::regex(R"(/\*.*?\*/)"), " ");
        if (size_t c = vline.find("//"); c != std::string::npos) vline.erase(c);
        std::istringstream words(vline.substr(vline.find("version") + 7));
        std::string number, prof, extra;
        words >> number >> prof >> extra;
        if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos)
            return copy(where + "syntax error: #version\n");
        if (!extra.empty()) return copy("ERROR: 0:" + std::to_string(line) + ": '" + extra + "' : syntax error: syntax error\n");
    }
    std::string directives = directive_errors(text);
    if (!core) {
        if (version == 110 || version == 120) {
            if (!profile.empty()) return copy(where + "syntax error: #version\n");
            return directives.empty() ? nullptr : copy(directives);
        }
        return copy(where + " version '" + std::to_string(version) + "' is not supported\n" +
                    (profile.empty() ? "" : where + "syntax error: #version\n"));
    }
    if (!has_version) return copy("ERROR: 0:1: '' :  #version required and missing.\n");
    switch (version) {
    case 100: case 140: case 150: case 330: case 400: case 410:
        // A profile only after 1.50 and later, and only "core".
        if (!profile.empty() && (profile != "core" || version < 150)) return copy(where + "syntax error: #version\n");
        return directives.empty() ? nullptr : copy(directives);
    }
    return copy(where + " version '" + std::to_string(version) + "' is not supported\n" +
                (profile.empty() ? "" : where + "syntax error: #version\n") + "ERROR: 0:" + std::to_string(line + 1) +
                ": '' :  #version required and missing.\n");
}

/* Successful checks are remembered (compile_cache.cpp): a game recompiles
   the same sources for many programs, and each check is a full parse. */
extern "C" bool glm_shader_check(glm_stage stage, const char *source, char **log)
{
    if (glm_shader_check_known(stage, source)) {
        if (log) *log = copy(std::string());
        return true;
    }
    bool ok;
    if (!glm_remote_check(stage, source, &ok, log)) ok = shader_check_uncached(stage, source, log);
    if (ok) glm_shader_check_remember(stage, source);
    return ok;
}

/* Built-ins glslang has that Apple's compiler does not: mix() of booleans
   (GL_EXT_shader_integer_mix; Apple's integer mix() is the float one through
   conversions). Its info log, or empty. */
static std::string builtin_errors(glslang::TShader &shader)
{
    struct Check : glslang::TIntermTraverser {
        std::string log;
        static std::string name(const glslang::TType &t)
        {
            std::string base = t.getBasicType() == glslang::EbtBool ? "bool" : t.getBasicType() == glslang::EbtInt ? "int"
                               : t.getBasicType() == glslang::EbtUint ? "uint" : "float";
            if (!t.isVector()) return base;
            std::string prefix = base == "bool" ? "b" : base == "int" ? "i" : base == "uint" ? "u" : "";
            return prefix + "vec" + std::to_string(t.getVectorSize());
        }
        bool visitAggregate(glslang::TVisit, glslang::TIntermAggregate *node) override
        {
            glslang::TIntermSequence &args = node->getSequence();
            if (node->getOp() == glslang::EOpMix && args.size() == 3 && log.empty() &&
                args[0]->getAsTyped()->getBasicType() == glslang::EbtBool)
                log = "ERROR: 0:" + std::to_string(node->getLoc().line) + ": No matching function for call to mix(" +
                      name(args[0]->getAsTyped()->getType()) + ", " + name(args[1]->getAsTyped()->getType()) + ", " +
                      name(args[2]->getAsTyped()->getType()) + ")\n";
            return true;
        }
    } check;
    if (shader.getIntermediate() && shader.getIntermediate()->getTreeRoot()) shader.getIntermediate()->getTreeRoot()->traverse(&check);
    return check.log;
}

static bool shader_check_uncached(glm_stage stage, const char *source, char **log)
{
    initialize();
    Prepared p = prepare(source, stage);
    glslang::TShader shader(language(stage));
    const char *text = p.text.c_str();
    shader.setStrings(&text, 1);
    configure(shader, language(stage));
    bool ok = shader.parse(shader_resources(source), 410, false, messages);
    std::string info = shader.getInfoLog();
    if (ok) {
        std::string builtins = builtin_errors(shader);
        if (!builtins.empty()) ok = false, info = builtins;
    }
    if (log) *log = copy(ok ? std::string() : info);
    return ok;
}

namespace {

uint32_t gl_type_of(const spirv_cross::SPIRType &t)
{
    using spirv_cross::SPIRType;
    // GL enums (numeric to avoid GL headers here).
    static const uint32_t floats[] = {0x1406, 0x8B50, 0x8B51, 0x8B52}, squares[] = {0, 0x8B5A, 0x8B5B, 0x8B5C};
    static const uint32_t ints[] = {0x1404, 0x8B53, 0x8B54, 0x8B55}, uints[] = {0x1405, 0x8DC6, 0x8DC7, 0x8DC8};
    static const uint32_t bools[] = {0x8B56, 0x8B57, 0x8B58, 0x8B59};
    uint32_t v = t.vecsize >= 1 && t.vecsize <= 4 ? t.vecsize - 1 : 0;
    if (t.basetype == SPIRType::Double) {
        static const uint32_t doubles[] = {0x140A, 0x8FFC, 0x8FFD, 0x8FFE};
        if (t.columns == 1) return doubles[v];
        if (t.columns == t.vecsize) return 0x8F46 + t.columns - 2;
        if (t.columns == 2) return t.vecsize == 3 ? 0x8F49 : 0x8F4A;
        if (t.columns == 3) return t.vecsize == 2 ? 0x8F4B : 0x8F4C;
        return t.vecsize == 2 ? 0x8F4D : 0x8F4E;
    }
    if (t.basetype == SPIRType::Float) {
        if (t.columns == 1) return floats[v];
        if (t.columns == t.vecsize) return squares[t.columns - 1];
        if (t.columns == 2) return t.vecsize == 3 ? 0x8B65 : 0x8B66;
        if (t.columns == 3) return t.vecsize == 2 ? 0x8B67 : 0x8B68;
        return t.vecsize == 2 ? 0x8B69 : 0x8B6A;
    }
    if (t.basetype == SPIRType::Int) return ints[v];
    if (t.basetype == SPIRType::UInt) return uints[v];
    if (t.basetype == SPIRType::Boolean) return bools[v];
    return 0;
}

uint32_t gl_sampler_type(const spirv_cross::Compiler &reflect, const spirv_cross::SPIRType &t)
{
    using spirv_cross::SPIRType;
    bool shadow = t.image.depth, array = t.image.arrayed;
    auto sampled = reflect.get_type(t.image.type).basetype;
    int kind = sampled == SPIRType::Int ? 1 : sampled == SPIRType::UInt ? 2 : 0; // float, int, uint
    auto pick = [&](uint32_t f, uint32_t i, uint32_t u) { return kind == 1 ? i : kind == 2 ? u : f; };
    switch (t.image.dim) {
    case spv::Dim1D:
        if (array) return shadow ? 0x8DC3 : pick(0x8DC0, 0x8DCE, 0x8DD6);
        return shadow ? 0x8B61 : pick(0x8B5D, 0x8DC9, 0x8DD1);
    case spv::Dim2D:
        if (t.image.ms) return array ? pick(0x910B, 0x910C, 0x910D) : pick(0x9108, 0x9109, 0x910A);
        if (array) return shadow ? 0x8DC4 : pick(0x8DC1, 0x8DCF, 0x8DD7);
        return shadow ? 0x8B62 : pick(0x8B5E, 0x8DCA, 0x8DD2);
    case spv::Dim3D: return pick(0x8B5F, 0x8DCB, 0x8DD3);
    case spv::DimCube:
        if (array) return shadow ? 0x900D : pick(0x900C, 0x900E, 0x900F);
        return shadow ? 0x8DC5 : pick(0x8B60, 0x8DCC, 0x8DD4);
    case spv::DimRect: return shadow ? 0x8B64 : pick(0x8B63, 0x8DCD, 0x8DD5);
    case spv::DimBuffer: return pick(0x8DC2, 0x8DD0, 0x8DD8);
    default: return 0x8B5E;
    }
}

// Metal has no rectangle textures: they become 2D images sampled with
// unnormalized-coordinate samplers (see sampler_for), so only the SPIR-V
// type changes. Reflection keeps using the original words.
std::vector<uint32_t> without_rect_images(const std::vector<uint32_t> &words)
{
    std::vector<uint32_t> out = words;
    for (size_t i = 5; i < out.size();) {
        uint32_t count = out[i] >> 16, opcode = out[i] & 0xffff;
        if (!count) break;
        if (opcode == spv::OpTypeImage && i + 3 < out.size() && out[i + 3] == spv::DimRect) out[i + 3] = spv::Dim2D;
        if (opcode == spv::OpCapability && i + 1 < out.size() &&
            (out[i + 1] == spv::CapabilitySampledRect || out[i + 1] == spv::CapabilityImageRect))
            out[i + 1] = spv::CapabilityShader;
        i += count;
    }
    return out;
}

} // namespace

namespace {

// Floats one captured value of `type` occupies, and its GL type.
struct BlockMember {
    glm_uniform_info info;
    std::string block;
};

// Flattens a uniform block's members (structs and their arrays expanded)
// into GL active uniforms.
void reflect_members(const spirv_cross::Compiler &reflect, const spirv_cross::SPIRType &type, const std::string &prefix,
                     int base, const std::string &block, std::vector<std::pair<std::string, BlockMember>> &out)
{
    for (uint32_t m = 0; m < type.member_types.size(); ++m) {
        const auto &member = reflect.get_type(type.member_types[m]);
        std::string name = prefix + reflect.get_member_name(type.self, m);
        int offset = base + static_cast<int>(reflect.type_struct_member_offset(type, m));
        int count = member.array.empty() ? 1 : static_cast<int>(member.array[0]);
        int stride = member.array.empty() ? 0 : static_cast<int>(reflect.type_struct_member_array_stride(type, m));
        if (member.basetype == spirv_cross::SPIRType::Struct) {
            for (int e = 0; e < count; ++e)
                reflect_members(reflect, member, name + (member.array.empty() ? "" : "[" + std::to_string(e) + "]") + ".",
                                offset + e * stride, block, out);
            continue;
        }
        BlockMember b = {};
        b.block = block;
        b.info.type = gl_type_of(member);
        b.info.array_size = count;
        b.info.is_array = !member.array.empty();
        b.info.offset = offset;
        b.info.array_stride = stride;
        b.info.matrix_stride = member.columns > 1 ? static_cast<int>(reflect.type_struct_member_matrix_stride(type, m)) : 0;
        b.info.row_major = reflect.has_member_decoration(type.self, m, spv::DecorationRowMajor);
        b.info.sampler_slot = -1;
        b.info.legacy = -1;
        out.push_back({name, b});
    }
}

int type_components(const spirv_cross::SPIRType &t) { return static_cast<int>(t.vecsize * t.columns); }

std::string reflect_feedback(const std::vector<uint32_t> &vertex, const Bindings &b, glm_compile_result *result)
{
    spirv_cross::Compiler reflect(vertex);
    auto resources = reflect.get_shader_resources();
    std::map<std::string, std::pair<const spirv_cross::SPIRType *, int>> outputs; // type, array size
    for (auto &output : resources.stage_outputs) {
        const auto &type = reflect.get_type(output.type_id);
        outputs[reflect.get_name(output.id)] = {&type, type.array.empty() ? 1 : static_cast<int>(type.array[0])};
    }
    for(auto &output:resources.builtin_outputs)if(output.builtin==spv::BuiltInClipDistance){
        const auto &type=reflect.get_type(output.value_type_id);
        outputs["gl_ClipDistance"]={&type,type.array.empty()?1:static_cast<int>(type.array[0])};
    }
    // Capture names must refer to outputs of the final vertex-processing
    // stage. Injecting a capture expression alone also accepts inputs and
    // uniforms, and therefore cannot perform this link validation.
    std::map<std::string, std::vector<int>> captured;
    int buffer = 0;
    if (!b.interleaved && b.feedback.size() > 4) return "too many separate transform feedback varyings";
    for (const auto &name : b.feedback) {
        if (name == "gl_NextBuffer") {
            if (!b.interleaved || ++buffer >= 4) return "invalid transform feedback buffer separator";
            continue;
        }
        if (name.rfind("gl_SkipComponents", 0) == 0) {
            if (!b.interleaved || name.size() != 18 || name.back() < '1' || name.back() > '4')
                return "invalid transform feedback component skip";
            continue;
        }
        std::string expression = xfb_expression(name);
        size_t bracket = expression.find('[');
        std::string base = expression.substr(0, bracket);
        auto found = outputs.find(base);
        bool builtin = name == "gl_Position" || name == "gl_PointSize";
        if (!builtin && found == outputs.end()) return "transform feedback output is not available: " + name;
        int first = 0, count = builtin ? 1 : found->second.second;
        if (bracket != std::string::npos) {
            if (found->second.first->array.empty() || expression.back() != ']' || bracket + 2 >= expression.size())
                return "invalid transform feedback array element: " + name;
            uint64_t index = 0;
            for (size_t n = bracket + 1; n + 1 < expression.size(); ++n) {
                unsigned digit = static_cast<unsigned>(expression[n] - '0');
                if (digit > 9 || index > (UINT64_MAX - digit) / 10)
                    return "invalid transform feedback array element: " + name;
                index = index * 10 + digit;
            }
            if (index >= static_cast<uint64_t>(count)) return "transform feedback array element is out of range: " + name;
            first = static_cast<int>(index);
            count = 1;
        }
        // A whole array and an indexed element are distinct requests in
        // desktop GL. Reject duplicate names and duplicate array indices.
        int selector = bracket == std::string::npos ? -1 : first;
        for (int previous : captured[base])
            if (selector == previous)
                return "transform feedback output is captured more than once: " + name;
        captured[base].push_back(selector);
    }
    result->xfb_count = static_cast<int>(b.feedback.size());
    result->xfb = static_cast<glm_xfb_varying *>(calloc(b.feedback.size() + 1, sizeof(glm_xfb_varying)));
    result->xfb_buffers = b.interleaved ? 1 : std::min(4, static_cast<int>(b.feedback.size()));
    int offset = 0, interleaved_buffer = 0;
    bool double_buffers[4] = {};
    for (size_t v = 0; v < b.feedback.size(); ++v) {
        glm_xfb_varying &x = result->xfb[v];
        const std::string &name = b.feedback[v];
        x.name = copy(name);
        x.buffer = b.interleaved ? interleaved_buffer : static_cast<int>(v);
        if (!b.interleaved) offset = 0;
        x.offset = offset;
        x.size = 1;
        if (b.interleaved && name == "gl_NextBuffer") {
            ++interleaved_buffer;
            result->xfb_buffers = std::min(4, interleaved_buffer + 1);
            offset = 0;
            continue;
        }
        if (name.rfind("gl_SkipComponents", 0) == 0) {
            x.components = std::atoi(name.c_str() + 17);
        } else if (name == "gl_Position") {
            x.components = 4;
            x.type = 0x8B52; // GL_FLOAT_VEC4
        } else if (name == "gl_PointSize") {
            x.components = 1;
            x.type = 0x1406;
        } else if (name != "gl_NextBuffer") {
            std::string expr = xfb_expression(name);
            size_t bracket = expr.find('[');
            std::string base = expr.substr(0, bracket);
            auto found = outputs.find(base);
            if (found != outputs.end()) {
                int per = type_components(*found->second.first);
                x.type = gl_type_of(*found->second.first);
                int count = bracket == std::string::npos ? found->second.second : 1;
                x.size = count;
                x.components = per * count;
            }
        }
        bool is_double = x.type == 0x140A || (x.type >= 0x8FFC && x.type <= 0x8FFE) ||
                         (x.type >= 0x8F46 && x.type <= 0x8F4E);
        if (is_double) {
            offset = (offset + 1) & ~1;
            x.offset = offset;
            if (x.buffer < 4) double_buffers[x.buffer] = true;
        }
        offset += x.components * (is_double ? 2 : 1);
        if (b.interleaved && interleaved_buffer < 4) result->xfb_stride[interleaved_buffer] = offset;
        else if (v < 4) result->xfb_stride[v] = offset;
    }
    for (int k = 0; k < 4; ++k)
        if (double_buffers[k]) result->xfb_stride[k] = (result->xfb_stride[k] + 1) & ~1;
    // Internal VS capture for geometry, tessellation and clipping uses these
    // buffers as stage storage; the public program was validated separately.
    for (int k = 0; !b.capture && k < result->xfb_buffers; ++k)
        if (result->xfb_stride[k] > (b.interleaved ? 64 : 4))
            return "transform feedback components exceed the buffer limit";
    return {};
}

} // namespace

static void compile_program(const glm_compile_request *req, glm_compile_result *result, bool capture,
                            bool allow_fp64 = true, bool *using_fp64 = nullptr);

// Generated stages share user uniform storage with the linked program.
// A generated stage must never silently lower an exact double block to float.
static bool uniform_storage_compatible(const glm_compile_result &parent, const glm_compile_result &child)
{
    for (int block = 0; block < 2; ++block) {
        const auto *a = block ? parent.block_uniforms : parent.uniforms;
        const auto *b = block ? child.block_uniforms : child.uniforms;
        int na = block ? parent.block_uniform_count : parent.uniform_count;
        int nb = block ? child.block_uniform_count : child.uniform_count;
        for (int j = 0; j < nb; ++j) for (int i = 0; i < na; ++i) {
            if (strcmp(a[i].name, b[j].name)) continue;
            if (block && strcmp(parent.blocks[a[i].block].name, child.blocks[b[j].block].name)) continue;
            if (a[i].type != b[j].type || a[i].array_stride != b[j].array_stride ||
                a[i].matrix_stride != b[j].matrix_stride || a[i].row_major != b[j].row_major ||
                (block && a[i].offset != b[j].offset)) return false;
        }
    }
    return true;
}

// The attributes of a post-tessellation vertex function's stage_in structs,
// one "patch type name attribute" line each (patch: 1 for main0_patchIn).
static std::string stage_in_members(const std::string &msl)
{
    std::string lines;
    const char *structs[2] = {"struct main0_in\n{\n", "struct main0_patchIn\n{\n"};
    for (int patch = 0; patch < 2; ++patch) {
        size_t start = msl.find(structs[patch]);
        if (start == std::string::npos) continue;
        size_t end = msl.find("};", start);
        std::string body = msl.substr(start, end - start);
        std::regex member(R"((\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\])");
        for (std::sregex_iterator it(body.begin(), body.end(), member), stop; it != stop; ++it)
            lines += std::to_string(patch) + " " + (*it)[1].str() + " " + (*it)[2].str() + " " + (*it)[3].str() + "\n";
    }
    return lines;
}

namespace {

std::string strip_comments(const std::string &in)
{
    std::string out;
    for (size_t i = 0; i < in.size();) {
        if (in.compare(i, 2, "//") == 0) {
            while (i < in.size() && in[i] != '\n') ++i;
        } else if (in.compare(i, 2, "/*") == 0) {
            size_t end = in.find("*/", i + 2);
            end = end == std::string::npos ? in.size() : end + 2;
            for (; i < end; ++i)
                if (in[i] == '\n') out += '\n';
        } else {
            out += in[i++];
        }
    }
    return out;
}

int type_floats(const std::string &t)
{
    std::smatch m;
    if (t == "float" || t == "int" || t == "uint" || t == "bool") return 1;
    if (std::regex_match(t, m, std::regex("[biu]?vec([234])"))) return std::stoi(m[1]);
    if (std::regex_match(t, m, std::regex("mat([234])"))) return std::stoi(m[1]) * std::stoi(m[1]);
    if (std::regex_match(t, m, std::regex("mat([234])x([234])"))) return std::stoi(m[1]) * std::stoi(m[2]);
    return 4;
}

// A constructor of `type` from floats arr[base + offset + c].
std::string read_value(const std::string &arr, const std::string &base, const std::string &type, int offset)
{
    auto at = [&](int c) { return arr + "[" + base + " + " + std::to_string(offset + c) + "u]"; };
    std::smatch m;
    if (type == "float") return at(0);
    if (type == "int") return "floatBitsToInt(" + at(0) + ")";
    if (type == "uint") return "floatBitsToUint(" + at(0) + ")";
    if (type == "bool") return "(" + at(0) + " != 0.0)";
    if (std::regex_match(type, m, std::regex("([iu]?)vec([234])"))) {
        std::string fn = m[1] == "i" ? "floatBitsToInt" : m[1] == "u" ? "floatBitsToUint" : "";
        std::string out = type + "(";
        for (int c = 0; c < std::stoi(m[2]); ++c) out += (c ? ", " : "") + (fn.empty() ? at(c) : fn + "(" + at(c) + ")");
        return out + ")";
    }
    if (std::regex_match(type, m, std::regex("mat([234])(x([234]))?"))) {
        int cols = std::stoi(m[1]), rows = m[3].matched ? std::stoi(m[3]) : cols;
        std::string out = type + "(";
        for (int c = 0; c < cols * rows; ++c) out += (c ? ", " : "") + at(c);
        return out + ")";
    }
    return "vec4(" + at(0) + ", " + at(1) + ", " + at(2) + ", " + at(3) + ")";
}

// glm_put<name>(inout uint i, T v) overloads writing floats into `array`.
std::string put_functions(const std::string &fn, const std::string &array)
{
    std::ostringstream out;
    const char *types[] = {"float", "vec2", "vec3", "vec4"};
    for (int n = 0; n < 4; ++n) {
        out << "void " << fn << "(inout uint i, " << types[n] << " v) {";
        for (int c = 0; c <= n; ++c) out << " " << array << "[i++] = v" << (n ? std::string(".") + "xyzw"[c] : "") << ";";
        out << " }\n";
    }
    const char *ints[] = {"int", "ivec2", "ivec3", "ivec4", "uint", "uvec2", "uvec3", "uvec4"};
    for (int n = 0; n < 8; ++n) {
        int size = n % 4 + 1;
        out << "void " << fn << "(inout uint i, " << ints[n] << " v) {";
        for (int c = 0; c < size; ++c)
            out << " " << array << "[i++] = " << (n >= 4 ? "uintBitsToFloat" : "intBitsToFloat") << "(v"
                << (size > 1 ? std::string(".") + "xyzw"[c] : "") << ");";
        out << " }\n";
    }
    for (int cols = 2; cols <= 4; ++cols)
        for (int rows = 2; rows <= 4; ++rows) {
            std::string type = cols == rows ? "mat" + std::to_string(cols) : "mat" + std::to_string(cols) + "x" + std::to_string(rows);
            out << "void " << fn << "(inout uint i, " << type << " v) {";
            for (int c = 0; c < cols; ++c) out << " " << fn << "(i, v[" << c << "]);";
            out << " }\n";
        }
    return out.str();
}

// Removes each declaration `re` matches (group 1 is the separator before
// it, which stays), calling `found` with the match; one at a time, so
// declarations sharing a separator are all seen.
template <typename F> void take_declarations(std::string &text, const std::regex &re, F found)
{
    std::smatch m;
    std::string::const_iterator from = text.begin();
    while (std::regex_search(from, text.cend(), m, re)) {
        found(m);
        size_t at = static_cast<size_t>(m.position(0) + (from - text.cbegin()));
        size_t keep = static_cast<size_t>(m.length(1));
        text.erase(at + keep, static_cast<size_t>(m.length(0)) - keep);
        from = text.begin() + static_cast<std::ptrdiff_t>(at);
    }
}

struct GeometryVar {
    std::string qualifiers, type, name;
    int array = 0;
    int stream = 0;
};

struct Geometry {
    int in_vertices = 3, max_vertices = 0, invocations = 1, out_primitive = 3; // MTLPrimitiveType
    std::vector<GeometryVar> inputs, outputs;
    bool reads_point_size = false, flat_outputs = false;
    bool writes_layer = false, writes_viewport = false; // gl_Layer / gl_ViewportIndex
    std::string body, error;
};

// The source with macros expanded and conditionals resolved (glslang's
// preprocessor), for the stages parsed here rather than by glslang; the
// source itself if it does not preprocess.
std::string preprocessed(const std::string &source, EShLanguage lang)
{
    if (source.find('#') == std::string::npos) return source;
    std::string text = normalize_newlines(source), profile;
    int version = take_version(text, &profile);
    // glslang's GLSL frontend takes stages it otherwise rejects at 150 only
    // from 150 core; the version line goes back unchanged.
    std::string head = "#version " + std::to_string(std::max(version, 150)) + (profile.empty() ? "" : " " + profile) + "\n";
    std::string full = head + text;
    const char *strings = full.c_str();
    glslang::TShader shader(lang);
    shader.setStrings(&strings, 1);
    configure(shader, lang);
    glslang::TShader::ForbidIncluder includer;
    std::string out;
    if (!shader.preprocess(shader_resources(source.c_str()), 450, ENoProfile, false, false, messages, &out, includer)) return source;
    // Keep the original version line (preprocessed output repeats one).
    out = std::regex_replace(out, std::regex(R"((^|\n)\s*#\s*(version|line)[^\n]*)"), "$1");
    return "#version " + std::to_string(version) + (profile.empty() ? "" : " " + profile) + "\n" + out;
}

Geometry parse_geometry(const std::string &raw)
{
    Geometry g;
    std::string source = preprocessed(raw, EShLangGeometry);
    std::string text = strip_comments(normalize_newlines(source));
    std::string profile;
    take_version(text, &profile);
    std::smatch m;
    std::regex layout_in(R"(layout\s*\(([^)]*)\)\s*in\s*;)"), layout_out(R"(layout\s*\(([^)]*)\)\s*out\s*;)");
    if (std::regex_search(text, m, layout_in)) {
        std::string q = m[1];
        if (q.find("triangles_adjacency") != std::string::npos) g.in_vertices = 6;
        else if (q.find("lines_adjacency") != std::string::npos) g.in_vertices = 4;
        else if (q.find("triangles") != std::string::npos) g.in_vertices = 3;
        else if (q.find("lines") != std::string::npos) g.in_vertices = 2;
        else if (q.find("points") != std::string::npos) g.in_vertices = 1;
        std::smatch n;
        if (std::regex_search(q, n, std::regex(R"(invocations\s*=\s*(\d+))"))) g.invocations = std::stoi(n[1]);
    }
    if (std::regex_search(text, m, layout_out)) {
        std::string q = m[1];
        g.out_primitive = q.find("triangle_strip") != std::string::npos ? 3 : q.find("line_strip") != std::string::npos ? 1 : 0;
        std::smatch n;
        if (std::regex_search(q, n, std::regex(R"(max_vertices\s*=\s*(\d+))"))) g.max_vertices = std::stoi(n[1]);
    }
    text = std::regex_replace(text, layout_in, "");
    text = std::regex_replace(text, layout_out, "");
    if (std::regex_search(text, std::regex(R"(\b(in|out)\s+\w+\s*\{)")))
        g.error = "interface blocks in geometry shaders are not supported yet";
    std::regex input(R"((^|;|\n)\s*((?:(?:flat|smooth|noperspective|centroid|layout\s*\([^)]*\))\s+)*)in\s+(\w+)\s+(\w+)\s*\[\s*\d*\s*\]\s*;)");
    take_declarations(text, input, [&](const std::smatch &d) { g.inputs.push_back({d[2], d[3], d[4], 0}); });
    std::regex output(R"((^|;|\n)\s*((?:(?:flat|smooth|noperspective|centroid|invariant|layout\s*\([^)]*\))\s+)*)out\s+(\w+)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;)");
    take_declarations(text, output, [&](const std::smatch &d) {
        GeometryVar v{d[2], d[3], d[4], d[5].matched ? std::stoi(d[5]) : 0};
        std::smatch stream;
        if (std::regex_search(v.qualifiers, stream, std::regex(R"(stream\s*=\s*(\d+))"))) v.stream = std::stoi(stream[1]);
        v.qualifiers = std::regex_replace(v.qualifiers, std::regex(R"(layout\s*\([^)]*stream[^)]*\)\s*)"), "");
        if (v.qualifiers.find("flat") != std::string::npos) g.flat_outputs = true;
        g.outputs.push_back(v);
    });
    // Remaining #extension lines are for the geometry stage; drop them.
    text = std::regex_replace(text, std::regex(R"(#\s*extension[^\n]*)"), "");
    static const std::map<std::string, std::string> renames = {
        {"main", "glm_gs_main"}, {"gl_in", "glm_gl_in"}, {"gl_Position", "glm_Position"}, {"gl_PointSize", "glm_PointSize"},
        {"EmitVertex", "glm_emit"}, {"EndPrimitive", "glm_end"},
        {"EmitStreamVertex", "glm_emit_stream"}, {"EndStreamPrimitive", "glm_end_stream"}, {"gl_PrimitiveIDIn", "int(glm_prim)"},
        {"gl_InvocationID", "int(glm_inv)"}, {"gl_Layer", "glm_Layer"}, {"gl_ViewportIndex", "glm_ViewportIndex"},
        {"gl_PrimitiveID", "glm_PrimitiveID"}, {"gl_ClipDistance", "glm_ClipDistance"}};
    std::string body;
    for (size_t i = 0; i < text.size();) {
        if (ident_start(text[i]) && (i == 0 || !ident_char(text[i - 1]))) {
            size_t start = i;
            while (i < text.size() && ident_char(text[i])) ++i;
            std::string word = text.substr(start, i - start);
            auto found = renames.find(word);
            // Members (gl_in[i].gl_Position) keep their names.
            size_t prev = body.find_last_not_of(" \t\n");
            bool member = prev != std::string::npos && body[prev] == '.';
            body += found != renames.end() && !member ? found->second : word;
            continue;
        }
        body += text[i++];
    }
    g.reads_point_size = body.find(".gl_PointSize") != std::string::npos;
    g.writes_layer = body.find("glm_Layer") != std::string::npos;
    g.writes_viewport = body.find("glm_ViewportIndex") != std::string::npos;
    g.body = body;
    if (!g.max_vertices && g.error.empty()) g.error = "geometry shader without max_vertices";
    return g;
}

int out_stride(const Geometry &g)
{
    int n = 8; // position, point size, layer, viewport index, stream
    for (auto &v : g.outputs) n += type_floats(v.type) * (v.array ? v.array : 1);
    return n;
}

int max_indices(const Geometry &g)
{
    int m = g.max_vertices;
    return g.out_primitive == 3 ? std::max(0, m - 2) * 3 : g.out_primitive == 1 ? std::max(0, m - 1) * 2 : m;
}

std::string geometry_kernel(const Geometry &g, const glm_compile_result &capture)
{
    std::ostringstream k;
    int N = g.in_vertices, M = g.max_vertices, OUT = out_stride(g), IDX = max_indices(g);
    k << "#version 430\nlayout(local_size_x = 64) in;\n"
      << "layout(std430, set = 1, binding = 24) readonly buffer GLMGsIn { float glm_vs[]; };\n"
      << "layout(std430, set = 1, binding = 25) writeonly buffer GLMGsVertices { float glm_gv[]; };\n"
      << "layout(std430, set = 1, binding = 26) writeonly buffer GLMGsIndices { uint glm_gi[]; };\n"
      << "layout(std140, set = 1, binding = 27) uniform GLMGsInfo { uint glm_prims; uint glm_vs_stride; };\n"
      << "struct glm_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[8]; };\n"
      << "glm_PerVertex glm_gl_in[" << N << "];\n";
    for (auto &v : g.inputs) k << v.type << " " << v.name << "[" << N << "];\n";
    for (auto &v : g.outputs)
        k << v.type << " " << v.name << (v.array ? "[" + std::to_string(v.array) + "]" : "") << ";\n";
    k << "vec4 glm_Position; float glm_PointSize = 1.0; int glm_Layer = 0; int glm_ViewportIndex = 0; int glm_PrimitiveID;\n"
      << "float glm_ClipDistance[8];\n"
      << "uint glm_id, glm_prim, glm_inv, glm_emitted, glm_strip, glm_indices;\n"
      << put_functions("glm_put", "glm_gv");
    k << "void glm_emit_stream(int stream) {\n  if (glm_emitted >= " << M << "u) return;\n"
      << "  uint slot = glm_id * " << M << "u + glm_emitted + 1u;\n  uint i = slot * " << OUT << "u;\n"
      << "  glm_put(i, glm_Position); glm_put(i, glm_PointSize); glm_put(i, float(glm_Layer)); glm_put(i, float(glm_ViewportIndex));\n";
    for (auto &v : g.outputs) {
        if (v.array) k << "  for (int a = 0; a < " << v.array << "; ++a) glm_put(i, " << v.name << "[a]);\n";
        else k << "  glm_put(i, " << v.name << ");\n";
    }
    k << "  glm_gv[slot * " << OUT << "u + " << OUT - 1 << "u] = float(stream);\n"
      << "  if (stream != 0) { ++glm_emitted; return; }\n";
    k << "  uint at = glm_id * " << IDX << "u + glm_indices;\n";
    if (g.out_primitive == 3) {
        // Strip triangle t = strip - 2: (s-2, s-1, s), odd ones (s-1, s-2, s);
        // with flat outputs the provoking vertex (s) goes first for Metal.
        if (g.flat_outputs)
            k << "  if (glm_strip >= 2u) { bool odd = (glm_strip & 1u) == 1u;\n"
                 "    glm_gi[at] = slot; glm_gi[at + 1u] = odd ? slot - 1u : slot - 2u; glm_gi[at + 2u] = odd ? slot - 2u : slot - 1u;\n"
                 "    glm_indices += 3u; }\n";
        else
            k << "  if (glm_strip >= 2u) { bool odd = (glm_strip & 1u) == 1u;\n"
                 "    glm_gi[at] = odd ? slot - 1u : slot - 2u; glm_gi[at + 1u] = odd ? slot - 2u : slot - 1u; glm_gi[at + 2u] = slot;\n"
                 "    glm_indices += 3u; }\n";
    } else if (g.out_primitive == 1) {
        k << "  if (glm_strip >= 1u) { glm_gi[at] = slot - 1u; glm_gi[at + 1u] = slot; glm_indices += 2u; }\n";
    } else {
        k << "  glm_gi[at] = slot; glm_indices += 1u;\n";
    }
    k << "  ++glm_strip; ++glm_emitted;\n}\n"
      << "void glm_emit() { glm_emit_stream(0); }\n"
      << "void glm_end_stream(int stream) { if (stream == 0) glm_strip = 0u; }\n"
      << "void glm_end() { glm_end_stream(0); }\n"
      << "#line 1\n" << g.body << "\n";
    // Where the captured vertex stage put each input (floats).
    std::map<std::string, int> offsets;
    for (int i = 0; i < capture.xfb_count; ++i) offsets[capture.xfb[i].name] = capture.xfb[i].offset;
    k << "void main() {\n  uint id = gl_GlobalInvocationID.x;\n"
      << "  if (id == 0u) { glm_gv[0] = 2.0; glm_gv[1] = 2.0; glm_gv[2] = 2.0; glm_gv[3] = 1.0; }\n"
      << "  if (id >= glm_prims * " << g.invocations << "u) return;\n"
      << "  glm_id = id; glm_prim = id / " << g.invocations << "u; glm_inv = id % " << g.invocations << "u;\n"
      << "  glm_PrimitiveID = int(glm_prim);\n"
      << "  for (uint v = 0u; v < " << N << "u; ++v) {\n    uint base = (glm_prim * " << N << "u + v) * glm_vs_stride;\n"
      << "    glm_gl_in[v].gl_Position = " << read_value("glm_vs", "base", "vec4", offsets["gl_Position"]) << ";\n";
    if (offsets.count("gl_PointSize"))
        k << "    glm_gl_in[v].gl_PointSize = " << read_value("glm_vs", "base", "float", offsets["gl_PointSize"]) << ";\n";
    for (auto &v : g.inputs)
        k << "    " << v.name << "[v] = " << read_value("glm_vs", "base", v.type, offsets[v.name]) << ";\n";
    k << "  }\n  for (uint v = 0u; v < " << M << "u; ++v) glm_gv[(id * " << M << "u + v + 1u) * " << OUT << "u + " << OUT - 1 << "u] = -1.0;\n"
      << "  glm_emitted = 0u; glm_strip = 0u; glm_indices = 0u;\n  glm_gs_main();\n"
      << "  for (uint i = glm_indices; i < " << IDX << "u; ++i) glm_gi[id * " << IDX << "u + i] = 0u;\n}\n";
    return k.str();
}

std::string geometry_pull(const Geometry &g, int version)
{
    std::ostringstream v;
    v << "#version " << std::max(version, 430) << "\n#define GLM_RAW_VERTEX_ID\n";
    if (g.writes_layer || g.writes_viewport) v << "#extension GL_ARB_shader_viewport_layer_array : require\n";
    if (g.out_primitive != 0) v << "#define GLM_NO_POINT_SIZE\n";
    v << "layout(std430, set = 1, binding = 25) readonly buffer GLMGsVertices { float glm_gv[]; };\n";
    for (auto &o : g.outputs)
        v << o.qualifiers << "out " << o.type << " " << o.name << (o.array ? "[" + std::to_string(o.array) + "]" : "") << ";\n";
    v << "void main() {\n  uint i = uint(gl_VertexID) * " << out_stride(g) << "u;\n"
      << "  gl_Position = " << read_value("glm_gv", "i", "vec4", 0) << ";\n"
      ;
    // Metal pipelines with an input topology (layered output) reject a
    // point size for lines and triangles.
    if (g.out_primitive == 0) v << "  gl_PointSize = " << read_value("glm_gv", "i", "float", 4) << ";\n";
    if (g.writes_layer) v << "  gl_Layer = int(" << read_value("glm_gv", "i", "float", 5) << ");\n";
    if (g.writes_viewport) v << "  gl_ViewportIndex = int(" << read_value("glm_gv", "i", "float", 6) << ");\n";
    int offset = 7;
    for (auto &o : g.outputs) {
        int n = type_floats(o.type);
        if (o.array)
            for (int a = 0; a < o.array; ++a, offset += n) v << "  " << o.name << "[" << a << "] = " << read_value("glm_gv", "i", o.type, offset) << ";\n";
        else {
            v << "  " << o.name << " = " << read_value("glm_gv", "i", o.type, offset) << ";\n";
            offset += n;
        }
    }
    v << "}\n";
    return v.str();
}

std::string geometry_emulation(const glm_compile_request *req, glm_compile_result *result, bool allow_fp64)
{
    Geometry g = parse_geometry(req->sources[GLM_STAGE_GEOMETRY]);
    if (!g.error.empty()) return g.error;
    // 1. The vertex stage, capturing what the geometry shader reads.
    std::vector<std::string> names = {"gl_Position"};
    if (g.reads_point_size) names.push_back("gl_PointSize");
    for (auto &v : g.inputs) names.push_back(v.name);
    std::vector<const char *> pointers;
    for (auto &n : names) pointers.push_back(n.c_str());
    glm_compile_request capture_request = *req;
    capture_request.feedback_varyings = pointers.data();
    capture_request.feedback_count = static_cast<int>(pointers.size());
    capture_request.feedback_interleaved = true;
    glm_compile_result capture;
    compile_program(&capture_request, &capture, true, allow_fp64);
    if (!capture.ok) {
        std::string log = std::string("geometry emulation, vertex stage: ") + (capture.log ? capture.log : "");
        glm_compile_result_free(&capture);
        return log;
    }
    if (allow_fp64 && !uniform_storage_compatible(*result, capture)) {
        glm_compile_result_free(&capture);
        return "geometry capture changed exact uniform storage";
    }
    auto *gs = static_cast<glm_gs_result *>(calloc(1, sizeof(glm_gs_result)));
    gs->vs_capture = capture.msl[GLM_STAGE_VERTEX];
    capture.msl[GLM_STAGE_VERTEX] = nullptr;
    gs->vs_stride = capture.xfb_stride[0];
    gs->in_vertices = g.in_vertices;
    gs->invocations = g.invocations;
    gs->max_vertices = g.max_vertices;
    gs->out_stride = out_stride(g);
    gs->max_indices = max_indices(g);
    gs->out_primitive = g.out_primitive;
    gs->writes_viewport = g.writes_viewport;
    gs->flat_outputs = g.flat_outputs;
    for (int f = 0; f < result->xfb_count && f < 64; ++f) {
        const std::string name = result->xfb[f].name;
        int offset = 7;
        gs->feedback_offsets[f] = name == "gl_Position" ? 0 : name == "gl_PointSize" ? 4 : -1;
        for (auto &v : g.outputs) {
            if (name == v.name || name == v.name + "[0]") {
                gs->feedback_offsets[f] = offset;
                gs->feedback_streams[f] = v.stream;
            }
            offset += type_floats(v.type) * (v.array ? v.array : 1);
        }
    }

    // 2. The geometry shader as a compute kernel.
    std::string kernel = geometry_kernel(g, capture);
    glm_compile_result_free(&capture);
    glm_compile_request kernel_request = {};
    kernel_request.sources[GLM_STAGE_COMPUTE] = kernel.c_str();
    gs->kernel = static_cast<glm_compile_result *>(calloc(1, sizeof(glm_compile_result)));
    compile_program(&kernel_request, gs->kernel, false, allow_fp64);
    // 3. Drawing its output with the program's fragment stage.
    std::string profile, text = req->sources[GLM_STAGE_GEOMETRY];
    int version = take_version(text, &profile);
    std::string pull = geometry_pull(g, version);
    glm_compile_request pull_request = *req;
    for (auto &source : pull_request.sources) source = nullptr;
    pull_request.sources[GLM_STAGE_VERTEX] = pull.c_str();
    pull_request.sources[GLM_STAGE_FRAGMENT] = req->sources[GLM_STAGE_FRAGMENT];
    pull_request.feedback_count = 0;
    pull_request.attribute_count = 0;
    gs->pull = static_cast<glm_compile_result *>(calloc(1, sizeof(glm_compile_result)));
    compile_program(&pull_request, gs->pull, false, allow_fp64);
    result->gs = gs;
    std::string log;
    if (!gs->kernel->ok) log += std::string("geometry kernel: ") + (gs->kernel->log ? gs->kernel->log : "") + "\n" + kernel;
    if (!gs->pull->ok) log += std::string("geometry output stage: ") + (gs->pull->log ? gs->pull->log : "") + "\n" + pull;
    if (allow_fp64 && (!uniform_storage_compatible(*result, *gs->kernel) || !uniform_storage_compatible(*result, *gs->pull)))
        log += "geometry emulation changed exact uniform storage\n";
    return log;
}

struct TessControl {
    int out_vertices = 0;
    std::vector<GeometryVar> inputs, outputs, patch_outputs;
    bool reads_point_size = false;
    std::string body, error;
};

// Renames a stage's built-ins (not members such as gl_out[i].gl_Position).
std::string rename_words(const std::string &text, const std::map<std::string, std::string> &renames)
{
    std::string body;
    for (size_t i = 0; i < text.size();) {
        if (ident_start(text[i]) && (i == 0 || !ident_char(text[i - 1]))) {
            size_t start = i;
            while (i < text.size() && ident_char(text[i])) ++i;
            std::string word = text.substr(start, i - start);
            auto found = renames.find(word);
            size_t prev = body.find_last_not_of(" \t\n");
            bool member = prev != std::string::npos && body[prev] == '.';
            body += found != renames.end() && !member ? found->second : word;
            continue;
        }
        body += text[i++];
    }
    return body;
}

TessControl parse_tess_control(const std::string &raw)
{
    TessControl t;
    std::string source = preprocessed(raw, EShLangTessControl);
    std::string text = strip_comments(normalize_newlines(source));
    std::string profile;
    take_version(text, &profile);
    std::smatch m;
    std::regex layout_out(R"(layout\s*\(\s*vertices\s*=\s*(\d+)\s*\)\s*out\s*;)");
    if (std::regex_search(text, m, layout_out)) t.out_vertices = std::stoi(m[1]);
    text = std::regex_replace(text, layout_out, "");
    if (std::regex_search(text, std::regex(R"(\b(in|out)\s+\w+\s*\{)")))
        t.error = "interface blocks in tessellation control shaders are not supported yet";
    const std::string qualifiers = R"(((?:(?:flat|smooth|noperspective|centroid|invariant|precise|layout\s*\([^)]*\))\s+)*))";
    std::regex input("(^|;|\\n)\\s*" + qualifiers + R"(in\s+(\w+)\s+(\w+)\s*\[[^\]]*\]\s*;)");
    take_declarations(text, input, [&](const std::smatch &d) { t.inputs.push_back({d[2], d[3], d[4], 0}); });
    std::regex patch_output("(^|;|\\n)\\s*" + qualifiers + R"(patch\s+out\s+(\w+)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;)");
    take_declarations(text, patch_output, [&](const std::smatch &d) {
        t.patch_outputs.push_back({d[2], d[3], d[4], d[5].matched ? std::stoi(d[5]) : 0});
    });
    std::regex output("(^|;|\\n)\\s*" + qualifiers + R"(out\s+(\w+)\s+(\w+)\s*\[[^\]]*\]\s*;)");
    take_declarations(text, output, [&](const std::smatch &d) { t.outputs.push_back({d[2], d[3], d[4], 0}); });
    text = std::regex_replace(text, std::regex(R"(#\s*extension[^\n]*)"), "");
    static const std::map<std::string, std::string> renames = {
        {"main", "glm_tcs_main"}, {"gl_in", "glm_gl_in"}, {"gl_out", "glm_gl_out"},
        {"gl_InvocationID", "int(glm_inv)"}, {"gl_PatchVerticesIn", "int(glm_pvin)"}, {"gl_PrimitiveID", "int(glm_patch)"},
        {"gl_TessLevelOuter", "glm_TessLevelOuter"}, {"gl_TessLevelInner", "glm_TessLevelInner"},
        {"gl_MaxPatchVertices", "32"}};
    t.body = rename_words(text, renames);
    t.reads_point_size = t.body.find("glm_gl_in") != std::string::npos && t.body.find(".gl_PointSize") != std::string::npos;
    if (!t.out_vertices && t.error.empty()) t.error = "tessellation control shader without layout(vertices = N) out";
    if (t.out_vertices > 32 && t.error.empty()) t.error = "more than 32 output patch vertices";
    return t;
}

struct TessInput {
    bool patch;
    std::string type, name;
    int attribute;
};

std::string tess_kernel(const TessControl &t, const glm_compile_result &capture, const std::vector<TessInput> &inputs,
                        int cp_slots, int patch_slots, const std::vector<int> &slots, bool quads)
{
    std::ostringstream k;
    int N = t.out_vertices;
    k << "#version 430\nlayout(local_size_x = " << N << ") in;\n"
      << "layout(std430, set = 1, binding = 24) readonly buffer GLMGsIn { float glm_vs[]; };\n"
      << "layout(std430, set = 1, binding = 25) writeonly buffer GLMTcsOut { float glm_cp[]; };\n"
      << "layout(std430, set = 1, binding = 26) writeonly buffer GLMTcsPatch { float glm_pp[]; };\n"
      << "layout(std430, set = 1, binding = 28) writeonly buffer GLMTcsFactors { uint glm_tf[]; };\n"
      << "layout(std140, set = 1, binding = 27) uniform GLMGsInfo { uint glm_prims; uint glm_vs_stride; uint glm_pvin; };\n"
      << "struct glm_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[8]; };\n"
      << "glm_PerVertex glm_gl_in[32];\n"
      << "shared glm_PerVertex glm_gl_out[" << N << "];\n"
      << "shared float glm_TessLevelOuter[4];\nshared float glm_TessLevelInner[2];\n"
      << "uint glm_inv, glm_patch;\n";
    for (auto &v : t.inputs) k << v.type << " " << v.name << "[32];\n";
    for (auto &v : t.outputs) k << "shared " << v.type << " " << v.name << "[" << N << "];\n";
    for (auto &v : t.patch_outputs)
        k << "shared " << v.type << " " << v.name << (v.array ? "[" + std::to_string(v.array) + "]" : "") << ";\n";
    k << put_functions("glm_put", "glm_cp") << put_functions("glm_patch_put", "glm_pp")
      << "#line 1\n" << t.body << "\n";
    std::map<std::string, int> offsets;
    for (int i = 0; i < capture.xfb_count; ++i) offsets[capture.xfb[i].name] = capture.xfb[i].offset;
    k << "void main() {\n  glm_patch = gl_WorkGroupID.x; glm_inv = gl_LocalInvocationID.x;\n"
      << "  if (glm_inv == 0u) { for (int l = 0; l < 4; ++l) glm_TessLevelOuter[l] = 0.0;"
         " glm_TessLevelInner[0] = 0.0; glm_TessLevelInner[1] = 0.0; }\n"
      << "  for (uint v = 0u; v < glm_pvin; ++v) {\n    uint base = (glm_patch * glm_pvin + v) * glm_vs_stride;\n"
      << "    glm_gl_in[v].gl_Position = " << read_value("glm_vs", "base", "vec4", offsets["gl_Position"]) << ";\n";
    if (offsets.count("gl_PointSize"))
        k << "    glm_gl_in[v].gl_PointSize = " << read_value("glm_vs", "base", "float", offsets["gl_PointSize"]) << ";\n";
    for (auto &v : t.inputs)
        if (offsets.count(v.name)) k << "    " << v.name << "[v] = " << read_value("glm_vs", "base", v.type, offsets[v.name]) << ";\n";
    k << "  }\n  barrier();\n  glm_tcs_main();\n  barrier();\n  uint i;\n";
    std::set<std::string> outputs, patch_outputs;
    for (auto &v : t.outputs) outputs.insert(v.name);
    for (auto &v : t.patch_outputs) patch_outputs.insert(v.name);
    std::ostringstream patch;
    for (size_t n = 0; n < inputs.size(); ++n) {
        const TessInput &in = inputs[n];
        std::string value;
        if (in.name == "gl_Position") value = "glm_gl_out[glm_inv].gl_Position";
        else if (in.name == "gl_PointSize") value = "glm_gl_out[glm_inv].gl_PointSize";
        else if (in.name == "gl_TessLevelOuter")
            value = "vec4(glm_TessLevelOuter[0], glm_TessLevelOuter[1], glm_TessLevelOuter[2], glm_TessLevelOuter[3])";
        else if (in.name == "gl_TessLevelInner") value = "vec2(glm_TessLevelInner[0], glm_TessLevelInner[1])";
        // Triangles: SPIRV-Cross packs outer 0-2 and inner 0 into one float4.
        else if (in.name == "gl_TessLevel")
            value = "vec4(glm_TessLevelOuter[0], glm_TessLevelOuter[1], glm_TessLevelOuter[2], glm_TessLevelInner[0])";
        else if (!in.patch && outputs.count(in.name)) value = in.name + "[glm_inv]";
        else if (in.patch && patch_outputs.count(in.name)) value = in.name;
        if (value.empty()) continue;
        if (in.patch)
            patch << "    i = glm_patch * " << patch_slots * 4 << "u + " << slots[n] * 4 << "u; glm_patch_put(i, " << value << ");\n";
        else
            k << "  i = (glm_patch * " << N << "u + glm_inv) * " << cp_slots * 4 << "u + " << slots[n] * 4
              << "u; glm_put(i, " << value << ");\n";
    }
    k << "  if (glm_inv == 0u) {\n" << patch.str()
      << "    float o0 = max(glm_TessLevelOuter[0], 0.0), o1 = max(glm_TessLevelOuter[1], 0.0);\n"
      << "    float o2 = max(glm_TessLevelOuter[2], 0.0), o3 = max(glm_TessLevelOuter[3], 0.0);\n"
      << "    float i0 = max(glm_TessLevelInner[0], 0.0), i1 = max(glm_TessLevelInner[1], 0.0);\n";
    // Apple feeds GL's levels and coordinates to Metal's tessellator as
    // they are (no domain flip): so does this.
    if (quads)
        k << "    glm_tf[glm_patch * 3u] = packHalf2x16(vec2(o0, o1));\n"
          << "    glm_tf[glm_patch * 3u + 1u] = packHalf2x16(vec2(o2, o3));\n"
          << "    glm_tf[glm_patch * 3u + 2u] = packHalf2x16(vec2(i0, i1));\n";
    else
        k << "    glm_tf[glm_patch * 2u] = packHalf2x16(vec2(o0, o1));\n"
          << "    glm_tf[glm_patch * 2u + 1u] = packHalf2x16(vec2(o2, i0));\n";
    k << "  }\n}\n";
    return k.str();
}

std::string tess_emulation(const glm_compile_request *req, glm_compile_result *result, bool allow_fp64)
{
    if (!req->sources[GLM_STAGE_TESS_CONTROL]) return "tessellation without a control shader is not supported yet";
    if (!req->sources[GLM_STAGE_TESS_EVALUATION]) return "a tessellation control shader needs an evaluation shader";
    if (req->sources[GLM_STAGE_GEOMETRY]) return "tessellation with a geometry shader is not supported yet";
    TessControl t = parse_tess_control(req->sources[GLM_STAGE_TESS_CONTROL]);
    if (!t.error.empty()) return t.error;
    auto *tess = static_cast<glm_tess_result *>(calloc(1, sizeof(glm_tess_result)));
    result->tess = tess;
    tess->out_vertices = t.out_vertices;
    // The evaluation stage's mode.
    std::string text = strip_comments(normalize_newlines(req->sources[GLM_STAGE_TESS_EVALUATION]));
    std::string mode;
    std::regex layout_in(R"(layout\s*\(([^)]*)\)\s*in\s*;)");
    for (std::sregex_iterator it(text.begin(), text.end(), layout_in), end; it != end; ++it) mode += (*it)[1].str() + ",";
    if (mode.find("isolines") != std::string::npos) return "isoline tessellation is not supported yet";
    tess->quads = mode.find("quads") != std::string::npos;
    tess->partition = mode.find("fractional_odd_spacing") != std::string::npos    ? 2
                      : mode.find("fractional_even_spacing") != std::string::npos ? 3
                                                                                  : 1;
    tess->cw = std::regex_search(mode, std::regex(R"(\bcw\b)"));
    tess->point_mode = mode.find("point_mode") != std::string::npos;
    // 1. The evaluation stage as a post-tessellation vertex function.
    glm_compile_request eval_request = *req;
    for (auto &source : eval_request.sources) source = nullptr;
    eval_request.sources[GLM_STAGE_TESS_EVALUATION] = req->sources[GLM_STAGE_TESS_EVALUATION];
    eval_request.sources[GLM_STAGE_FRAGMENT] = req->sources[GLM_STAGE_FRAGMENT];
    eval_request.feedback_count = 0;
    eval_request.attribute_count = 0;
    eval_request.tess_output_vertices = t.out_vertices;
    tess->eval = static_cast<glm_compile_result *>(calloc(1, sizeof(glm_compile_result)));
    compile_program(&eval_request, tess->eval, false, allow_fp64);
    if (!tess->eval->ok) return std::string("tessellation evaluation stage: ") + (tess->eval->log ? tess->eval->log : "");
    if (allow_fp64 && !uniform_storage_compatible(*result, *tess->eval)) return "tessellation evaluation changed exact uniform storage";
    // Where each of its inputs lives.
    std::vector<TessInput> inputs;
    std::vector<int> slots;
    std::istringstream lines(tess->eval->tess_inputs ? tess->eval->tess_inputs : "");
    TessInput in;
    int cp_slots = 0, patch_slots = 0;
    while (lines >> in.patch >> in.type >> in.name >> in.attribute) {
        inputs.push_back(in);
        slots.push_back(in.patch ? patch_slots++ : cp_slots++);
    }
    tess->cp_slots = std::max(cp_slots, 1);
    tess->patch_slots = std::max(patch_slots, 1);
    tess->input_count = static_cast<int>(inputs.size());
    tess->inputs = static_cast<glm_tess_input *>(calloc(inputs.size() + 1, sizeof(glm_tess_input)));
    for (size_t n = 0; n < inputs.size(); ++n) {
        glm_tess_input &out = tess->inputs[n];
        out.attribute = inputs[n].attribute;
        out.slot = slots[n];
        out.patch = inputs[n].patch;
        out.integer = inputs[n].type.rfind("int", 0) == 0 || inputs[n].type.rfind("uint", 0) == 0;
        out.is_unsigned = inputs[n].type.rfind("uint", 0) == 0;
    }
    // 2. The vertex stage, capturing what the control stage reads.
    std::vector<std::string> names = {"gl_Position"};
    if (t.reads_point_size) names.push_back("gl_PointSize");
    for (auto &v : t.inputs) names.push_back(v.name);
    std::vector<const char *> pointers;
    for (auto &n : names) pointers.push_back(n.c_str());
    glm_compile_request capture_request = *req;
    capture_request.sources[GLM_STAGE_TESS_CONTROL] = nullptr;
    capture_request.sources[GLM_STAGE_TESS_EVALUATION] = nullptr;
    capture_request.feedback_varyings = pointers.data();
    capture_request.feedback_count = static_cast<int>(pointers.size());
    capture_request.feedback_interleaved = true;
    glm_compile_result capture;
    compile_program(&capture_request, &capture, true, allow_fp64);
    if (!capture.ok) {
        std::string log = std::string("tessellation emulation, vertex stage: ") + (capture.log ? capture.log : "");
        glm_compile_result_free(&capture);
        return log;
    }
    if (allow_fp64 && !uniform_storage_compatible(*result, capture)) {
        glm_compile_result_free(&capture);
        return "tessellation capture changed exact uniform storage";
    }
    tess->vs_capture = capture.msl[GLM_STAGE_VERTEX];
    capture.msl[GLM_STAGE_VERTEX] = nullptr;
    tess->vs_stride = capture.xfb_stride[0];
    // 3. The control stage as a compute kernel.
    std::string kernel = tess_kernel(t, capture, inputs, tess->cp_slots, tess->patch_slots, slots, tess->quads);
    glm_compile_result_free(&capture);
    glm_compile_request kernel_request = {};
    kernel_request.sources[GLM_STAGE_COMPUTE] = kernel.c_str();
    tess->kernel = static_cast<glm_compile_result *>(calloc(1, sizeof(glm_compile_result)));
    compile_program(&kernel_request, tess->kernel, false, allow_fp64);
    if (!tess->kernel->ok)
        return std::string("tessellation control kernel: ") + (tess->kernel->log ? tess->kernel->log : "") + "\n" + kernel;
    if (allow_fp64 && !uniform_storage_compatible(*result, *tess->kernel)) return "tessellation kernel changed exact uniform storage";
    if (getenv("GLM_DUMP_SHADERS")) fprintf(stderr, "glmetal: tessellation control kernel:\n%s\n", kernel.c_str());
    return "";
}

} // namespace

extern "C" char *glm_glsl_rename(const char *source, const char *const *from, const char *const *to, int count)
{
    std::map<std::string, std::string> renames;
    for (int i = 0; i < count; ++i) renames[from[i]] = to[i];
    return copy(rename_words(normalize_newlines(source), renames));
}

#include "shader_clip.h"

/* compile_cache.cpp: this build's stamp keys the disk cache. */
#include "compiler_stamp.h"
extern "C" const char glm_compiler_build[] = GLM_COMPILER_STAMP;
extern "C" bool glm_compile_cache_load(const glm_compile_request *req, glm_compile_result *res);
extern "C" void glm_compile_cache_store(const glm_compile_request *req, const glm_compile_result *res);
extern "C" void glm_compile_dump(const glm_compile_request *req, const glm_compile_result *res);

static void compile_uncached(const glm_compile_request *req, glm_compile_result *result);

extern "C" void glm_program_compile(const glm_compile_request *req, glm_compile_result *result)
{
    if (!glm_compile_cache_load(req, result)) {
        // Natively in the helper process when this one runs translated.
        if (!glm_remote_compile(req, result)) compile_uncached(req, result);
        glm_compile_cache_store(req, result);
    }
    glm_compile_dump(req, result);
}

static void compile_uncached_names(const glm_compile_request *req, glm_compile_result *result, bool allow_fp64 = true);
static void compile_uncached(const glm_compile_request *req, glm_compile_result *result)
{
    compile_uncached_names(req, result);
    strip_kw_prefixes(result);
}

static void compile_uncached_names(const glm_compile_request *req, glm_compile_result *result, bool allow_fp64)
{
    bool using_fp64 = false;
    compile_program(req, result, false, allow_fp64, &using_fp64);
    bool tessellated = req->sources[GLM_STAGE_TESS_CONTROL] || req->sources[GLM_STAGE_TESS_EVALUATION];
    if (result->ok && (tessellated || req->sources[GLM_STAGE_GEOMETRY])) {
        std::string error = tessellated ? tess_emulation(req, result, using_fp64) : geometry_emulation(req, result, using_fp64);
        if (!error.empty()) {
            if (using_fp64) {
                glm_compile_result_free(result);
                compile_uncached_names(req, result, false);
                return;
            }
            result->ok = false;
            std::string log = std::string(result->log ? result->log : "") + error + "\n";
            free(result->log);
            result->log = copy(log);
            return;
        }
    }
    if (result->ok && result->clip) {
        glm_clip_emulation(req,result,using_fp64);
        auto *c=result->clip;
        if(!c->vs_capture||!c->kernel||!c->kernel->ok||!c->pull||!c->pull->ok){
            glm_compile_result unsupported={};unsupported.clip=c;result->clip=nullptr;
            glm_compile_result_free(&unsupported);
        }
    }
    // Geometry feedback is compacted from its generated output stream. A VS
    // capture variant cannot reference GS/TES outputs and must not change
    // the representation selected for the actual emitting stage.
    if (!result->ok || result->gs || result->tess || !req->feedback_count || !req->sources[GLM_STAGE_VERTEX]) return;
    glm_compile_result variant;
    bool capture_fp64 = false;
    compile_program(req, &variant, true, using_fp64, &capture_fp64);
    // Both variants share uniform storage and feedback metadata. A fallback
    // in either one must select the same representation for the whole program.
    if (using_fp64 && !capture_fp64) {
        glm_compile_result_free(result);
        glm_compile_result_free(&variant);
        compile_uncached_names(req, result, false);
        return;
    }
    if (variant.ok) {
        result->msl_capture = variant.msl[GLM_STAGE_VERTEX];
        variant.msl[GLM_STAGE_VERTEX] = nullptr;
    } else {
        result->ok = false;
        std::string log = std::string(result->log ? result->log : "") + "transform feedback variant: " +
                          (variant.log ? variant.log : "") + "\n";
        free(result->log);
        result->log = copy(log);
    }
    glm_compile_result_free(&variant);
}

// GLSL's % on negative operands is undefined; Apple's compiler gives C's
// truncated remainder (sign of the dividend), SPIR-V's OpSMod the sign of
// the divisor. OpSRem has the same operands.
static void signed_remainder(std::vector<unsigned int> &words)
{
    enum { OP_SREM = 138, OP_SMOD = 139 };
    for (size_t i = 5; i < words.size();) {
        unsigned count = words[i] >> 16;
        if ((words[i] & 0xffff) == OP_SMOD) words[i] = (words[i] & 0xffff0000u) | OP_SREM;
        i += count ? count : 1;
    }
}

#include "shader_texture_bias.h"
#include "shader_fp64.h"
#include "shader_cube_shadow.h"
#include "shader_vertex_id.h"
extern "C" const char *glm_cube_msl_helpers(void)
{
    static const std::string helpers = glm_cube_shadow_helpers();
    return helpers.c_str();
}


// Border colour emulation (glm_compile_request.border_samplers): every
// sampling call on a listed sampler is repeated with glm_bw_<name> (same
// texture, opaque white border; the main sampler has a transparent black
// one). Their difference is the filter weight that fell on the border.
const char border_code[] =
    "layout(std140, set = 1, binding = 22) uniform GLMBorder { vec4 glm_border_color[32]; vec4 glm_border_clamp[32]; };\n"
    "vec4 glm_border(vec4 a, vec4 w, int k) { vec4 c = glm_border_color[k]; vec4 v = mix(a, w, c); return mix(mix(v, a, equal(c, vec4(0.0))), w, equal(c, vec4(1.0))); }\n"
    "float glm_border_p(float p, int k) { return mix(p, clamp(p, 0.0, 1.0), glm_border_clamp[k].x); }\n"
    "vec2 glm_border_p(vec2 p, int k) { return mix(p, clamp(p, 0.0, 1.0), glm_border_clamp[k].xy); }\n"
    "vec3 glm_border_p(vec3 p, int k) { return mix(p, clamp(p, 0.0, 1.0), glm_border_clamp[k].xyz); }\n";

// Sampling calls on `declared` samplers, repeated with glm_bw_<name>.
std::string border_calls(const std::string &text, const std::map<std::string, int> &declared)
{
    // GL_CLAMP clamps the coordinate (argument 2) of these; projective ones
    // are sampled unclamped.
    static const std::set<std::string> clamped = {
        "texture", "texture1D", "texture2D", "texture3D", "textureLod", "texture1DLod", "texture2DLod", "texture3DLod",
        "textureOffset", "textureLodOffset", "textureGrad", "textureGradOffset", "texture2DLodARB", "texture2DGradARB"};
    static const std::set<std::string> projective = {
        "textureProj", "textureProjLod", "textureProjOffset", "textureProjLodOffset", "textureProjGrad",
        "texture1DProj", "texture2DProj", "texture3DProj", "texture1DProjLod", "texture2DProjLod", "texture3DProjLod"};
    auto trim = [](const std::string &v) {
        size_t a = v.find_first_not_of(" \t\n"), b = v.find_last_not_of(" \t\n");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    };
    std::string result;
    size_t i = 0;
    while (i < text.size()) {
        if (!ident_start(text[i]) || (i > 0 && (ident_char(text[i - 1]) || text[i - 1] == '.'))) {
            result += text[i++];
            continue;
        }
        size_t start = i;
        while (i < text.size() && ident_char(text[i])) ++i;
        std::string word = text.substr(start, i - start);
        bool clamp = clamped.count(word) != 0, proj = projective.count(word) != 0;
        size_t open = i;
        while (open < text.size() && std::isspace(static_cast<unsigned char>(text[open]))) ++open;
        if ((!clamp && !proj) || open >= text.size() || text[open] != '(') {
            result += word;
            continue;
        }
        // The call's top-level arguments.
        std::vector<std::string> args(1);
        int depth = 0;
        size_t j = open + 1;
        for (; j < text.size(); ++j) {
            char c = text[j];
            if (c == '(' || c == '[') ++depth;
            else if ((c == ')' || c == ']') && depth > 0) --depth;
            else if (c == ')') break;
            else if (c == ',' && depth == 0) {
                args.emplace_back();
                continue;
            }
            args.back() += c;
        }
        std::string sampler = trim(args[0]);
        auto found = declared.find(sampler);
        if (j >= text.size() || args.size() < 2 || found == declared.end()) {
            result += word;
            continue;
        }
        for (size_t a = 1; a < args.size(); ++a) args[a] = border_calls(args[a], declared);
        std::string k = std::to_string(found->second);
        if (clamp) args[1] = "glm_border_p(" + args[1] + ", " + k + ")";
        std::string rest;
        for (size_t a = 1; a < args.size(); ++a) rest += "," + args[a];
        result += "glm_border(" + word + "(" + sampler + rest + "), " + word + "(glm_bw_" + sampler + rest + "), " + k + ")";
        i = j + 1;
    }
    return result;
}

std::string border_rewrite(const std::string &text, const std::map<std::string, int> &border)
{
    std::string out = text;
    std::map<std::string, int> declared;
    for (auto &[name, slot] : border) {
        std::regex decl("uniform\\s+(?:(?:lowp|mediump|highp)\\s+)?(sampler1D|sampler2D|sampler3D|sampler2DRect|"
                        "sampler1DArray|sampler2DArray)\\s+([^;]+);");
        std::regex scalar("(^|,)\\s*" + name + "\\s*(,|$)");
        std::smatch m;
        bool found = false;
        size_t end = 0;
        std::string type;
        for (std::sregex_iterator it(out.begin(), out.end(), decl), last; it != last; ++it) {
            if (!std::regex_search((*it)[2].str(), scalar)) continue;
            end = static_cast<size_t>(it->position(0) + it->length(0));
            type = (*it)[1].str();
            found = true;
            break;
        }
        if (!found) continue;
        out.insert(end, " uniform " + type + " glm_bw_" + name + ";");
        declared[name] = slot;
    }
    if (declared.empty()) return text;
    std::string result = border_calls(out, declared);
    size_t line = result.find("#line 1\n");
    if (line == std::string::npos) line = 0;
    result.insert(line, border_code);
    return result;
}

// The order loose uniforms are declared in, by name: the stages' sources
// in pipeline order, first declaration wins.
std::map<std::string, int> uniform_declaration_order(const glm_compile_request *req)
{
    std::map<std::string, int> order;
    static const std::regex declaration(
        R"((^|[;}\n])\s*(?:layout\s*\([^)]*\)\s*)?uniform\s+(?:(?:lowp|mediump|highp)\s+)?\w+\s+([^;{]*);)");
    static const glm_stage stages[] = {GLM_STAGE_VERTEX, GLM_STAGE_TESS_CONTROL, GLM_STAGE_TESS_EVALUATION, GLM_STAGE_GEOMETRY,
                                       GLM_STAGE_FRAGMENT, GLM_STAGE_COMPUTE};
    for (glm_stage stage : stages) {
        if (!req->sources[stage]) continue;
        std::string text = strip_comments(normalize_newlines(req->sources[stage]));
        for (std::sregex_iterator it(text.begin(), text.end(), declaration), end; it != end; ++it) {
            // "a, b[3], c = 1.0": each declarator's identifier.
            std::string list = (*it)[2];
            int depth = 0;
            std::string current;
            auto take = [&] {
                size_t start = current.find_first_not_of(" \t\n");
                if (start != std::string::npos) {
                    size_t end = start;
                    while (end < current.size() && ident_char(current[end])) ++end;
                    std::string name = current.substr(start, end - start);
                    if (!name.empty() && !order.count(name)) order[name] = static_cast<int>(order.size());
                }
                current.clear();
            };
            for (char c : list) {
                if (c == '(' || c == '[') ++depth;
                if (c == ')' || c == ']') --depth;
                if (c == ',' && depth == 0) take();
                else current += c;
            }
            take();
        }
    }
    return order;
}

static void compile_program(const glm_compile_request *req, glm_compile_result *result, bool capture, bool allow_fp64,
                            bool *using_fp64)
{
    initialize();
    memset(result, 0, sizeof *result);
    if (using_fp64) *using_fp64 = false;
    std::string log;
    std::vector<std::unique_ptr<glslang::TShader>> shaders;
    glslang::TProgram program;
    Prepared prepared[GLM_STAGE_COUNT];
    Bindings bindings;
    // Binary64 arithmetic and uniforms can stay local to each stage. Stage
    // interfaces retain their existing 32-bit storage, except VS-only TF.
    if (allow_fp64) {
        bool eligible = true;
        for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
            if (!req->sources[s]) continue;
            std::string source = strip_comments(req->sources[s]);
            bool uses_double = source.find("double") != std::string::npos || source.find("dvec") != std::string::npos ||
                               source.find("dmat") != std::string::npos || source.find("Double2x32") != std::string::npos;
            if (!uses_double) continue;
            bindings.fp64 = true;
            std::string version_source = source, profile;
            if (take_version(version_source, &profile) < 400 && source.find("GL_ARB_gpu_shader_fp64") == std::string::npos)
                eligible = false;
        }
        bindings.fp64 &= eligible;
    }
    for (int i = 0; i < req->attribute_count; ++i) bindings.attributes[req->attributes[i].name] = req->attributes[i].location;
    for (int i = 0; i < req->frag_output_count; ++i) bindings.outputs[req->frag_outputs[i].name] = req->frag_outputs[i].location;
    for (int i = 0; i < req->feedback_count; ++i) bindings.feedback.push_back(req->feedback_varyings[i]);
    bindings.interleaved = req->feedback_interleaved;
    bindings.uint_inputs = req->uint_inputs;
    bindings.int_inputs = req->int_inputs;
    for (int k = 0; k < 32; ++k)
        if (req->border_samplers[k]) bindings.border[req->border_samplers[k]] = k;
    bindings.capture = capture;
    bool present[GLM_STAGE_COUNT] = {};
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
        if (!req->sources[s]) continue;
        present[s] = true;
        prepared[s] = prepare(req->sources[s], static_cast<glm_stage>(s), &bindings);
        if (bindings.fp64 && !prepared[s].uniform_inits.empty()) {
            compile_program(req, result, capture, false);
            return;
        }
        if (s == GLM_STAGE_FRAGMENT && !bindings.border.empty())
            prepared[s].text = border_rewrite(prepared[s].text, bindings.border);
        auto shader = std::make_unique<glslang::TShader>(language(static_cast<glm_stage>(s)));
        const char *text = prepared[s].text.c_str();
        shader->setStrings(&text, 1);
        configure(*shader, language(static_cast<glm_stage>(s)));
        if (!shader->parse(shader_resources(req->sources[s]), 410, false, messages)) {
            log += shader->getInfoLog();
            result->log = copy(log);
            return;
        }
        program.addShader(shader.get());
        shaders.push_back(std::move(shader));
    }
    if (!program.link(messages)) {
        log += program.getInfoLog();
        result->log = copy(log);
        return;
    }
    // OpenGL links implicit varying locations by name across stages. The
    // generic mapper assigns each stage independently, so declaration order
    // can connect a vertex output to an unrelated fragment input.
    glslang::TIntermediate *first_stage = nullptr;
    for (int s = 0; s < GLM_STAGE_COUNT && !first_stage; ++s)
        if (present[s]) first_stage = program.getIntermediate(language(static_cast<glm_stage>(s)));
    if (!first_stage) {
        result->log = copy("Program has no shader stages.");
        return;
    }
    glslang::TDefaultGlslIoResolver io_resolver(*first_stage);
    glslang::TGlslIoMapper io_mapper;
    if (!program.mapIO(&io_resolver, &io_mapper)) {
        log += program.getInfoLog();
        result->log = copy(log);
        return;
    }
    result->writes_frag_color = present[GLM_STAGE_FRAGMENT] && prepared[GLM_STAGE_FRAGMENT].writes_frag_color;
    {
        std::vector<glm_subroutine> subroutines;
        std::vector<glm_subroutine_uniform> uniforms;
        for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
            if (!present[s]) continue;
            for (auto &f : prepared[s].subroutines) subroutines.push_back({s, f.second, copy(f.first)});
            for (auto &u : prepared[s].subroutine_uniforms) {
                glm_subroutine_uniform info = {};
                info.stage = s;
                info.location = u.location;
                info.array_size = u.array_size;
                info.name = copy(u.name);
                for (int c : u.compatible)
                    if (info.compatible_count < 32) info.compatible[info.compatible_count++] = c;
                uniforms.push_back(info);
            }
        }
        result->subroutine_count = static_cast<int>(subroutines.size());
        result->subroutines = static_cast<glm_subroutine *>(calloc(subroutines.size() + 1, sizeof(glm_subroutine)));
        std::copy(subroutines.begin(), subroutines.end(), result->subroutines);
        result->subroutine_uniform_count = static_cast<int>(uniforms.size());
        result->subroutine_uniforms =
            static_cast<glm_subroutine_uniform *>(calloc(uniforms.size() + 1, sizeof(glm_subroutine_uniform)));
        std::copy(uniforms.begin(), uniforms.end(), result->subroutine_uniforms);
    }
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) result->legacy_parts |= prepared[s].legacy_parts;

    // SPIR-V per stage, then one global view of resources across stages.
    std::vector<uint32_t> spirv[GLM_STAGE_COUNT];
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
        if (!present[s]) continue;
        glslang::SpvOptions options;
        options.generateDebugInfo = false;
        options.disableOptimizer = true;
        glslang::GlslangToSpv(*program.getIntermediate(language(static_cast<glm_stage>(s))), spirv[s], &options);
        signed_remainder(spirv[s]);
    }
    if (bindings.fp64) {
        bool supported = true;
        for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
            if (present[s] && !glm_fp64_supported(spirv[s])) supported = false;
        }
        bool vertex_only = present[GLM_STAGE_VERTEX];
        for (int s = 0; s < GLM_STAGE_COUNT; ++s)
            if (s != GLM_STAGE_VERTEX && present[s]) vertex_only = false;
        for (int s = 0; supported && s < GLM_STAGE_COUNT; ++s) {
            if (!present[s] || (s == GLM_STAGE_VERTEX && vertex_only)) continue;
            // Metal raster outputs cannot contain ulong, including varyings
            // unused by the fragment stage. Keep both normal and capture
            // variants on the same representation for these programs.
            spirv_cross::Compiler reflect(spirv[s]);
            auto contains_double = [&](auto &&self, uint32_t id) -> bool {
                const auto &type = reflect.get_type(id);
                if (type.basetype == spirv_cross::SPIRType::Double) return true;
                for (auto member : type.member_types) if (self(self, member)) return true;
                return false;
            };
            for (const auto &output : reflect.get_shader_resources().stage_outputs)
                if (contains_double(contains_double, output.type_id)) supported = false;
        }
        if (!supported) {
            glm_compile_result_free(result);
            compile_program(req, result, capture, false);
            return;
        }
    }
    if (using_fp64) *using_fp64 = bindings.fp64;

    std::map<std::string, int> sampler_slots, block_slots, block_sizes;
    std::map<std::string, uint32_t> block_stages;
    std::vector<std::pair<std::string, BlockMember>> block_members;
    std::map<std::string, glm_uniform_info> uniforms;
    int next_block = GLM_SLOT_FIRST_UBO;
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
        if (!present[s]) continue;
        spirv_cross::Compiler reflect(spirv[s]);
        auto resources = reflect.get_shader_resources();
        for (auto &image : resources.sampled_images) {
            std::string name = reflect.get_name(image.id);
            if (name.rfind("glm_bw_", 0) == 0) continue; // after the program's own samplers, below
            if (!sampler_slots.count(name)) {
                int slot = static_cast<int>(sampler_slots.size());
                sampler_slots[name] = slot;
                glm_uniform_info info = {};
                info.name = nullptr;
                const auto &type = reflect.get_type(image.type_id);
                info.type = gl_sampler_type(reflect, type);
                info.array_size = type.array.empty() ? 1 : static_cast<int>(type.array[0]);
                info.is_array = !type.array.empty();
                info.offset = -1;
                info.sampler_slot = slot;
                info.legacy = -1;
                uniforms[name] = info;
                // Array samplers take consecutive slots.
                for (int k = 1; k < info.array_size; ++k) sampler_slots[name + "#" + std::to_string(k)] = slot + k;
            }
        }
        for (auto &block : resources.uniform_buffers) {
            std::string name = reflect.get_name(block.base_type_id);
            if (name == "GLMGlobals" || name == "GLMLegacy" || name == "GLMPoint" || name == "GLMARB" ||
                name == "GLMXfbInfo" || name == "GLMGsInfo" || name == "GLMBorder")
                continue;
            // Arrays of blocks are blocks "Block[0]", "Block[1]"... on
            // consecutive buffer slots (their members listed once).
            const auto &var_type = reflect.get_type(block.type_id);
            int elements = var_type.array.empty() ? 0 : static_cast<int>(var_type.array[0]);
            std::string first = elements ? name + "[0]" : name;
            for (int e = 0; e < std::max(elements, 1); ++e)
                block_stages[elements ? name + "[" + std::to_string(e) + "]" : name] |= 1u << s;
            if (block_slots.count(first)) continue;
            const auto &type = reflect.get_type(block.base_type_id);
            for (int e = 0; e < std::max(elements, 1); ++e) {
                std::string element = elements ? name + "[" + std::to_string(e) + "]" : name;
                block_slots[element] = next_block++;
                /* Rounded up to a vec4, as Apple reports GL_UNIFORM_BLOCK_DATA_SIZE. */
                block_sizes[element] = (static_cast<int>(reflect.get_declared_struct_size(type)) + 15) & ~15;
            }
            // Members, named as GL names them: "Block.member" when the
            // block has an instance name.
            std::string prefix = reflect.get_name(block.id).empty() ? "" : name + ".";
            reflect_members(reflect, type, prefix, 0, first, block_members);
        }
        // Loose uniforms: members of GLMGlobals.
        for (auto &block : resources.uniform_buffers) {
            if (reflect.get_name(block.base_type_id) != "GLMGlobals") continue;
            const auto &type = reflect.get_type(block.base_type_id);
            result->global_size = std::max(result->global_size, static_cast<int>(reflect.get_declared_struct_size(type)));
            for (uint32_t m = 0; m < type.member_types.size(); ++m) {
                std::string name = reflect.get_member_name(block.base_type_id, m);
                if (uniforms.count(name)) continue;
                const auto &member = reflect.get_type(type.member_types[m]);
                glm_uniform_info info = {};
                info.offset = static_cast<int>(reflect.type_struct_member_offset(type, m));
                info.array_size = member.array.empty() ? 1 : static_cast<int>(member.array[0]);
                info.is_array = !member.array.empty();
                info.array_stride = member.array.empty() ? 0 : static_cast<int>(reflect.type_struct_member_array_stride(type, m));
                info.matrix_stride = member.columns > 1 ? static_cast<int>(reflect.type_struct_member_matrix_stride(type, m)) : 0;
                info.type = member.basetype == spirv_cross::SPIRType::Struct ? 0 : gl_type_of(member);
                info.sampler_slot = -1;
                info.legacy = -1;
                if (info.type == 0) {
                    // Struct uniforms: a uniform per leaf member, through
                    // nested structs and arrays of them ("s[0].b[1].c").
                    std::function<void(const spirv_cross::SPIRType &, const std::string &, int)> add_struct;
                    add_struct = [&](const spirv_cross::SPIRType &st, const std::string &prefix, int base) {
                        for (uint32_t f = 0; f < st.member_types.size(); ++f) {
                            const auto &field = reflect.get_type(st.member_types[f]);
                            std::string fname = prefix + "." + reflect.get_member_name(st.self, f);
                            int offset = base + static_cast<int>(reflect.type_struct_member_offset(st, f));
                            int count = field.array.empty() ? 1 : static_cast<int>(field.array[0]);
                            int stride = field.array.empty() ? 0 : static_cast<int>(reflect.type_struct_member_array_stride(st, f));
                            if (field.basetype == spirv_cross::SPIRType::Struct) {
                                for (int e = 0; e < count; ++e)
                                    add_struct(field, fname + (field.array.empty() ? "" : "[" + std::to_string(e) + "]"), offset + e * stride);
                                continue;
                            }
                            glm_uniform_info fi = {};
                            fi.offset = offset;
                            fi.array_size = count;
                            fi.is_array = !field.array.empty();
                            fi.array_stride = stride;
                            fi.matrix_stride = field.columns > 1 ? static_cast<int>(reflect.type_struct_member_matrix_stride(st, f)) : 0;
                            fi.type = gl_type_of(field);
                            fi.sampler_slot = -1;
                            fi.legacy = -1;
                            uniforms[fname] = fi;
                        }
                    };
                    for (int e = 0; e < info.array_size; ++e)
                        add_struct(member, name + (member.array.empty() ? "" : "[" + std::to_string(e) + "]"),
                                   info.offset + e * info.array_stride);
                    continue;
                }
                uniforms[name] = info;
            }
        }
    }

    // Bools live in uniform blocks as uints in SPIR-V: glslang's reflection of
    // the GLSL still knows them (GL_BOOL..GL_BOOL_VEC4, as Apple reports).
    if (program.buildReflection(EShReflectionDefault | EShReflectionAllBlockVariables)) {
        std::map<std::string, int> gl_types;
        for (int i = 0; i < program.getNumUniformVariables(); ++i) {
            const auto &u = program.getUniform(i);
            gl_types[u.name] = u.glDefineType;
        }
        for (auto &entry : uniforms) {
            auto found = gl_types.find(entry.first);
            if (found == gl_types.end()) found = gl_types.find(entry.first + "[0]");
            if (found != gl_types.end() && found->second >= 0x8B56 && found->second <= 0x8B59) entry.second.type = found->second;
        }
        // Block members: glslang's names index arrays (of blocks, structs
        // and members) differently; every element has the same type, so
        // names compare without their indices.
        auto unindexed = [](const std::string &name) {
            std::string out;
            for (size_t i = 0; i < name.size(); ++i) {
                if (name[i] == '[') {
                    size_t close = name.find(']', i);
                    if (close != std::string::npos) { i = close; continue; }
                }
                out += name[i];
            }
            return out;
        };
        std::map<std::string, int> bool_types;
        for (const auto &entry : gl_types)
            if (entry.second >= 0x8B56 && entry.second <= 0x8B59) bool_types[unindexed(entry.first)] = entry.second;
        // Blocks no stage uses are not in glslang's reflection: their bool
        // members come from the declarations ("bvec4 g[8];" in "uniform
        // Block { ... } inst;" is "Block.g").
        static const std::regex block_decl(R"(uniform\s+(\w+)\s*\{([^}]*)\}\s*(\w*))");
        static const std::regex bool_member(R"((?:^|;)\s*(?:(?:lowp|mediump|highp|layout\s*\([^)]*\))\s+)*(bool|bvec[234])\s+([^;]*);)");
        for (int stage = 0; stage < GLM_STAGE_COUNT; ++stage) {
            if (!req->sources[stage]) continue;
            std::string text = strip_comments(normalize_newlines(req->sources[stage]));
            for (std::sregex_iterator b(text.begin(), text.end(), block_decl), end; b != end; ++b) {
                std::string block = (*b)[1], body = (*b)[2], prefix = std::string((*b)[3]).empty() ? "" : block + ".";
                for (std::sregex_iterator m(body.begin(), body.end(), bool_member); m != end; ++m) {
                    std::string type = (*m)[1];
                    int gl = type == "bool" ? 0x8B56 : 0x8B56 + (type.back() - '1');
                    std::string list = (*m)[2];
                    for (size_t start = 0; start <= list.size();) {
                        size_t comma = list.find(',', start);
                        std::string decl = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                        size_t first = decl.find_first_not_of(" \t\n");
                        if (first != std::string::npos) {
                            size_t last = first;
                            while (last < decl.size() && ident_char(decl[last])) ++last;
                            std::string key = unindexed(prefix + decl.substr(first, last - first));
                            if (!bool_types.count(key)) bool_types[key] = gl;
                        }
                        if (comma == std::string::npos) break;
                        start = comma + 1;
                    }
                }
            }
        }
        for (auto &member : block_members) {
            auto found = bool_types.find(unindexed(member.first));
            if (found != bool_types.end()) member.second.info.type = static_cast<uint32_t>(found->second);
        }
    }

    // Uniform initializers: the values the global block starts with.
    {
        std::vector<UniformInit> inits;
        std::string defs;
        std::set<std::string> seen;
        const TBuiltInResource *resources = GetDefaultResources();
        bool selected_resources = false;
        for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
            if (!present[s]) continue;
            if (!selected_resources) {
                resources = shader_resources(req->sources[s]);
                selected_resources = true;
            }
            for (const auto &u : prepared[s].uniform_inits)
                if (seen.insert(u.name).second) inits.push_back(u);
            defs += prepared[s].init_defs;
        }
        if (!inits.empty() && result->global_size > 0) {
            std::vector<uint8_t> globals(static_cast<size_t>(result->global_size));
            initial_globals(inits, defs, uniforms, globals, resources);
            result->initial_globals = static_cast<unsigned char *>(malloc(globals.size()));
            memcpy(result->initial_globals, globals.data(), globals.size());
        }
    }

    // Border emulation samplers: after the program's own, in base slot order
    // (the backend binds them there).
    {
        int next = static_cast<int>(sampler_slots.size());
        for (int k = 0; k < 32; ++k)
            if (req->border_samplers[k]) sampler_slots[std::string("glm_bw_") + req->border_samplers[k]] = next++;
    }
    // MSL per stage.
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
        if (!present[s]) continue;
        // Geometry (and tessellation) stages are emulated: no MSL of their own.
        // A tessellation evaluation stage without a vertex stage is the
        // post-tessellation vertex function of tessellation emulation.
        bool eval = s == GLM_STAGE_TESS_EVALUATION && !present[GLM_STAGE_VERTEX];
        if (s == GLM_STAGE_GEOMETRY || s == GLM_STAGE_TESS_CONTROL || (s == GLM_STAGE_TESS_EVALUATION && !eval)) continue;
        int out = eval ? GLM_STAGE_VERTEX : s;
        auto stage_spirv = s == GLM_STAGE_VERTEX ? vertex_id_spirv(spirv[s]) : spirv[s];
        auto cube_code = cube_shadow_spirv(without_rect_images(texture_bias_spirv(stage_spirv, sampler_slots)), sampler_slots);
        GLMCubeCompilerMSL msl(cube_code, bindings.fp64);
        if (s == GLM_STAGE_FRAGMENT) {
            const auto &capabilities = msl.get_declared_capabilities();
            if (std::find(capabilities.begin(), capabilities.end(), spv::CapabilitySampleRateShading) != capabilities.end()) {
                // SampleID/Position imply sample-rate default interpolation.
                // Keep explicitly flat and centroid interfaces unchanged.
                auto inputs = msl.get_shader_resources().stage_inputs;
                for (const auto &input : inputs) {
                    if (msl.has_decoration(input.id, spv::DecorationFlat) ||
                        msl.has_decoration(input.id, spv::DecorationCentroid)) continue;
                    const auto &type = msl.get_type(input.base_type_id);
                    if (type.basetype == spirv_cross::SPIRType::Struct) {
                        for (uint32_t member = 0; member < type.member_types.size(); ++member) {
                            if (msl.has_member_decoration(type.self, member, spv::DecorationFlat) ||
                                msl.has_member_decoration(type.self, member, spv::DecorationCentroid)) continue;
                            const auto &field = msl.get_type(type.member_types[member]);
                            if (field.basetype == spirv_cross::SPIRType::Float || field.basetype == spirv_cross::SPIRType::Half)
                                msl.set_member_decoration(type.self, member, spv::DecorationSample);
                        }
                    } else if (type.basetype == spirv_cross::SPIRType::Float || type.basetype == spirv_cross::SPIRType::Half)
                        msl.set_decoration(input.id, spv::DecorationSample);
                }
            }
        }
        auto options = msl.get_msl_options();
        options.platform = spirv_cross::CompilerMSL::Options::macOS;
        options.set_msl_version(2, 3);
        options.enable_decoration_binding = false;
        options.pad_fragment_output_components = true;
        options.texture_1D_as_2D = true;
        options.texture_buffer_native = true;
        // The capture variant runs in its own pass with rasterization off.
        if ((capture || (bindings.fp64 && !present[GLM_STAGE_FRAGMENT])) && s == GLM_STAGE_VERTEX)
            options.disable_rasterization = true;
        msl.set_msl_options(options);
        auto common = msl.get_common_options();
        common.vertex.flip_vert_y = true;
        common.vertex.fixup_clipspace = true;
        msl.set_common_options(common);
        // The post-tessellation function's patch(..., N) must name the
        // control point count, which only the control stage declares.
        if (eval && req->tess_output_vertices)
            msl.set_execution_mode(spv::ExecutionModeOutputVertices, static_cast<uint32_t>(req->tess_output_vertices));
        auto model = msl.get_execution_model();
        auto resources = msl.get_shader_resources();
        {
            auto active = msl.get_shader_resources(msl.get_active_interface_variables());
            uint8_t bit = out == GLM_STAGE_FRAGMENT ? 2 : 1;
            for (auto &block : active.uniform_buffers) {
                std::string block_name = msl.get_name(block.base_type_id);
                if (block_name == "GLMLegacy") {
                    result->uses_legacy = true;
                    result->legacy_stages |= bit;
                }
                if (block_name == "GLMGlobals") result->globals_stages |= bit;
            }
        }
        auto bind = [&](spirv_cross::ID id, int buffer, int texture, int sampler) {
            spirv_cross::MSLResourceBinding b;
            b.stage = model;
            b.desc_set = msl.get_decoration(id, spv::DecorationDescriptorSet);
            b.binding = msl.get_decoration(id, spv::DecorationBinding);
            b.msl_buffer = buffer >= 0 ? buffer : 0;
            b.msl_texture = texture >= 0 ? texture : 0;
            b.msl_sampler = sampler >= 0 ? sampler : 0;
            msl.add_msl_resource_binding(b);
        };
        for (auto &block : resources.uniform_buffers) {
            std::string name = msl.get_name(block.base_type_id);
            int slot = name == "GLMGlobals" ? GLM_SLOT_GLOBALS
                       : name == "GLMLegacy" ? GLM_SLOT_LEGACY
                       : name == "GLMARB" ? GLM_SLOT_ARB
                       : name == "GLMPoint" ? GLM_SLOT_POINT
                       : name == "GLMLodBias" ? GLM_SLOT_LOD_BIAS
                       : name == "GLMBorder" ? GLM_SLOT_BORDER
                       : name == "GLMXfbInfo" ? GLM_SLOT_XFB + 4
                       : name == "GLMGsInfo" ? GLM_SLOT_GS_INFO
                       : block_slots.count(name) ? block_slots[name]
                                                 : block_slots[name + "[0]"]; /* arrays from element 0 */
            // glslang's automatic bindings can give two blocks of a stage
            // the same (set, binding): each block gets its own, its slot.
            msl.set_decoration(block.id, spv::DecorationDescriptorSet, 7);
            msl.set_decoration(block.id, spv::DecorationBinding, static_cast<uint32_t>(slot));
            bind(block.id, slot, -1, -1);
        }
        for (auto &buffer : resources.storage_buffers) {
            std::string name = msl.get_name(buffer.base_type_id);
            if (name == "GLMVertexIDs" || name == "glm_vertex_ids") bind(buffer.id, GLM_SLOT_STREAM, -1, -1);
            else if (name.rfind("GLMXfb", 0) == 0) bind(buffer.id, GLM_SLOT_XFB + std::atoi(name.c_str() + 6), -1, -1);
            else if (name == "GLMGsIn") bind(buffer.id, GLM_SLOT_GS_INPUT, -1, -1);
            else if (name == "GLMGsVertices") bind(buffer.id, GLM_SLOT_GS_VERTICES, -1, -1);
            else if (name == "GLMGsIndices") bind(buffer.id, GLM_SLOT_GS_INDICES, -1, -1);
            else if (name == "GLMTcsOut") bind(buffer.id, GLM_SLOT_GS_VERTICES, -1, -1);
            else if (name == "GLMTcsPatch") bind(buffer.id, GLM_SLOT_GS_INDICES, -1, -1);
            else if (name == "GLMTcsFactors") bind(buffer.id, GLM_SLOT_TESS_FACTORS, -1, -1);
        }
        for (auto &image : resources.sampled_images) {
            std::string name = msl.get_name(image.id);
            auto cube = cube_code.resources.find(name);
            int slot = cube != cube_code.resources.end() ? cube->second : sampler_slots[name];
            bind(image.id, -1, cube != cube_code.resources.end() ? 64 + slot : slot, slot);
        }
        for (auto &image : resources.separate_images) {
            auto cube = cube_code.resources.find(msl.get_name(image.id));
            if (cube != cube_code.resources.end()) bind(image.id, -1, 64 + cube->second, cube->second);
        }
        // GL names of the stage inputs: compiling renames those that are
        // MSL keywords ("vertex" becomes "vertex0").
        std::map<uint32_t, std::string> input_names;
        for (auto &input : resources.stage_inputs) input_names[input.id] = msl.get_name(input.id);
        try {
            std::string text = msl.compile();
            // SPIRV-Cross pads array elements that already fill their
            // stride with zero bytes, which Metal rejects.
            const std::string padded = "template <typename T, int stride>\nstruct spvPaddedArrayElement { T data; char padding[stride - sizeof(T)]; };";
            if (size_t at = text.find(padded); at != std::string::npos)
                text.replace(at, padded.size(),
                             "template <typename T, int stride, bool = (stride > int(sizeof(T)))>\n"
                             "struct spvPaddedArrayElement { T data; char padding[stride - sizeof(T)]; };\n"
                             "template <typename T, int stride>\nstruct spvPaddedArrayElement<T, stride, false> { T data; };");
            result->msl[out] = copy(text);
            if (eval) result->tess_inputs = copy(stage_in_members(text));
        } catch (const std::exception &error) {
            log += std::string("SPIR-V to MSL: ") + error.what() + "\n";
            result->log = copy(log);
            return;
        }
        if (s == GLM_STAGE_VERTEX) {
            std::vector<glm_io_info> inputs;
            auto active = msl.get_active_interface_variables();
            for (auto &input : resources.stage_inputs) {
                glm_io_info io = {};
                io.index = active.count(input.id) ? 0 : GLM_INPUT_INACTIVE;
                std::string name = input_names.count(input.id) ? input_names[input.id] : msl.get_name(input.id);
                if (name.rfind("glm_", 0) == 0) {
                    // Legacy attribute stand-ins answer to their GL names.
                    name = "gl_" + name.substr(4);
                    if (name.size() > 3 && name.compare(name.size() - 3, 3, "_in") == 0) name.resize(name.size() - 3);
                }
                io.name = copy(name);
                const auto &type = msl.get_type(input.type_id);
                io.type = gl_type_of(type);
                io.location = static_cast<int>(msl.get_decoration(input.id, spv::DecorationLocation));
                io.array_size = 1;
                io.integer = type.basetype == spirv_cross::SPIRType::Int || type.basetype == spirv_cross::SPIRType::UInt;
                inputs.push_back(io);
            }
            // gl_InstanceID / gl_VertexID are active attributes to GL (Apple
            // lists them, located after the program's own).
            int next_location = 0;
            for (const auto &io : inputs)
                if (io.index == 0) next_location = std::max(next_location, io.location + 1);
            std::vector<std::string> builtins;
            for (auto &input : resources.builtin_inputs) {
                if (!active.count(input.resource.id)) continue;
                if (input.builtin == spv::BuiltInInstanceIndex) builtins.push_back("gl_InstanceID");
                if (input.builtin == spv::BuiltInVertexIndex) builtins.push_back("gl_VertexID");
            }
            std::sort(builtins.begin(), builtins.end());
            for (const auto &name : builtins) {
                glm_io_info io = {};
                io.name = copy(name);
                io.type = 0x1404; /* GL_INT */
                io.location = next_location++;
                io.index = GLM_INPUT_BUILTIN;
                io.array_size = 1;
                io.integer = true;
                inputs.push_back(io);
            }
            /* Listed by name (Apple's order for these). */
            std::stable_sort(inputs.begin(), inputs.end(),
                             [](const glm_io_info &a, const glm_io_info &b) { return strcmp(a.name, b.name) < 0; });
            result->attribute_count = static_cast<int>(inputs.size());
            result->attributes = static_cast<glm_io_info *>(calloc(inputs.size() + 1, sizeof(glm_io_info)));
            std::copy(inputs.begin(), inputs.end(), result->attributes);
        }
        if (s == GLM_STAGE_FRAGMENT) {
            for (auto &input : resources.stage_inputs)
                if (msl.has_decoration(input.id, spv::DecorationFlat)) result->flat_inputs = true;
            std::vector<glm_io_info> outputs;
            for (auto &output : resources.stage_outputs) {
                glm_io_info io = {};
                io.name = copy(msl.get_name(output.id));
                const auto &type = msl.get_type(output.type_id);
                io.type = gl_type_of(type);
                io.location = static_cast<int>(msl.get_decoration(output.id, spv::DecorationLocation));
                io.index = static_cast<int>(msl.get_decoration(output.id, spv::DecorationIndex));
                io.array_size = type.array.empty() ? 1 : static_cast<int>(type.array[0]);
                io.integer = type.basetype == spirv_cross::SPIRType::Int || type.basetype == spirv_cross::SPIRType::UInt;
                outputs.push_back(io);
            }
            result->output_count = static_cast<int>(outputs.size());
            result->outputs = static_cast<glm_io_info *>(calloc(outputs.size() + 1, sizeof(glm_io_info)));
            std::copy(outputs.begin(), outputs.end(), result->outputs);
        }
    }

    if (present[GLM_STAGE_VERTEX] && !bindings.feedback.empty()) {
        std::string error = reflect_feedback(spirv[!capture && present[GLM_STAGE_GEOMETRY] ? GLM_STAGE_GEOMETRY :
                               !capture && present[GLM_STAGE_TESS_EVALUATION] ? GLM_STAGE_TESS_EVALUATION : GLM_STAGE_VERTEX], bindings, result);
        if (!error.empty()) { result->log = copy(error); return; }
    }
    result->sampler_count = static_cast<int>(sampler_slots.size());
    result->uniform_count = static_cast<int>(uniforms.size());
    result->uniforms = static_cast<glm_uniform_info *>(calloc(uniforms.size() + 1, sizeof(glm_uniform_info)));
    // Active uniforms in declaration order (stage by stage), as Apple's
    // implementation lists and locates them; then the blocks' members by
    // block index, each block's in declaration order.
    std::map<std::string, int> declared = uniform_declaration_order(req);
    std::vector<std::pair<std::string, glm_uniform_info>> ordered(uniforms.begin(), uniforms.end());
    auto rank = [&](const std::string &name) {
        auto found = declared.find(name.substr(0, name.find_first_of(".[")));
        return found == declared.end() ? INT32_MAX : found->second;
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&](const auto &a, const auto &b) { return rank(a.first) < rank(b.first); });
    int index = 0;
    for (auto &entry : ordered) {
        result->uniforms[index] = entry.second;
        result->uniforms[index].name = copy(entry.first);
        ++index;
    }
    std::stable_sort(block_members.begin(), block_members.end(), [&](const auto &a, const auto &b) {
        return std::distance(block_slots.begin(), block_slots.find(a.second.block)) <
               std::distance(block_slots.begin(), block_slots.find(b.second.block));
    });
    result->block_uniform_count = static_cast<int>(block_members.size());
    result->block_uniforms = static_cast<glm_uniform_info *>(calloc(block_members.size() + 1, sizeof(glm_uniform_info)));
    index = 0;
    for (auto &member : block_members) {
        glm_uniform_info &u = result->block_uniforms[index++];
        u = member.second.info;
        u.name = copy(member.first);
        u.block = static_cast<int>(std::distance(block_slots.begin(), block_slots.find(member.second.block)));
    }
    result->block_count = static_cast<int>(block_slots.size());
    result->blocks = static_cast<glm_block_info *>(calloc(block_slots.size() + 1, sizeof(glm_block_info)));
    index = 0;
    for (auto &entry : block_slots) {
        result->blocks[index].name = copy(entry.first);
        result->blocks[index].slot = entry.second;
        result->blocks[index].size = block_sizes[entry.first];
        result->blocks[index].stages = block_stages[entry.first];
        ++index;
    }
    if (!capture && present[GLM_STAGE_VERTEX] && present[GLM_STAGE_FRAGMENT] &&
        !present[GLM_STAGE_GEOMETRY] && !present[GLM_STAGE_TESS_CONTROL] && !present[GLM_STAGE_TESS_EVALUATION])
        result->clip=glm_clip_reflect(spirv[GLM_STAGE_VERTEX],spirv[GLM_STAGE_FRAGMENT]);
    result->ok = true;
    result->log = copy(log);
}

extern "C" void glm_compile_result_free(glm_compile_result *r)
{
    free(r->log);
    free(r->initial_globals);
    free(r->msl_capture);
    if(r->clip){
        free(r->clip->vs_capture);
        for(int i=0;i<r->clip->varying_count;++i)free(r->clip->varyings[i].name);
        free(r->clip->varyings);
        if(r->clip->kernel)glm_compile_result_free(r->clip->kernel);
        if(r->clip->pull)glm_compile_result_free(r->clip->pull);
        free(r->clip->kernel);free(r->clip->pull);free(r->clip);
    }
    if (r->gs) {
        free(r->gs->vs_capture);
        if (r->gs->kernel) glm_compile_result_free(r->gs->kernel);
        if (r->gs->pull) glm_compile_result_free(r->gs->pull);
        free(r->gs->kernel);
        free(r->gs->pull);
        free(r->gs);
    }
    for (int i = 0; i < r->xfb_count; ++i) free(r->xfb[i].name);
    if (r->tess) {
        free(r->tess->vs_capture);
        free(r->tess->inputs);
        if (r->tess->kernel) glm_compile_result_free(r->tess->kernel);
        if (r->tess->eval) glm_compile_result_free(r->tess->eval);
        free(r->tess->kernel);
        free(r->tess->eval);
        free(r->tess);
    }
    free(r->tess_inputs);
    for (int i = 0; i < r->subroutine_count; ++i) free(r->subroutines[i].name);
    free(r->subroutines);
    for (int i = 0; i < r->subroutine_uniform_count; ++i) free(r->subroutine_uniforms[i].name);
    free(r->subroutine_uniforms);
    free(r->xfb);
    for (auto &text : r->msl) free(text);
    for (int i = 0; i < r->uniform_count; ++i) free(r->uniforms[i].name);
    free(r->uniforms);
    for (int i = 0; i < r->block_uniform_count; ++i) free(r->block_uniforms[i].name);
    free(r->block_uniforms);
    for (int i = 0; i < r->block_count; ++i) free(r->blocks[i].name);
    free(r->blocks);
    for (int i = 0; i < r->attribute_count; ++i) free(r->attributes[i].name);
    free(r->attributes);
    for (int i = 0; i < r->output_count; ++i) free(r->outputs[i].name);
    free(r->outputs);
    memset(r, 0, sizeof *r);
}
