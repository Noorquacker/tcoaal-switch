// WebGL 1 on top of OpenGL ES 2. Methods live on __native.gl.proto, which the
// JS side uses as the prototype of WebGLRenderingContext.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>
#include "rt.h"
#include "gl_consts.h"

static JSClassID globj_class, uniloc_class;

typedef struct {
    GLuint id;
    int kind;  // 1 buffer 2 texture 3 program 4 shader 5 framebuffer 6 renderbuffer
} GLObj;

typedef struct {
    GLint loc;
} UniLoc;

static bool unpack_flip, unpack_premul;
static GLuint bound_fb;
static bool fb_dirty;

bool rt_gl_take_dirty(void) {
    bool d = fb_dirty;
    fb_dirty = false;
    return d;
}

static void globj_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    // GL objects are deleted explicitly by the game (WebGL semantics); just free the wrapper.
    free(JS_GetOpaque(val, globj_class));
}

static void uniloc_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    free(JS_GetOpaque(val, uniloc_class));
}

static JSValue new_obj(JSContext *ctx, GLuint id, int kind) {
    if (!id) return JS_NULL;
    GLObj *o = malloc(sizeof *o);
    o->id = id;
    o->kind = kind;
    JSValue v = JS_NewObjectClass(ctx, (int)globj_class);
    JS_SetOpaque(v, o);
    return v;
}

static GLuint obj_id(JSValueConst v) {
    GLObj *o = JS_GetOpaque(v, globj_class);
    return o ? o->id : 0;
}

static GLint loc_of(JSValueConst v) {
    UniLoc *u = JS_GetOpaque(v, uniloc_class);
    return u ? u->loc : -1;
}

#define A_I(n) ((GLint)rt_arg_i(ctx, argv[n]))
#define A_U(n) ((GLenum)rt_arg_i(ctx, argv[n]))
#define A_F(n) ((GLfloat)rt_arg_f(ctx, argv[n]))
#define A_B(n) ((GLboolean)JS_ToBool(ctx, argv[n]))
#define FN(name) static JSValue name(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
#define UNUSED (void)this; (void)argc; (void)ctx; (void)argv

static inline void mark_draw(void) {
    if (bound_fb == 0) fb_dirty = true;
}

// ---------------------------------------------------------------------------
// simple state calls

