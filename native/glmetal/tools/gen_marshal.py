#!/usr/bin/env python3
"""Generates GLMetal's threaded entry points (see src/thread.h).

  gen_marshal.py SDK OUT_DIR apple-gl-exports.txt apple-cgl-exports.txt

Outputs:
  glm_impl_rename.h  force-included into the implementation: every exported
                     gl* name becomes glm_impl_<name>, so the implementation
                     (and its internal calls) sit behind the marshal layer.
  marshal.c          the exported gl* functions. Calls whose arguments are
                     values, or pointers to data of a size computable from the
                     arguments, are recorded into the context's command stream
                     when it has one; everything else drains the stream and
                     calls the implementation directly.
  stubs.c            weak glm_impl_* (and plain exports without a prototype)
                     for what GLMetal does not implement.
"""
import pathlib
import re
import sys

sdk, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
exports = []
for path in sys.argv[3:]:
    exports += [l.strip() for l in open(path) if l.strip()]
headers = sdk / 'System/Library/Frameworks/OpenGL.framework/Headers'

protos = {}
for h in ['gl.h', 'glext.h', 'gl3.h', 'gl3ext.h']:
    text = re.sub(r'/\*.*?\*/', '', (headers / h).read_text(errors='replace'), flags=re.S)
    for m in re.finditer(r'(?:extern|GLAPI)[ \t]+((?:const[ \t]+)?\w+(?:[ \t]*\*)*[ \t]*(?:APIENTRY[ \t]+)?)'
                         r'\b(gl[A-Z]\w*)\s*\(([^)]*)\)\s*(?:OPENGL_\w+(?:\([^)]*\))?\s*)*;', text):
        ret = ' '.join(m.group(1).replace('APIENTRY', '').split())
        protos.setdefault(m.group(2), (ret, ' '.join(m.group(3).split())))


def parse_params(text):
    if text.strip() in ('', 'void'):
        return []
    params = []
    for part in text.split(','):
        part = part.strip()
        if part.endswith('*'):  # unnamed pointer parameter
            params.append((part, None))
            continue
        m = re.match(r'^(.*?)(\w+)\s*(\[\s*\d*\s*\])?$', part)
        ptype, name, array = m.group(1).strip(), m.group(2), m.group(3)
        if not ptype:  # unnamed parameter
            ptype, name = part, None
        if array:
            ptype += ' *'
        params.append((ptype, name))
    return params


# Apple's dispatch table exposes symbols that are unavailable in a profile.
# Reject them before shadow updates, pointer copies or display-list recording.
profile_rejections = {}
for line in (pathlib.Path(__file__).resolve().parents[1] / 'data/apple-profile-rejections.txt').read_text().splitlines():
    if line.strip() and not line.startswith('#'):
        name, profile = line.split()
        profile_rejections[name] = profile


def profile_guard(name, ret):
    profile = profile_rejections.get(name)
    if profile is None:
        return ''
    value = '' if ret == 'void' else ' 0'
    return (f'    if (glm_profile_rejected(ctx, {int(profile == "core")})) return{value};\n')


# ---- classification --------------------------------------------------------

