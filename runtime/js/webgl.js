// WebGLRenderingContext over the native GLES2 binding. All canvases share the one GL context.
'use strict';
(function (global) {
    const P = __native.gl.proto;
    const rt = global.__rt;

    const nativeTexImage2D = P.texImage2D;
    const nativeTexSubImage2D = P.texSubImage2D;

    function sourceSurface(src) {
        if (!src) return null;
        if (src._surface) return src._surface;
        if (src instanceof global.HTMLCanvasElement) return src._ensureSurface();
        return null;
    }

    // texImage2D(target, level, ifmt, w, h, border, fmt, type, pixels)
    // texImage2D(target, level, ifmt, fmt, type, source)
    P.texImage2D = function (target, level, ifmt, a, b, c, d, e, f) {
        if (arguments.length === 6) {
            const src = c;
            const surf = sourceSurface(src);
            if (surf) return this.texImageSurface(target, level, ifmt, a, b, surf);
            if (src && src.data && src.width !== undefined)  // ImageData
                return nativeTexImage2D.call(this, target, level, ifmt, src.width, src.height, 0, a, b, src.data);
            if (src instanceof global.HTMLVideoElement) return;
            throw new TypeError('texImage2D: unsupported source');
        }
        return nativeTexImage2D.call(this, target, level, ifmt, a, b, c, d, e, f === undefined ? null : f);
    };

    // texSubImage2D(target, level, x, y, w, h, fmt, type, pixels)
    // texSubImage2D(target, level, x, y, fmt, type, source)
    P.texSubImage2D = function (target, level, x, y, a, b, c, d, e) {
        if (arguments.length === 7) {
            const src = c;
            const surf = sourceSurface(src);
            if (surf) return this.texSubImageSurface(target, level, x, y, a, b, surf);
            if (src && src.data && src.width !== undefined)
                return nativeTexSubImage2D.call(this, target, level, x, y, src.width, src.height, a, b, src.data);
            return;
        }
        return nativeTexSubImage2D.call(this, target, level, x, y, a, b, c, d, e);
    };

    // Typed-array helpers that WebGL accepts but GLES does not care about.
    P.getExtension = function (name) {
        return null;
    };
    P.getSupportedExtensions = function () { return []; };
    P.isContextLost = function () { return false; };
    P.getContextAttributes = function () { return Object.assign({}, this._attrs); };
    P.compressedTexImage2D = function () {};
    P.compressedTexSubImage2D = function () {};

    function WebGLRenderingContext() { throw new TypeError('Illegal constructor'); }
    WebGLRenderingContext.prototype = P;
    for (const k in P) if (typeof P[k] === 'number') WebGLRenderingContext[k] = P[k];
    global.WebGLRenderingContext = WebGLRenderingContext;
    global.WebGLObject = function WebGLObject() {};
    global.WebGLBuffer = global.WebGLTexture = global.WebGLProgram = global.WebGLShader = global.WebGLObject;
    global.WebGLFramebuffer = global.WebGLRenderbuffer = global.WebGLUniformLocation = global.WebGLObject;

    rt.createWebGLContext = function (canvas, attrs) {
        const gl = Object.create(P);
        gl.canvas = canvas;
        gl._attrs = {
            alpha: attrs.alpha !== false, antialias: false, depth: false, stencil: true,
            premultipliedAlpha: attrs.premultipliedAlpha !== false, preserveDrawingBuffer: !!attrs.preserveDrawingBuffer,
            failIfMajorPerformanceCaveat: false,
        };
        Object.defineProperty(gl, 'drawingBufferWidth', { get: () => canvas.width });
        Object.defineProperty(gl, 'drawingBufferHeight', { get: () => canvas.height });
        return gl;
    };
})(globalThis);