FN(gl_activeTexture) { UNUSED; glActiveTexture(A_U(0)); return JS_UNDEFINED; }
FN(gl_blendColor) { UNUSED; glBlendColor(A_F(0), A_F(1), A_F(2), A_F(3)); return JS_UNDEFINED; }
FN(gl_blendEquation) { UNUSED; glBlendEquation(A_U(0)); return JS_UNDEFINED; }
FN(gl_blendEquationSeparate) { UNUSED; glBlendEquationSeparate(A_U(0), A_U(1)); return JS_UNDEFINED; }
FN(gl_blendFunc) { UNUSED; glBlendFunc(A_U(0), A_U(1)); return JS_UNDEFINED; }
FN(gl_blendFuncSeparate) { UNUSED; glBlendFuncSeparate(A_U(0), A_U(1), A_U(2), A_U(3)); return JS_UNDEFINED; }
FN(gl_clearColor) { UNUSED; glClearColor(A_F(0), A_F(1), A_F(2), A_F(3)); return JS_UNDEFINED; }
FN(gl_clearDepth) { UNUSED; glClearDepthf(A_F(0)); return JS_UNDEFINED; }
FN(gl_clearStencil) { UNUSED; glClearStencil(A_I(0)); return JS_UNDEFINED; }
FN(gl_colorMask) { UNUSED; glColorMask(A_B(0), A_B(1), A_B(2), A_B(3)); return JS_UNDEFINED; }
FN(gl_cullFace) { UNUSED; glCullFace(A_U(0)); return JS_UNDEFINED; }
FN(gl_depthFunc) { UNUSED; glDepthFunc(A_U(0)); return JS_UNDEFINED; }
FN(gl_depthMask) { UNUSED; glDepthMask(A_B(0)); return JS_UNDEFINED; }
FN(gl_depthRange) { UNUSED; glDepthRangef(A_F(0), A_F(1)); return JS_UNDEFINED; }
FN(gl_disable) { UNUSED; glDisable(A_U(0)); return JS_UNDEFINED; }
FN(gl_enable) { UNUSED; glEnable(A_U(0)); return JS_UNDEFINED; }
FN(gl_isEnabled) { UNUSED; return JS_NewBool(ctx, glIsEnabled(A_U(0))); }
FN(gl_disableVertexAttribArray) { UNUSED; glDisableVertexAttribArray((GLuint)A_I(0)); return JS_UNDEFINED; }
FN(gl_enableVertexAttribArray) { UNUSED; glEnableVertexAttribArray((GLuint)A_I(0)); return JS_UNDEFINED; }
FN(gl_finish) { UNUSED; glFinish(); return JS_UNDEFINED; }
FN(gl_flush) { UNUSED; glFlush(); return JS_UNDEFINED; }
FN(gl_frontFace) { UNUSED; glFrontFace(A_U(0)); return JS_UNDEFINED; }
FN(gl_generateMipmap) { UNUSED; glGenerateMipmap(A_U(0)); return JS_UNDEFINED; }
FN(gl_getError) { UNUSED; return JS_NewInt32(ctx, (int32_t)glGetError()); }
FN(gl_hint) { UNUSED; glHint(A_U(0), A_U(1)); return JS_UNDEFINED; }
FN(gl_lineWidth) { UNUSED; glLineWidth(A_F(0)); return JS_UNDEFINED; }
FN(gl_polygonOffset) { UNUSED; glPolygonOffset(A_F(0), A_F(1)); return JS_UNDEFINED; }
FN(gl_sampleCoverage) { UNUSED; glSampleCoverage(A_F(0), A_B(1)); return JS_UNDEFINED; }
FN(gl_scissor) { UNUSED; glScissor(A_I(0), A_I(1), A_I(2), A_I(3)); return JS_UNDEFINED; }
FN(gl_stencilFunc) { UNUSED; glStencilFunc(A_U(0), A_I(1), (GLuint)A_I(2)); return JS_UNDEFINED; }
FN(gl_stencilFuncSeparate) { UNUSED; glStencilFuncSeparate(A_U(0), A_U(1), A_I(2), (GLuint)A_I(3)); return JS_UNDEFINED; }
FN(gl_stencilMask) { UNUSED; glStencilMask((GLuint)A_I(0)); return JS_UNDEFINED; }
FN(gl_stencilMaskSeparate) { UNUSED; glStencilMaskSeparate(A_U(0), (GLuint)A_I(1)); return JS_UNDEFINED; }
FN(gl_stencilOp) { UNUSED; glStencilOp(A_U(0), A_U(1), A_U(2)); return JS_UNDEFINED; }
FN(gl_stencilOpSeparate) { UNUSED; glStencilOpSeparate(A_U(0), A_U(1), A_U(2), A_U(3)); return JS_UNDEFINED; }
FN(gl_viewport) { UNUSED; glViewport(A_I(0), A_I(1), A_I(2), A_I(3)); return JS_UNDEFINED; }
FN(gl_texParameteri) { UNUSED; glTexParameteri(A_U(0), A_U(1), A_I(2)); return JS_UNDEFINED; }
FN(gl_texParameterf) { UNUSED; glTexParameterf(A_U(0), A_U(1), A_F(2)); return JS_UNDEFINED; }
FN(gl_vertexAttribPointer) {
    UNUSED;
    glVertexAttribPointer((GLuint)A_I(0), A_I(1), A_U(2), A_B(3), A_I(4), (const void *)(intptr_t)rt_arg_f(ctx, argv[5]));
    return JS_UNDEFINED;
}
FN(gl_vertexAttrib1f) { UNUSED; glVertexAttrib1f((GLuint)A_I(0), A_F(1)); return JS_UNDEFINED; }
FN(gl_vertexAttrib2f) { UNUSED; glVertexAttrib2f((GLuint)A_I(0), A_F(1), A_F(2)); return JS_UNDEFINED; }
FN(gl_vertexAttrib3f) { UNUSED; glVertexAttrib3f((GLuint)A_I(0), A_F(1), A_F(2), A_F(3)); return JS_UNDEFINED; }
FN(gl_vertexAttrib4f) { UNUSED; glVertexAttrib4f((GLuint)A_I(0), A_F(1), A_F(2), A_F(3), A_F(4)); return JS_UNDEFINED; }