# Hand-written in marshal_custom.c (draws, queries answered from the shadow,
# stream control).
CUSTOM = {
    'glDrawArrays', 'glDrawArraysInstanced', 'glDrawArraysInstancedARB', 'glDrawElements', 'glDrawRangeElements',
    'glDrawRangeElementsEXT', 'glDrawElementsInstanced', 'glDrawElementsInstancedARB', 'glDrawElementsBaseVertex',
    'glDrawRangeElementsBaseVertex', 'glDrawElementsInstancedBaseVertex', 'glGetIntegerv', 'glFlush', 'glFinish',
    'glFenceSync', 'glClientWaitSync', 'glGetSynciv', 'glGetTexParameteriv', 'glGetTexParameterfv',
    'glGetTexLevelParameteriv', 'glGetTexLevelParameterfv', 'glGetBufferParameteriv', 'glGetBufferParameterivARB',
    'glBufferData', 'glBufferDataARB', 'glBufferSubData', 'glBufferSubDataARB',
    'glNewList', 'glEndList', 'glGenLists', 'glDeleteLists', 'glIsList', 'glCallList', 'glCallLists', 'glListBase',
    'glIsEnabled', 'glGetBooleanv', 'glSwapAPPLE', 'glElementPointerAPPLE', 'glDrawElementArrayAPPLE',
    'glDrawRangeElementArrayAPPLE', 'glMultiDrawElementArrayAPPLE', 'glMultiDrawRangeElementArrayAPPLE',
    'glCreateShader', 'glCreateProgram', 'glShaderSource', 'glCompileShader', 'glGetShaderiv', 'glGetShaderInfoLog',
    'glLinkProgram', 'glGetProgramiv', 'glGetProgramInfoLog', 'glGetAttribLocation', 'glGetUniformLocation',
    'glGetQueryObjectiv', 'glGetQueryObjectivARB', 'glGetQueryObjectuiv', 'glGetQueryObjectuivARB',
}
# Change texture objects (not bindings): texture queries answered on the
# calling thread wait for these (pending counter 1).
TEXOBJ = re.compile(r'^gl(TexParameter|GenerateMipmap|CopyTexImage|CopyTexSubImage|TexSubImage|TexImage|'
                    r'CompressedTex|TexStorage|TextureParameter)')