FN(gl_pixelStorei) {
    UNUSED;
    GLenum p = A_U(0);
    if (p == 0x9240) unpack_flip = JS_ToBool(ctx, argv[1]);
    else if (p == 0x9241) unpack_premul = JS_ToBool(ctx, argv[1]);
    else if (p == 0x9243) { /* colorspace conversion: ignored */ }
    else glPixelStorei(p, A_I(1));
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// drawing

FN(gl_clear) { UNUSED; glClear((GLbitfield)A_I(0)); mark_draw(); return JS_UNDEFINED; }
FN(gl_drawArrays) { UNUSED; glDrawArrays(A_U(0), A_I(1), A_I(2)); mark_draw(); return JS_UNDEFINED; }
FN(gl_drawElements) {
    UNUSED;
    glDrawElements(A_U(0), A_I(1), A_U(2), (const void *)(intptr_t)rt_arg_f(ctx, argv[3]));
    mark_draw();
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// objects

#define GEN(name, glgen, kind)                   \
    FN(name) {                                   \
        UNUSED;                                  \
        GLuint id = 0;                           \
        glgen(1, &id);                           \
        return new_obj(ctx, id, kind);           \
    }
GEN(gl_createBuffer, glGenBuffers, 1)
GEN(gl_createTexture, glGenTextures, 2)
GEN(gl_createFramebuffer, glGenFramebuffers, 5)
GEN(gl_createRenderbuffer, glGenRenderbuffers, 6)

FN(gl_createProgram) { UNUSED; return new_obj(ctx, glCreateProgram(), 3); }
FN(gl_createShader) { UNUSED; return new_obj(ctx, glCreateShader(A_U(0)), 4); }

#define DEL(name, gldel)                                    \
    FN(name) {                                              \
        UNUSED;                                             \
        GLObj *o = JS_GetOpaque(argv[0], globj_class);      \
        if (o && o->id) { gldel(1, &o->id); o->id = 0; }    \
        return JS_UNDEFINED;                                \
    }
DEL(gl_deleteBuffer, glDeleteBuffers)
DEL(gl_deleteTexture, glDeleteTextures)
DEL(gl_deleteFramebuffer, glDeleteFramebuffers)
DEL(gl_deleteRenderbuffer, glDeleteRenderbuffers)

FN(gl_deleteProgram) {
    UNUSED;
    GLObj *o = JS_GetOpaque(argv[0], globj_class);
    if (o && o->id) { glDeleteProgram(o->id); o->id = 0; }
    return JS_UNDEFINED;
}
FN(gl_deleteShader) {
    UNUSED;
    GLObj *o = JS_GetOpaque(argv[0], globj_class);
    if (o && o->id) { glDeleteShader(o->id); o->id = 0; }
    return JS_UNDEFINED;
}

FN(gl_isBuffer) { UNUSED; return JS_NewBool(ctx, obj_id(argv[0]) && glIsBuffer(obj_id(argv[0]))); }
FN(gl_isTexture) { UNUSED; return JS_NewBool(ctx, obj_id(argv[0]) && glIsTexture(obj_id(argv[0]))); }
FN(gl_isProgram) { UNUSED; return JS_NewBool(ctx, obj_id(argv[0]) && glIsProgram(obj_id(argv[0]))); }
FN(gl_isShader) { UNUSED; return JS_NewBool(ctx, obj_id(argv[0]) && glIsShader(obj_id(argv[0]))); }
FN(gl_isFramebuffer) { UNUSED; return JS_NewBool(ctx, obj_id(argv[0]) && glIsFramebuffer(obj_id(argv[0]))); }
FN(gl_isRenderbuffer) { UNUSED; return JS_NewBool(ctx, obj_id(argv[0]) && glIsRenderbuffer(obj_id(argv[0]))); }

FN(gl_bindBuffer) { UNUSED; glBindBuffer(A_U(0), obj_id(argv[1])); return JS_UNDEFINED; }
FN(gl_bindTexture) { UNUSED; glBindTexture(A_U(0), obj_id(argv[1])); return JS_UNDEFINED; }
FN(gl_bindRenderbuffer) { UNUSED; glBindRenderbuffer(A_U(0), obj_id(argv[1])); return JS_UNDEFINED; }
FN(gl_bindFramebuffer) {
    UNUSED;
    bound_fb = obj_id(argv[1]);
    glBindFramebuffer(A_U(0), bound_fb);
    return JS_UNDEFINED;
}

FN(gl_framebufferTexture2D) {
    UNUSED;
    glFramebufferTexture2D(A_U(0), A_U(1), A_U(2), obj_id(argv[3]), A_I(4));
    return JS_UNDEFINED;
}
FN(gl_framebufferRenderbuffer) {
    UNUSED;
    GLenum att = A_U(1);
    GLuint rb = obj_id(argv[3]);
    if (att == 0x821A) {  // DEPTH_STENCIL_ATTACHMENT: GLES2 needs both
        glFramebufferRenderbuffer(A_U(0), GL_DEPTH_ATTACHMENT, A_U(2), rb);
        glFramebufferRenderbuffer(A_U(0), GL_STENCIL_ATTACHMENT, A_U(2), rb);
    } else {
        glFramebufferRenderbuffer(A_U(0), att, A_U(2), rb);
    }
    return JS_UNDEFINED;
}
FN(gl_renderbufferStorage) {
    UNUSED;
    GLenum fmt = A_U(1);
    if (fmt == 0x84F9) fmt = 0x88F0;  // DEPTH_STENCIL -> DEPTH24_STENCIL8_OES
    glRenderbufferStorage(A_U(0), fmt, A_I(2), A_I(3));
    return JS_UNDEFINED;
}
FN(gl_checkFramebufferStatus) { UNUSED; return JS_NewInt32(ctx, (int32_t)glCheckFramebufferStatus(A_U(0))); }

// ---------------------------------------------------------------------------
// buffers

FN(gl_bufferData) {
    UNUSED;
    if (JS_IsNumber(argv[1])) {
        glBufferData(A_U(0), (GLsizeiptr)rt_arg_f(ctx, argv[1]), NULL, A_U(2));
    } else {
        size_t len = 0;
        uint8_t *p = rt_get_bytes(ctx, argv[1], &len);
        glBufferData(A_U(0), (GLsizeiptr)len, p, A_U(2));
    }
    return JS_UNDEFINED;
}
FN(gl_bufferSubData) {
    UNUSED;
    size_t len = 0;
    uint8_t *p = rt_get_bytes(ctx, argv[2], &len);
    if (p) glBufferSubData(A_U(0), (GLintptr)rt_arg_f(ctx, argv[1]), (GLsizeiptr)len, p);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// shaders / programs

FN(gl_shaderSource) {
    UNUSED;
    size_t len;
    const char *src = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!src) return JS_EXCEPTION;
    GLint l = (GLint)len;
    glShaderSource(obj_id(argv[0]), 1, &src, &l);
    JS_FreeCString(ctx, src);
    return JS_UNDEFINED;
}
FN(gl_compileShader) { UNUSED; glCompileShader(obj_id(argv[0])); return JS_UNDEFINED; }
FN(gl_attachShader) { UNUSED; glAttachShader(obj_id(argv[0]), obj_id(argv[1])); return JS_UNDEFINED; }
FN(gl_detachShader) { UNUSED; glDetachShader(obj_id(argv[0]), obj_id(argv[1])); return JS_UNDEFINED; }
FN(gl_linkProgram) { UNUSED; glLinkProgram(obj_id(argv[0])); return JS_UNDEFINED; }
FN(gl_useProgram) { UNUSED; glUseProgram(obj_id(argv[0])); return JS_UNDEFINED; }
FN(gl_validateProgram) { UNUSED; glValidateProgram(obj_id(argv[0])); return JS_UNDEFINED; }

FN(gl_bindAttribLocation) {
    UNUSED;
    const char *n = JS_ToCString(ctx, argv[2]);
    glBindAttribLocation(obj_id(argv[0]), (GLuint)A_I(1), n);
    JS_FreeCString(ctx, n);
    return JS_UNDEFINED;
}

static JSValue param_value(JSContext *ctx, GLenum pname, GLint v) {
    switch (pname) {
    case GL_DELETE_STATUS: case GL_COMPILE_STATUS: case GL_LINK_STATUS: case GL_VALIDATE_STATUS:
        return JS_NewBool(ctx, v);
    default:
        return JS_NewInt32(ctx, v);
    }
}

FN(gl_getShaderParameter) {
    UNUSED;
    GLint v = 0;
    glGetShaderiv(obj_id(argv[0]), A_U(1), &v);
    return param_value(ctx, A_U(1), v);
}
FN(gl_getProgramParameter) {
    UNUSED;
    GLint v = 0;
    glGetProgramiv(obj_id(argv[0]), A_U(1), &v);
    return param_value(ctx, A_U(1), v);
}

FN(gl_getShaderInfoLog) {
    UNUSED;
    char buf[4096] = "";
    glGetShaderInfoLog(obj_id(argv[0]), sizeof buf, NULL, buf);
    return JS_NewString(ctx, buf);
}
FN(gl_getProgramInfoLog) {
    UNUSED;
    char buf[4096] = "";
    glGetProgramInfoLog(obj_id(argv[0]), sizeof buf, NULL, buf);
    return JS_NewString(ctx, buf);
}
FN(gl_getShaderSource) {
    UNUSED;
    char *buf = malloc(65536);
    buf[0] = 0;
    glGetShaderSource(obj_id(argv[0]), 65536, NULL, buf);
    JSValue s = JS_NewString(ctx, buf);
    free(buf);
    return s;
}

static JSValue active_info(JSContext *ctx, bool uniform, GLuint prog, GLuint idx) {
    char name[256];
    GLint size = 0;
    GLenum type = 0;
    GLsizei len = 0;
    if (uniform) glGetActiveUniform(prog, idx, sizeof name, &len, &size, &type, name);
    else glGetActiveAttrib(prog, idx, sizeof name, &len, &size, &type, name);
    if (len <= 0) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewStringLen(ctx, name, (size_t)len));
    JS_SetPropertyStr(ctx, o, "size", JS_NewInt32(ctx, size));
    JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, (int32_t)type));
    return o;
}
FN(gl_getActiveUniform) { UNUSED; return active_info(ctx, true, obj_id(argv[0]), (GLuint)A_I(1)); }
FN(gl_getActiveAttrib) { UNUSED; return active_info(ctx, false, obj_id(argv[0]), (GLuint)A_I(1)); }

FN(gl_getAttribLocation) {
    UNUSED;
    const char *n = JS_ToCString(ctx, argv[1]);
    GLint l = glGetAttribLocation(obj_id(argv[0]), n);
    JS_FreeCString(ctx, n);
    return JS_NewInt32(ctx, l);
}
FN(gl_getUniformLocation) {
    UNUSED;
    const char *n = JS_ToCString(ctx, argv[1]);
    GLint l = glGetUniformLocation(obj_id(argv[0]), n);
    JS_FreeCString(ctx, n);
    if (l < 0) return JS_NULL;
    UniLoc *u = malloc(sizeof *u);
    u->loc = l;
    JSValue v = JS_NewObjectClass(ctx, (int)uniloc_class);
    JS_SetOpaque(v, u);
    return v;
}

FN(gl_getShaderPrecisionFormat) {
    UNUSED;
    GLint range[2] = {127, 127}, prec = 23;
    glGetShaderPrecisionFormat(A_U(0), A_U(1), range, &prec);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "rangeMin", JS_NewInt32(ctx, range[0]));
    JS_SetPropertyStr(ctx, o, "rangeMax", JS_NewInt32(ctx, range[1]));
    JS_SetPropertyStr(ctx, o, "precision", JS_NewInt32(ctx, prec));
    return o;
}