# Client-side shadow updates (marshal_custom.c): glm_shadow_<name>(ctx, args).
SHADOW = {
    'glBindBuffer', 'glBindBufferARB', 'glBindVertexArray', 'glBindVertexArrayAPPLE', 'glDeleteVertexArrays',
    'glDeleteVertexArraysAPPLE', 'glEnableVertexAttribArray', 'glEnableVertexAttribArrayARB', 'glDisableVertexAttribArray',
    'glDisableVertexAttribArrayARB', 'glEnableClientState', 'glDisableClientState', 'glClientActiveTexture',
    'glClientActiveTextureARB', 'glVertexAttribPointer', 'glVertexAttribPointerARB', 'glVertexAttribIPointer',
    'glVertexAttribIPointerEXT', 'glVertexPointer', 'glNormalPointer', 'glColorPointer', 'glSecondaryColorPointer',
    'glSecondaryColorPointerEXT', 'glFogCoordPointer', 'glFogCoordPointerEXT', 'glTexCoordPointer', 'glUseProgram',
    'glUseProgramObjectARB', 'glActiveTexture', 'glActiveTextureARB', 'glBindTexture', 'glBindTextureEXT',
    'glBindFramebuffer', 'glBindFramebufferEXT', 'glBindRenderbuffer', 'glBindRenderbufferEXT', 'glDeleteBuffers',
    'glDeleteBuffersARB', 'glViewport', 'glVertexAttribDivisor', 'glVertexAttribDivisorARB', 'glScissor',
    'glReadBuffer', 'glDrawBuffer', 'glPixelStorei', 'glPixelStoref', 'glBindSampler', 'glTexParameteri',
    'glTexParameterf', 'glTexParameteriv', 'glTexParameterfv', 'glPolygonMode', 'glDeleteTextures', 'glDeleteTexturesEXT',
    'glTexImage1D', 'glTexImage2D', 'glTexImage3D', 'glCompressedTexImage2D', 'glCompressedTexImage2DARB',
    'glCopyTexImage2D', 'glTexStorage1D', 'glTexStorage2D', 'glTexStorage3D', 'glGenerateMipmap', 'glGenerateMipmapEXT',
    'glDrawBuffers', 'glDrawBuffersARB', 'glEnable', 'glDisable', 'glEnablei', 'glDisablei', 'glColorMask', 'glColorMaski',
    'glViewportArrayv', 'glViewportIndexedf', 'glViewportIndexedfv', 'glScissorArrayv', 'glScissorIndexed',
    'glScissorIndexedv', 'glAttachShader', 'glDetachShader', 'glBindAttribLocation', 'glBindFragDataLocation',
    'glBindFragDataLocationEXT', 'glBindFragDataLocationIndexed', 'glTransformFeedbackVaryings', 'glDeleteShader',
    'glDeleteProgram', 'glBeginQuery', 'glBeginQueryARB', 'glEndQuery', 'glEndQueryARB',
}
# Change shadowed state in ways the hooks do not follow: run synchronously,
# then re-read the shadow (glm_shadow_refresh).
REFRESH = {
    'glPopAttrib', 'glPopClientAttrib', 'glInterleavedArrays', 'glDeleteTextures', 'glDeleteTexturesEXT',
    'glDeleteFramebuffers', 'glDeleteFramebuffersEXT', 'glDeleteRenderbuffers', 'glDeleteRenderbuffersEXT',
    'glDeleteProgram', 'glDeleteObjectARB',
}
# Calls that cannot change draw state beyond what the backend re-reads on
# every draw (uniforms, current attributes, matrices, buffer contents, the
# vertex layout it compares itself): they leave the state serial alone.
CHEAP = re.compile(
    r'^gl(Uniform|ProgramLocalParameter|ProgramEnvParameter|VertexAttrib[1-4I]|VertexAttribP|Color[34]|Normal3|'
    r'TexCoord[1-4]|MultiTexCoord|SecondaryColor3|FogCoord[fd]|Vertex[234]|Index[dfisub]|EdgeFlag$|EdgeFlagv|'
    r'LoadIdentity|LoadMatrix|LoadTransposeMatrix|MultMatrix|MultTransposeMatrix|Translate|Rotate|Scale|PushMatrix|'
    r'PopMatrix|Ortho|Frustum|MatrixMode|Material|Begin$|End$|Draw|MultiDraw|BufferSubData|BufferData|MapBuffer|'
    r'UnmapBuffer|FlushMappedBufferRange|Get|Is[A-Z]|Flush|Finish|BindBuffer|VertexAttribPointer|VertexAttribIPointer|'
    r'VertexPointer|NormalPointer|ColorPointer|TexCoordPointer|SecondaryColorPointer|FogCoordPointer|'
    r'EnableVertexAttribArray|DisableVertexAttribArray|EnableClientState|DisableClientState|ClientActiveTexture|'
    r'BindVertexArray|ArrayElement|Rect|Gen[A-Z]|PixelStore|BeginQuery|EndQuery|TestFence|FinishFence|SetFence|'
    r'ClientWaitSync|WaitSync|FenceSync|DeleteSync)')