// ---------------------------------------------------------------------------
// uniforms

#define UNI_F(name, call)                                          \
    FN(name) { UNUSED; GLint l = loc_of(argv[0]); call; return JS_UNDEFINED; }
UNI_F(gl_uniform1f, glUniform1f(l, A_F(1)))
UNI_F(gl_uniform2f, glUniform2f(l, A_F(1), A_F(2)))
UNI_F(gl_uniform3f, glUniform3f(l, A_F(1), A_F(2), A_F(3)))
UNI_F(gl_uniform4f, glUniform4f(l, A_F(1), A_F(2), A_F(3), A_F(4)))
UNI_F(gl_uniform1i, glUniform1i(l, A_I(1)))
UNI_F(gl_uniform2i, glUniform2i(l, A_I(1), A_I(2)))
UNI_F(gl_uniform3i, glUniform3i(l, A_I(1), A_I(2), A_I(3)))
UNI_F(gl_uniform4i, glUniform4i(l, A_I(1), A_I(2), A_I(3), A_I(4)))

// Typed array (fast path) or plain JS array -> temporary C array.
static void *array_arg(JSContext *ctx, JSValueConst v, bool is_float, size_t *count, void *tmp, size_t tmpcap) {
    int t = JS_GetTypedArrayType(v);
    if ((is_float && t == JS_TYPED_ARRAY_FLOAT32) || (!is_float && t == JS_TYPED_ARRAY_INT32)) {
        size_t len;
        void *p = rt_get_bytes(ctx, v, &len);
        *count = len / 4;
        return p;
    }
    int64_t n = 0;
    JS_GetLength(ctx, v, &n);
    if ((size_t)n > tmpcap) n = (int64_t)tmpcap;
    for (int64_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyInt64(ctx, v, i);
        if (is_float) ((float *)tmp)[i] = (float)rt_arg_f(ctx, e);
        else ((int32_t *)tmp)[i] = rt_arg_i(ctx, e);
        JS_FreeValue(ctx, e);
    }
    *count = (size_t)n;
    return tmp;
}

#define UNI_V(name, isf, comps, call)                                         \
    FN(name) {                                                                \
        UNUSED;                                                               \
        static uint32_t tmp[4096];                                            \
        size_t n;                                                             \
        void *p = array_arg(ctx, argv[1], isf, &n, tmp, 4096);                \
        GLint l = loc_of(argv[0]);                                            \
        if (p && n >= comps) call(l, (GLsizei)(n / comps), p);                \
        return JS_UNDEFINED;                                                  \
    }
UNI_V(gl_uniform1fv, true, 1, glUniform1fv)
UNI_V(gl_uniform2fv, true, 2, glUniform2fv)
UNI_V(gl_uniform3fv, true, 3, glUniform3fv)
UNI_V(gl_uniform4fv, true, 4, glUniform4fv)
UNI_V(gl_uniform1iv, false, 1, glUniform1iv)
UNI_V(gl_uniform2iv, false, 2, glUniform2iv)
UNI_V(gl_uniform3iv, false, 3, glUniform3iv)
UNI_V(gl_uniform4iv, false, 4, glUniform4iv)

#define UNI_M(name, comps, call)                                              \
    FN(name) {                                                                \
        UNUSED;                                                               \
        static float tmp[4096];                                               \
        size_t n;                                                             \
        void *p = array_arg(ctx, argv[2], true, &n, tmp, 4096);               \
        if (p && n >= comps) call(loc_of(argv[0]), (GLsizei)(n / comps), GL_FALSE, p); \
        return JS_UNDEFINED;                                                  \
    }
UNI_M(gl_uniformMatrix2fv, 4, glUniformMatrix2fv)
UNI_M(gl_uniformMatrix3fv, 9, glUniformMatrix3fv)
UNI_M(gl_uniformMatrix4fv, 16, glUniformMatrix4fv)

// ---------------------------------------------------------------------------
// textures

static uint8_t *scratch;
static size_t scratch_cap;

static uint8_t *scratch_get(size_t n) {
    if (n > scratch_cap) {
        free(scratch);
        scratch_cap = n + n / 4;
        scratch = malloc(scratch_cap);
    }
    return scratch;
}