# Queries of state only state-changing (serial-bumping) calls modify: when
# none of those are pending in the stream, they read the implementation
# directly instead of draining it. glGet{Boolean,Float,Double}v also check
# the pname is not cheap state (matrices, current attributes, arrays).
QUERY_DIRECT = {
    'glGetTexLevelParameteriv', 'glGetTexLevelParameterfv', 'glGetTexParameteriv', 'glGetTexParameterfv',
    'glGetTexParameterIiv', 'glGetTexParameterIuiv', 'glIsEnabled', 'glIsEnabledi', 'glGetBooleanv', 'glGetFloatv',
    'glGetDoublev', 'glGetFramebufferAttachmentParameteriv', 'glGetFramebufferAttachmentParameterivEXT',
    'glCheckFramebufferStatus', 'glCheckFramebufferStatusEXT', 'glGetRenderbufferParameteriv',
    'glGetRenderbufferParameterivEXT', 'glGetProgramiv', 'glGetShaderiv', 'glGetUniformLocation',
    'glGetUniformLocationARB', 'glGetAttribLocation', 'glGetAttribLocationARB', 'glGetProgramivARB',
    'glGetObjectParameterivARB', 'glIsTexture', 'glIsProgram', 'glIsShader', 'glIsFramebuffer', 'glIsRenderbuffer',
    'glIsFramebufferEXT', 'glIsRenderbufferEXT', 'glGetFragDataLocation', 'glGetFragDataLocationEXT',
    'glGetUniformBlockIndex', 'glGetActiveUniform', 'glGetActiveAttrib', 'glGetProgramInfoLog', 'glGetShaderInfoLog',
    'glGetString', 'glGetStringi', 'glGetActiveUniformARB', 'glGetActiveAttribARB',
}
PNAME_CHECKED = {'glGetBooleanv', 'glGetFloatv', 'glGetDoublev'}
# Executed immediately, never compiled into display lists (GL 2.1 5.4 and
# the object/buffer/client-state commands of later versions).
NOT_LISTED = re.compile(
    r'^gl(Gen|Delete|Is|Get|Are|Bind(Buffer|Framebuffer|Renderbuffer|VertexArray|TransformFeedback|ProgramPipeline)|'
    r'Buffer|Map|Unmap|FlushMapped|Flush$|Finish$|FeedbackBuffer|SelectBuffer|RenderMode|ReadPixels|PixelStore|'
    r'\w*Pointer|InterleavedArrays|EnableClientState|DisableClientState|EnableVertexAttribArray|'
    r'DisableVertexAttribArray|ClientActiveTexture|PushClientAttrib|PopClientAttrib|Fence|ClientWaitSync|WaitSync|'
    r'Create|ShaderSource|CompileShader|LinkProgram|AttachShader|DetachShader|ValidateProgram|Framebuffer|'
    r'Renderbuffer|CheckFramebuffer|NewList|EndList|ListBase|CallList|TransformFeedbackVaryings|BindAttribLocation|'
    r'BindFragDataLocation|UniformBlockBinding|VertexAttribDivisor|VertexAttribIPointer|ProgramParameter|'
    r'ObjectPurgeable|TextureRange|VertexArrayRange|FlushVertexArrayRange|UseProgramStages|ActiveShaderProgram|'
    r'ProgramString|BindProgram|Label|PushGroupMarker|PopGroupMarker|InsertEventMarker|PatchParameteri$)')
# Array pointers are only recorded, never read, by these calls.
POINTER_IS_VALUE = {
    'glVertexPointer', 'glNormalPointer', 'glColorPointer', 'glTexCoordPointer', 'glSecondaryColorPointer',
    'glSecondaryColorPointerEXT', 'glFogCoordPointer', 'glFogCoordPointerEXT', 'glIndexPointer', 'glEdgeFlagPointer',
    'glVertexAttribPointer', 'glVertexAttribPointerARB', 'glVertexAttribIPointer', 'glVertexAttribIPointerEXT',
    'glDrawArraysIndirect', 'glDrawElementsIndirect',
}
# Pointer parameters whose data these calls read, and its size in bytes.
TYPE_SIZE = {'b': 1, 'ub': 1, 's': 2, 'us': 2, 'i': 4, 'ui': 4, 'f': 4, 'd': 8}


def pointer_sizes(name, params):
    """{parameter index: C expression of the bytes read}, or None to sync."""
    pn = [p[1] for p in params]

    def at(pname):
        return pn.index(pname)

    def last():
        return len(params) - 1

    m = re.match(r'^glUniform([1234])(f|i|ui)v(ARB|EXT)?$', name)
    if m:
        return {last(): f'(size_t){pn[1]} * {m.group(1)} * 4'}
    m = re.match(r'^glUniformMatrix([234])(x([234]))?fv(ARB)?$', name)
    if m:
        cols = int(m.group(1))
        rows = int(m.group(3)) if m.group(3) else cols
        return {last(): f'(size_t){pn[1]} * {cols * rows} * 4'}
    m = re.match(r'^glVertexAttrib(I)?([1234])(N)?(b|ub|s|us|i|ui|f|d)v(ARB|EXT)?$', name)
    if m:
        return {last(): str(int(m.group(2)) * TYPE_SIZE[m.group(4)])}
    m = re.match(r'^gl(Color|Normal|Vertex|TexCoord|SecondaryColor|RasterPos|WindowPos|Index|FogCoord)([1234])?'
                 r'(b|ub|s|us|i|ui|f|d)v(EXT|ARB)?$', name)
    if m:
        n = int(m.group(2)) if m.group(2) else {'Normal': 3}.get(m.group(1), 1)
        return {last(): str(n * TYPE_SIZE[m.group(3)])}
    m = re.match(r'^glMultiTexCoord([1234])(s|i|f|d)v(ARB)?$', name)
    if m:
        return {last(): str(int(m.group(1)) * TYPE_SIZE[m.group(2)])}
    m = re.match(r'^gl(Load|Mult)(Transpose)?Matrix(f|d)(ARB)?$', name)
    if m:
        return {0: str(16 * TYPE_SIZE[m.group(3)])}
    if name == 'glClipPlane':
        return {1: '32'}
    m = re.match(r'^glRect(s|i|f|d)v$', name)
    if m:
        return {0: str(2 * TYPE_SIZE[m.group(1)]), 1: str(2 * TYPE_SIZE[m.group(1)])}
    m = re.match(r'^gl(Material|Light|LightModel|Fog|TexEnv|TexGen|TexParameter|SamplerParameter|PointParameter)'
                 r'(I)?(f|i|ui|d)v(ARB|EXT)?$', name)
    if m:
        return {last(): f'glm_param_count({pn[-2]}) * {TYPE_SIZE[m.group(3)]}'}
    m = re.match(r'^glProgram(Env|Local)Parameter4(f|d)vARB$', name)
    if m:
        return {last(): str(4 * TYPE_SIZE[m.group(2)])}
    if re.match(r'^glProgram(Env|Local)Parameters4fvEXT$', name):
        return {last(): f'(size_t){pn[2]} * 16'}
    if re.match(r'^glDelete(Textures|Buffers|Framebuffers|Renderbuffers|VertexArrays|Queries|Samplers|Fences|Programs|'
                r'TransformFeedbacks)(ARB|EXT|APPLE)?$', name):
        return {1: f'(size_t){pn[0]} * 4'}
    if re.match(r'^glDrawBuffers(ARB|ATI)?$', name):
        return {1: f'(size_t){pn[0]} * 4'}
    m = re.match(r'^glClearBuffer(f|i|ui)v$', name)
    if m:
        return {2: f'({pn[0]} == 0x1800 /* GL_COLOR */ ? 16 : 4)'}
    if re.match(r'^gl(BindAttribLocation|BindFragDataLocation)(ARB|EXT)?$', name):
        return {2: f'strlen((const char *){pn[2]}) + 1'}
    image = {'glTexImage1D': (3, None, None, 5, 6), 'glTexImage2D': (3, 4, None, 6, 7), 'glTexImage3D': (3, 4, 5, 7, 8),
             'glTexSubImage1D': (3, None, None, 4, 5), 'glTexSubImage2D': (4, 5, None, 6, 7),
             'glTexSubImage3D': (5, 6, 7, 8, 9), 'glTexImage3DEXT': (3, 4, 5, 7, 8), 'glTexSubImage3DEXT': (5, 6, 7, 8, 9)}
    if name in image:
        w, h, d, f, t = image[name]
        dims = ', '.join(pn[i] if i is not None else '1' for i in (w, h, d))
        return {last(): f'glm_marshal_image_size(ctx, {dims}, {pn[f]}, {pn[t]})'}
    if re.match(r'^glCompressedTex(Sub)?Image[123]D(ARB)?$', name):
        return {last(): f'(size_t){pn[-2]}'}
    if name in ('glViewportIndexedfv', 'glScissorIndexedv', 'glDepthRangeIndexedfv'):
        return {1: '16'}
    if name in ('glViewportArrayv', 'glScissorArrayv'):
        return {2: f'(size_t){pn[1]} * 16'}
    if name == 'glDepthRangeArrayv':
        return {2: f'(size_t){pn[1]} * 16'}
    if re.match(r'^glBufferSubData(ARB)?$', name):
        return {3: f'(size_t){pn[2]}'}
    if re.match(r'^glBufferData(ARB)?$', name):
        return {2: f'(size_t){pn[1]}'}
    return None