// Converts a plutovg surface (premultiplied BGRA) into RGBA honouring the
// WebGL unpack flags. Returns a pointer into the scratch buffer.
static const uint8_t *surface_to_rgba(const uint8_t *data, int w, int h, int stride) {
    uint8_t *out = scratch_get((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++) {
        const uint32_t *row = (const uint32_t *)(data + (size_t)(unpack_flip ? h - 1 - y : y) * (size_t)stride);
        uint8_t *o = out + (size_t)y * (size_t)w * 4;
        if (unpack_premul) {
            for (int x = 0; x < w; x++) {
                uint32_t p = row[x];
                o[0] = (uint8_t)(p >> 16);
                o[1] = (uint8_t)(p >> 8);
                o[2] = (uint8_t)p;
                o[3] = (uint8_t)(p >> 24);
                o += 4;
            }
        } else {
            for (int x = 0; x < w; x++) {
                uint32_t p = row[x], a = p >> 24;
                if (a == 255 || a == 0) {
                    o[0] = (uint8_t)(p >> 16);
                    o[1] = (uint8_t)(p >> 8);
                    o[2] = (uint8_t)p;
                } else {
                    o[0] = (uint8_t)((((p >> 16) & 255) * 255 + a / 2) / a);
                    o[1] = (uint8_t)((((p >> 8) & 255) * 255 + a / 2) / a);
                    o[2] = (uint8_t)(((p & 255) * 255 + a / 2) / a);
                }
                o[3] = (uint8_t)a;
                o += 4;
            }
        }
    }
    return out;
}

// Raw byte uploads (ArrayBufferView / ImageData): apply flip/premultiply for RGBA8.
static const uint8_t *bytes_for_upload(const uint8_t *p, int w, int h, GLenum fmt, GLenum type) {
    if (!p || (!unpack_flip && !unpack_premul) || type != GL_UNSIGNED_BYTE) return p;
    int bpp = fmt == GL_RGBA ? 4 : fmt == GL_RGB ? 3 : fmt == GL_LUMINANCE_ALPHA ? 2 : 1;
    size_t row = (size_t)w * (size_t)bpp;
    uint8_t *out = scratch_get(row * (size_t)h);
    for (int y = 0; y < h; y++) memcpy(out + (size_t)y * row, p + (size_t)(unpack_flip ? h - 1 - y : y) * row, row);
    if (unpack_premul && fmt == GL_RGBA) {
        for (size_t i = 0; i < row * (size_t)h; i += 4) {
            uint32_t a = out[i + 3];
            out[i] = (uint8_t)((out[i] * a + 127) / 255);
            out[i + 1] = (uint8_t)((out[i + 1] * a + 127) / 255);
            out[i + 2] = (uint8_t)((out[i + 2] * a + 127) / 255);
        }
    }
    return out;
}

// texImage2D(target, level, ifmt, w, h, border, fmt, type, bytes|null)
FN(gl_texImage2D) {
    UNUSED;
    int w = A_I(3), h = A_I(4);
    GLenum fmt = A_U(6), type = A_U(7);
    const uint8_t *p = NULL;
    if (argc > 8 && !JS_IsNull(argv[8]) && !JS_IsUndefined(argv[8])) {
        size_t len;
        p = bytes_for_upload(rt_get_bytes(ctx, argv[8], &len), w, h, fmt, type);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(A_U(0), A_I(1), A_I(2), w, h, A_I(5), fmt, type, p);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return JS_UNDEFINED;
}

// texSubImage2D(target, level, x, y, w, h, fmt, type, bytes)
FN(gl_texSubImage2D) {
    UNUSED;
    int w = A_I(4), h = A_I(5);
    size_t len;
    const uint8_t *p = bytes_for_upload(rt_get_bytes(ctx, argv[8], &len), w, h, A_U(6), A_U(7));
    if (!p) return JS_UNDEFINED;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(A_U(0), A_I(1), A_I(2), A_I(3), w, h, A_U(6), A_U(7), p);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return JS_UNDEFINED;
}

// texImageSurface(target, level, ifmt, fmt, type, surface)
FN(gl_texImageSurface) {
    UNUSED;
    const uint8_t *data;
    int w, h, stride;
    if (!rt_canvas_pixels(ctx, argv[5], &data, &w, &h, &stride)) return JS_ThrowTypeError(ctx, "texImage2D: bad source");
    const uint8_t *px = (data && w > 0 && h > 0) ? surface_to_rgba(data, w, h, stride) : NULL;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(A_U(0), A_I(1), GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return JS_UNDEFINED;
}

// texSubImageSurface(target, level, x, y, fmt, type, surface)
FN(gl_texSubImageSurface) {
    UNUSED;
    const uint8_t *data;
    int w, h, stride;
    if (!rt_canvas_pixels(ctx, argv[6], &data, &w, &h, &stride)) return JS_ThrowTypeError(ctx, "texSubImage2D: bad source");
    if (!data || w <= 0 || h <= 0) return JS_UNDEFINED;
    const uint8_t *px = surface_to_rgba(data, w, h, stride);
    glTexSubImage2D(A_U(0), A_I(1), A_I(2), A_I(3), w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return JS_UNDEFINED;
}

FN(gl_copyTexImage2D) {
    UNUSED;
    glCopyTexImage2D(A_U(0), A_I(1), A_U(2), A_I(3), A_I(4), A_I(5), A_I(6), A_I(7));
    return JS_UNDEFINED;
}
FN(gl_copyTexSubImage2D) {
    UNUSED;
    glCopyTexSubImage2D(A_U(0), A_I(1), A_I(2), A_I(3), A_I(4), A_I(5), A_I(6), A_I(7));
    return JS_UNDEFINED;
}

// readPixels(x, y, w, h, fmt, type, out)
FN(gl_readPixels) {
    UNUSED;
    size_t len;
    uint8_t *p = rt_get_bytes(ctx, argv[6], &len);
    int w = A_I(2), h = A_I(3);
    if (!p || len < (size_t)w * (size_t)h * 4) return JS_ThrowRangeError(ctx, "readPixels: buffer too small");
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(A_I(0), A_I(1), w, h, A_U(4), A_U(5), p);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// queries

FN(gl_getParameter) {
    UNUSED;
    GLenum p = A_U(0);
    switch (p) {
    case GL_VENDOR: return JS_NewString(ctx, "tcoaal-switch");
    case GL_RENDERER: return JS_NewString(ctx, (const char *)glGetString(GL_RENDERER));
    case GL_VERSION: return JS_NewString(ctx, "WebGL 1.0 (OpenGL ES 2.0)");
    case GL_SHADING_LANGUAGE_VERSION: return JS_NewString(ctx, "WebGL GLSL ES 1.0");
    case 0x9240: return JS_NewBool(ctx, unpack_flip);
    case 0x9241: return JS_NewBool(ctx, unpack_premul);
    case 0x9243: return JS_NewInt32(ctx, 0x9244);
    case GL_BLEND: case GL_CULL_FACE: case GL_DEPTH_TEST: case GL_DITHER: case GL_POLYGON_OFFSET_FILL:
    case GL_SAMPLE_ALPHA_TO_COVERAGE: case GL_SAMPLE_COVERAGE: case GL_SCISSOR_TEST: case GL_STENCIL_TEST:
        return JS_NewBool(ctx, glIsEnabled(p));
    case GL_DEPTH_WRITEMASK: {
        GLboolean b;
        glGetBooleanv(p, &b);
        return JS_NewBool(ctx, b);
    }
    case GL_COLOR_WRITEMASK: {
        GLboolean b[4];
        glGetBooleanv(p, b);
        JSValue a = JS_NewArray(ctx);
        for (uint32_t i = 0; i < 4; i++) JS_SetPropertyUint32(ctx, a, i, JS_NewBool(ctx, b[i]));
        return a;
    }
    case GL_VIEWPORT: case GL_SCISSOR_BOX: case GL_MAX_VIEWPORT_DIMS: {
        GLint v[4] = {0};
        glGetIntegerv(p, v);
        int n = p == GL_MAX_VIEWPORT_DIMS ? 2 : 4;
        JSValue a = JS_NewArray(ctx);
        for (int i = 0; i < n; i++) JS_SetPropertyUint32(ctx, a, (uint32_t)i, JS_NewInt32(ctx, v[i]));
        return a;
    }
    case GL_COLOR_CLEAR_VALUE: case GL_BLEND_COLOR: case GL_ALIASED_LINE_WIDTH_RANGE:
    case GL_ALIASED_POINT_SIZE_RANGE: case GL_DEPTH_RANGE: {
        GLfloat v[4] = {0};
        glGetFloatv(p, v);
        int n = (p == GL_COLOR_CLEAR_VALUE || p == GL_BLEND_COLOR) ? 4 : 2;
        JSValue a = JS_NewArray(ctx);
        for (int i = 0; i < n; i++) JS_SetPropertyUint32(ctx, a, (uint32_t)i, JS_NewFloat64(ctx, v[i]));
        return a;
    }
    case GL_DEPTH_CLEAR_VALUE: case GL_LINE_WIDTH: case GL_POLYGON_OFFSET_FACTOR: case GL_POLYGON_OFFSET_UNITS:
    case GL_SAMPLE_COVERAGE_VALUE: {
        GLfloat f = 0;
        glGetFloatv(p, &f);
        return JS_NewFloat64(ctx, f);
    }
    default: {
        GLint v = 0;
        glGetIntegerv(p, &v);
        return JS_NewInt32(ctx, v);
    }
    }
}

FN(gl_getTexParameter) {
    UNUSED;
    GLint v = 0;
    glGetTexParameteriv(A_U(0), A_U(1), &v);
    return JS_NewInt32(ctx, v);
}
FN(gl_getBufferParameter) {
    UNUSED;
    GLint v = 0;
    glGetBufferParameteriv(A_U(0), A_U(1), &v);
    return JS_NewInt32(ctx, v);
}
FN(gl_getVertexAttrib) {
    UNUSED;
    GLint v = 0;
    glGetVertexAttribiv((GLuint)A_I(0), A_U(1), &v);
    return JS_NewInt32(ctx, v);
}

// ---------------------------------------------------------------------------

#define F(name, n) JS_CFUNC_DEF(#name, n, gl_##name)
static const JSCFunctionListEntry gl_funcs[] = {
    F(activeTexture, 1), F(attachShader, 2), F(bindAttribLocation, 3), F(bindBuffer, 2),
    F(bindFramebuffer, 2), F(bindRenderbuffer, 2), F(bindTexture, 2), F(blendColor, 4),
    F(blendEquation, 1), F(blendEquationSeparate, 2), F(blendFunc, 2), F(blendFuncSeparate, 4),
    F(bufferData, 3), F(bufferSubData, 3), F(checkFramebufferStatus, 1), F(clear, 1),
    F(clearColor, 4), F(clearDepth, 1), F(clearStencil, 1), F(colorMask, 4), F(compileShader, 1),
    F(copyTexImage2D, 8), F(copyTexSubImage2D, 8), F(createBuffer, 0), F(createFramebuffer, 0),
    F(createProgram, 0), F(createRenderbuffer, 0), F(createShader, 1), F(createTexture, 0),
    F(cullFace, 1), F(deleteBuffer, 1), F(deleteFramebuffer, 1), F(deleteProgram, 1),
    F(deleteRenderbuffer, 1), F(deleteShader, 1), F(deleteTexture, 1), F(depthFunc, 1),
    F(depthMask, 1), F(depthRange, 2), F(detachShader, 2), F(disable, 1),
    F(disableVertexAttribArray, 1), F(drawArrays, 3), F(drawElements, 4), F(enable, 1),
    F(enableVertexAttribArray, 1), F(finish, 0), F(flush, 0), F(framebufferRenderbuffer, 4),
    F(framebufferTexture2D, 5), F(frontFace, 1), F(generateMipmap, 1), F(getActiveAttrib, 2),
    F(getActiveUniform, 2), F(getAttribLocation, 2), F(getBufferParameter, 2), F(getError, 0),
    F(getParameter, 1), F(getProgramInfoLog, 1), F(getProgramParameter, 2), F(getShaderInfoLog, 1),
    F(getShaderParameter, 2), F(getShaderPrecisionFormat, 2), F(getShaderSource, 1),
    F(getTexParameter, 2), F(getUniformLocation, 2), F(getVertexAttrib, 2), F(hint, 2),
    F(isBuffer, 1), F(isEnabled, 1), F(isFramebuffer, 1), F(isProgram, 1), F(isRenderbuffer, 1),
    F(isShader, 1), F(isTexture, 1), F(lineWidth, 1), F(linkProgram, 1), F(pixelStorei, 2),
    F(polygonOffset, 2), F(readPixels, 7), F(renderbufferStorage, 4), F(sampleCoverage, 2),
    F(scissor, 4), F(shaderSource, 2), F(stencilFunc, 3), F(stencilFuncSeparate, 4),
    F(stencilMask, 1), F(stencilMaskSeparate, 2), F(stencilOp, 3), F(stencilOpSeparate, 4),
    F(texImage2D, 9), F(texSubImage2D, 9), F(texImageSurface, 6), F(texSubImageSurface, 7),
    F(texParameterf, 3), F(texParameteri, 3),
    F(uniform1f, 2), F(uniform2f, 3), F(uniform3f, 4), F(uniform4f, 5),
    F(uniform1i, 2), F(uniform2i, 3), F(uniform3i, 4), F(uniform4i, 5),
    F(uniform1fv, 2), F(uniform2fv, 2), F(uniform3fv, 2), F(uniform4fv, 2),
    F(uniform1iv, 2), F(uniform2iv, 2), F(uniform3iv, 2), F(uniform4iv, 2),
    F(uniformMatrix2fv, 3), F(uniformMatrix3fv, 3), F(uniformMatrix4fv, 3),
    F(useProgram, 1), F(validateProgram, 1), F(vertexAttrib1f, 2), F(vertexAttrib2f, 3),
    F(vertexAttrib3f, 4), F(vertexAttrib4f, 5), F(vertexAttribPointer, 6), F(viewport, 4),
};
#undef F

void rt_init_gl(JSContext *ctx, JSValueConst ns) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &globj_class);
    JS_NewClass(rt, globj_class, &(JSClassDef){.class_name = "WebGLObject", .finalizer = globj_finalizer});
    JS_NewClassID(rt, &uniloc_class);
    JS_NewClass(rt, uniloc_class, &(JSClassDef){.class_name = "WebGLUniformLocation", .finalizer = uniloc_finalizer});

    JSValue gl = JS_NewObject(ctx);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, gl_funcs, sizeof gl_funcs / sizeof gl_funcs[0]);
    for (size_t i = 0; i < sizeof gl_consts / sizeof gl_consts[0]; i++)
        JS_SetPropertyStr(ctx, proto, gl_consts[i].name, JS_NewInt32(ctx, (int32_t)gl_consts[i].value));
    JS_SetPropertyStr(ctx, gl, "proto", proto);
    JS_SetPropertyStr(ctx, ns, "gl", gl);
}