def is_pointer(ptype):
    return '*' in ptype


decls = []      # prototypes of glm_impl_* for marshal.c
bodies = []
renamed = []
stats = {'async': 0, 'sync': 0, 'custom': 0}
for name in sorted(set(e for e in exports if e.startswith('gl'))):
    if name not in protos:
        continue
    renamed.append(name)
    ret, ptext = protos[name]
    params = parse_params(ptext)
    names = [f'p{i}' for i in range(len(params))]
    sig = ', '.join(f'{t} {n}' for (t, _), n in zip(params, names)) or 'void'
    call = ', '.join(names)
    decls.append(f'extern {ret} glm_impl_{name}({sig});')
    if name in SHADOW:
        decls.append(f'void glm_shadow_{name}(struct glm_context *ctx{"".join(", " + t + " " + n for (t, _), n in zip(params, names))});')
    if name in CUSTOM:
        stats['custom'] += 1
        continue
    guard = profile_guard(name, ret)
    shadow = f'    if (ctx) glm_shadow_{name}(ctx{"".join(", " + n for n in names)});\n' if name in SHADOW else ''
    # The state serial moves when the call executes (worker or caller), never
    # when it is recorded: a draw executing earlier must not see it.
    bump = not CHEAP.match(name)
    pointers = [i for i, (t, _) in enumerate(params) if is_pointer(t)]
    sizes = {} if not pointers else pointer_sizes(name, [(t, n) for (t, _), n in zip(params, names)])
    if name in POINTER_IS_VALUE:
        sizes, pointers = {}, []
    can_async = name not in REFRESH and ret == 'void' and (not pointers or (sizes is not None and set(sizes) == set(pointers) and
                                                    all(params[i][0].startswith('const') for i in pointers)))
    if not can_async:
        stats['sync'] += 1
        ret_kw = '' if ret == 'void' else 'return '
        if name in REFRESH:
            bodies.append(f'GLM_EXPORT void {name}({sig})\n{{\n    struct glm_context *ctx = glm_current();\n{guard}{shadow}'
                          f'    if (ctx && GLM_STREAM(ctx)) glm_thread_sync_named(ctx, "{name}");\n'
                          f'    if (ctx) GLM_STATE_CHANGED(ctx);\n    glm_impl_{name}({call});\n'
                          f'    if (ctx) glm_shadow_refresh(ctx);\n}}\n')
            continue
        bump_line = '    if (ctx) GLM_STATE_CHANGED(ctx);\n' if bump else ''
        if ret == 'void' and not NOT_LISTED.match(name):
            # Listable, but its data size is only known to the implementation:
            # executed at compile time instead (glm_list_unsupported logs it).
            bump_line = f'    if (ctx && GLM_LISTING(ctx)) glm_list_unsupported(ctx, "{name}");\n' + bump_line
        settled = ''
        if name in QUERY_DIRECT:
            cond = 'glm_thread_state_settled(ctx)'
            if name in PNAME_CHECKED:
                cond += ' && !glm_cheap_pname(p0)'
            settled = f'!({cond}) && '
        bodies.append(f'GLM_EXPORT {ret} {name}({sig})\n{{\n    struct glm_context *ctx = glm_current();\n{guard}{shadow}'
                      f'    if (ctx && GLM_STREAM(ctx) && {settled}1) glm_thread_sync_named(ctx, "{name}");\n{bump_line}    {ret_kw}glm_impl_{name}({call});\n}}\n')
        continue
    stats['async'] += 1
    fields = ''.join(f'    {params[i][0].replace("const", "").strip() if i in sizes else params[i][0]} {names[i]};\n'
                     for i in range(len(params)))
    if sizes:
        fields += '    void *heap;\n'
    if not fields:
        fields = '    char unused;\n'
    struct = f'struct glm_cmd_{name} {{\n{fields}}};\n'
    exec_bump = '    GLM_STATE_CHANGED(glm_current());\n' if bump else ''
    exec_done = '    glm_thread_state_executed(glm_current());\n' if bump else ''
    if TEXOBJ.match(name):
        exec_done += '    glm_thread_pending_add(glm_current(), 1, -1);\n'
    exec_free = '    free(c->heap);\n' if sizes else ''
    exec_fn = (f'static void glm_exec_{name}(const void *payload)\n{{\n'
               f'    const struct glm_cmd_{name} *c = payload;\n'
               f'    (void)c;\n{exec_bump}    glm_impl_{name}({", ".join("c->" + n for n in names)});\n{exec_free}{exec_done}}}\n')
    listed = not NOT_LISTED.match(name)
    list_branch = ''
    if listed:
        # Display lists replay the same payload without stream accounting.
        exec_fn += (f'static void glm_list_exec_{name}(const void *payload)\n{{\n'
                    f'    const struct glm_cmd_{name} *c = payload;\n'
                    f'    (void)c;\n{exec_bump}    glm_impl_{name}({", ".join("c->" + n for n in names)});\n}}\n')
        fill = ''
        if sizes:
            fill += ''.join(f'        size_t n{i} = {names[i]} ? (size_t)({expr}) : 0;\n' for i, expr in sizes.items())
            total = ' + '.join(f'((n{i} + 7) & ~(size_t)7)' for i in sizes)
            fill += (f'        struct glm_cmd_{name} *c = glm_list_alloc(ctx, sizeof *c + {total}, glm_list_exec_{name});\n'
                     f'        uint8_t *data = (uint8_t *)(c + 1);\n        (void)data;\n        c->heap = NULL;\n')
        else:
            fill += f'        struct glm_cmd_{name} *c = glm_list_alloc(ctx, sizeof *c, glm_list_exec_{name});\n'
        for i, n in enumerate(names):
            if i in sizes:
                ftype = params[i][0].replace('const', '').strip()
                fill += (f'        if (n{i}) {{\n            memcpy(data, {n}, n{i});\n            c->{n} = ({ftype})data;\n'
                         f'            data += (n{i} + 7) & ~(size_t)7;\n        }} else {{\n            c->{n} = ({ftype}){n};\n        }}\n')
            else:
                fill += f'        c->{n} = {n};\n'
        shadow_note = '        glm_list_note_shadowed(ctx);\n' if name in SHADOW else ''
        if sizes:
            # Data of unknown size cannot be recorded: it runs now instead.
            # (Negative counts make huge sizes: the call reports the error.)
            unknown = ' || '.join(f'(size_t)({expr}) == (size_t)-1 || (size_t)({expr}) > ((size_t)256 << 20)'
                                  for i, expr in sizes.items())
            fill = (f'        if ({unknown}) {{\n            glm_list_unsupported(ctx, "{name}");\n'
                    f'        }} else {{\n' + fill.replace('\n        ', '\n            ').replace('        ', '            ', 1) +
                    '        }\n')
        list_branch = (f'    if (ctx && GLM_LISTING(ctx)) {{\n{fill}{shadow_note}'
                       f'        if (!glm_list_executes(ctx)) return;\n    }}\n')
    body = f'GLM_EXPORT void {name}({sig})\n{{\n    struct glm_context *ctx = glm_current();\n{guard}{list_branch}{shadow}'
    direct_bump = '        if (ctx) GLM_STATE_CHANGED(ctx);\n' if bump else ''
    body += f'    if (!ctx || !GLM_STREAM(ctx)) {{\n{direct_bump}        glm_impl_{name}({call});\n        return;\n    }}\n'
    record_pending = '    glm_thread_state_recorded(ctx);\n' if bump else ''
    if TEXOBJ.match(name):
        record_pending += '    glm_thread_pending_add(ctx, 1, 1);\n'
    if sizes:
        body += ''.join(f'    size_t n{i} = {names[i]} ? (size_t)({expr}) : 0;\n' for i, expr in sizes.items())
        total = ' + '.join(f'((n{i} + 7) & ~(size_t)7)' for i in sizes)
        body += f'    size_t extra = {total};\n'
        unknown = ' || '.join(f'n{i} == (size_t)-1 || n{i} > ((size_t)256 << 20)' for i in sizes)
        body += (f'    if ({unknown} || extra > ((size_t)256 << 20)) {{\n        glm_thread_sync_named(ctx, "{name}");\n{direct_bump}'
                 f'        glm_impl_{name}({call});\n        return;\n    }}\n')
        # Large data goes in a heap copy the command frees once it ran.
        body += (f'{record_pending}    bool heap = extra > GLM_MARSHAL_MAX;\n'
                 f'    struct glm_cmd_{name} *c = glm_thread_alloc(ctx, sizeof *c + (heap ? 0 : extra), glm_exec_{name});\n'
                 f'    uint8_t *data = heap ? malloc(extra) : (uint8_t *)(c + 1);\n'
                 f'    c->heap = heap ? data : NULL;\n')
    else:
        body += f'{record_pending}    struct glm_cmd_{name} *c = glm_thread_alloc(ctx, sizeof *c, glm_exec_{name});\n'
    for i, n in enumerate(names):
        if i in sizes:
            ftype = params[i][0].replace('const', '').strip()
            body += (f'    if (n{i}) {{\n        memcpy(data, {n}, n{i});\n        c->{n} = ({ftype})data;\n'
                     f'        data += (n{i} + 7) & ~(size_t)7;\n    }} else {{\n        c->{n} = ({ftype}){n};\n    }}\n')
        else:
            body += f'    c->{n} = {n};\n'
    body += '}\n'
    bodies.append(struct + exec_fn + body)

out.mkdir(parents=True, exist_ok=True)
(out / 'glm_impl_rename.h').write_text(
    '/* Generated by tools/gen_marshal.py: the implementation behind the marshal layer. */\n'
    '#ifndef GLM_IMPL_RENAME_H\n#define GLM_IMPL_RENAME_H\n' +
    ''.join(f'#define {n} glm_impl_{n}\n' for n in renamed) + '#endif\n')
(out / 'marshal.c').write_text(
    '/* Generated by tools/gen_marshal.py. */\n#define GL_SILENCE_DEPRECATION 1\n'
    '#include <OpenGL/gltypes.h>\n#include <stdbool.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n'
    '#include "marshal_support.h"\n\n' + '\n'.join(decls) + '\n\n' + '\n'.join(bodies))
stub = ['/* Generated by tools/gen_marshal.py: weak fallbacks for what GLMetal does not implement. */',
        '#include <stdint.h>', 'void glm_unimplemented(const char *name);']
for n in sorted(set(exports)):
    target = f'glm_impl_{n}' if n in renamed else n
    stub.append(f'__attribute__((weak, visibility("default"))) uintptr_t {target}(void) '
                f'{{ glm_unimplemented("{n}"); return 0; }}')
(out / 'stubs.c').write_text('\n'.join(stub) + '\n')
print(f'gen_marshal: {stats["async"]} recorded, {stats["sync"]} synchronous, {stats["custom"]} custom', file=sys.stderr)
