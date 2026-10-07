// Canvas 2D backend on plutovg. JS keeps the CanvasRenderingContext2D state
// (styles, font strings); this file owns pixels, paths, clip and transforms.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <SDL.h>
#include "plutovg.h"
#include "rt.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

// ---------------------------------------------------------------------------
// fonts

typedef struct Font {
    char family[64];
    bool bold, italic;
    plutovg_font_face_t *face;
    stbtt_fontinfo info;  // for glyph coverage checks
    uint8_t *data;
} Font;

#define MAX_FONTS 32
static Font fonts[MAX_FONTS];
static int nfonts;
static Font *fallback_font;

static Font *font_add(const char *family, bool bold, bool italic, uint8_t *data, size_t len) {
    if (nfonts >= MAX_FONTS) return NULL;
    Font *f = &fonts[nfonts];
    if (!stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0))) return NULL;
    f->face = plutovg_font_face_load_from_data(data, (unsigned)len, 0, NULL, NULL);
    if (!f->face) return NULL;
    snprintf(f->family, sizeof f->family, "%s", family);
    f->bold = bold;
    f->italic = italic;
    f->data = data;
    nfonts++;
    return f;
}

static Font *font_find(const char *family, bool bold, bool italic) {
    Font *any = NULL;
    for (int i = 0; i < nfonts; i++) {
        if (strcasecmp(fonts[i].family, family)) continue;
        if (fonts[i].bold == bold && fonts[i].italic == italic) return &fonts[i];
        if (!any) any = &fonts[i];
    }
    return any;
}

// family list like `"GameFont", Verdana, sans-serif`
static Font *font_resolve(const char *list, bool bold, bool italic) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ' || *tok == '"' || *tok == '\'') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '"' || end[-1] == '\'')) *--end = 0;
        Font *f = font_find(tok, bold, italic);
        if (f) return f;
        if (!strcasecmp(tok, "sans-serif") || !strcasecmp(tok, "serif") || !strcasecmp(tok, "monospace"))
            return fallback_font;
    }
    return fallback_font;
}

// ---------------------------------------------------------------------------
// surfaces

typedef struct Surface {
    plutovg_surface_t *surf;
    plutovg_canvas_t *cv;
    int w, h;
    Font *font;
    float font_size;
    int op;        // composite op code from JS
    int depth;     // save depth, to keep save/restore balanced on resize
} Surface;

static JSClassID surface_class, gradient_class;
static JSValue surface_proto;

static void surface_alloc(Surface *s, int w, int h) {
    if (s->cv) plutovg_canvas_destroy(s->cv);
    if (s->surf) plutovg_surface_destroy(s->surf);
    s->cv = NULL;
    s->surf = NULL;
    s->w = w < 0 ? 0 : w;
    s->h = h < 0 ? 0 : h;
    s->depth = 0;
    s->op = 0;
    if (s->w > 0 && s->h > 0) {
        s->surf = plutovg_surface_create(s->w, s->h);
        if (s->surf) s->cv = plutovg_canvas_create(s->surf);
    }
}

static void surface_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    Surface *s = JS_GetOpaque(val, surface_class);
    if (!s) return;
    if (s->cv) plutovg_canvas_destroy(s->cv);
    if (s->surf) plutovg_surface_destroy(s->surf);
    free(s);
}

static JSValue surface_wrap(JSContext *ctx, Surface *s) {
    JSValue o = JS_NewObjectProtoClass(ctx, surface_proto, surface_class);
    JS_SetOpaque(o, s);
    return o;
}

static Surface *S(JSContext *ctx, JSValueConst v) {
    return JS_GetOpaque2(ctx, v, surface_class);
}

bool rt_canvas_pixels(JSContext *ctx, JSValueConst obj, const uint8_t **data, int *w, int *h, int *stride) {
    (void)ctx;
    Surface *s = JS_GetOpaque(obj, surface_class);
    if (!s) return false;
    *w = s->w;
    *h = s->h;
    *data = s->surf ? plutovg_surface_get_data(s->surf) : NULL;
    *stride = s->surf ? plutovg_surface_get_stride(s->surf) : 0;
    return true;
}

// ---------------------------------------------------------------------------
// gradients

typedef struct Gradient {
    plutovg_paint_t *paint;
} Gradient;

static void gradient_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    Gradient *g = JS_GetOpaque(val, gradient_class);
    if (g) {
        plutovg_paint_destroy(g->paint);
        free(g);
    }
}

static plutovg_gradient_stop_t *read_stops(JSContext *ctx, JSValueConst arr, int *n) {
    size_t len;
    float *f = (float *)rt_get_bytes(ctx, arr, &len);
    *n = f ? (int)(len / sizeof(float) / 5) : 0;
    plutovg_gradient_stop_t *stops = calloc((size_t)(*n ? *n : 1), sizeof *stops);
    for (int i = 0; i < *n; i++) {
        stops[i].offset = f[i * 5];
        stops[i].color = (plutovg_color_t){f[i * 5 + 1], f[i * 5 + 2], f[i * 5 + 3], f[i * 5 + 4]};
    }
    return stops;
}

// linearGradient(x0, y0, x1, y1, Float32Array[offset,r,g,b,a,...])
static JSValue js_linear_gradient(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    int n;
    plutovg_gradient_stop_t *stops = read_stops(ctx, argv[4], &n);
    Gradient *g = malloc(sizeof *g);
    g->paint = plutovg_paint_create_linear_gradient(
        (float)rt_arg_f(ctx, argv[0]), (float)rt_arg_f(ctx, argv[1]), (float)rt_arg_f(ctx, argv[2]),
        (float)rt_arg_f(ctx, argv[3]), PLUTOVG_SPREAD_METHOD_PAD, stops, n, NULL);
    free(stops);
    JSValue o = JS_NewObjectClass(ctx, (int)gradient_class);
    JS_SetOpaque(o, g);
    return o;
}

// radialGradient(x0, y0, r0, x1, y1, r1, stops)
static JSValue js_radial_gradient(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    int n;
    plutovg_gradient_stop_t *stops = read_stops(ctx, argv[6], &n);
    Gradient *g = malloc(sizeof *g);
    // canvas: circle 0 is the focal circle, circle 1 the outer one
    g->paint = plutovg_paint_create_radial_gradient(
        (float)rt_arg_f(ctx, argv[3]), (float)rt_arg_f(ctx, argv[4]), (float)rt_arg_f(ctx, argv[5]),
        (float)rt_arg_f(ctx, argv[0]), (float)rt_arg_f(ctx, argv[1]), (float)rt_arg_f(ctx, argv[2]),
        PLUTOVG_SPREAD_METHOD_PAD, stops, n, NULL);
    free(stops);
    JSValue o = JS_NewObjectClass(ctx, (int)gradient_class);
    JS_SetOpaque(o, g);
    return o;
}

// paint: [r,g,b,a] (0..1) or a Gradient. Returns false for "nothing to draw".
static bool apply_paint_to(JSContext *ctx, plutovg_canvas_t *cv, JSValueConst paint) {
    Gradient *g = JS_GetOpaque(paint, gradient_class);
    if (g) {
        plutovg_canvas_set_paint(cv, g->paint);
        return true;
    }
    double c[4] = {0, 0, 0, 1};
    for (int i = 0; i < 4; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, paint, (uint32_t)i);
        JS_ToFloat64(ctx, &c[i], v);
        JS_FreeValue(ctx, v);
    }
    plutovg_canvas_set_rgba(cv, (float)c[0], (float)c[1], (float)c[2], (float)c[3]);
    return true;
}

static bool apply_paint(JSContext *ctx, Surface *s, JSValueConst paint) { return apply_paint_to(ctx, s->cv, paint); }

// ---------------------------------------------------------------------------
// composite operations

enum {
    OP_SOURCE_OVER, OP_COPY, OP_DEST_IN, OP_DEST_OUT, OP_SOURCE_ATOP, OP_SOURCE_IN,
    OP_SOURCE_OUT, OP_DEST_OVER, OP_DEST_ATOP, OP_XOR, OP_LIGHTER, OP_UNSUPPORTED
};

static void apply_op(Surface *s) {
    static const plutovg_operator_t map[] = {
        PLUTOVG_OPERATOR_SRC_OVER, PLUTOVG_OPERATOR_SRC, PLUTOVG_OPERATOR_DST_IN, PLUTOVG_OPERATOR_DST_OUT,
        PLUTOVG_OPERATOR_SRC_ATOP, PLUTOVG_OPERATOR_SRC_IN, PLUTOVG_OPERATOR_SRC_OUT, PLUTOVG_OPERATOR_DST_OVER,
        PLUTOVG_OPERATOR_DST_ATOP, PLUTOVG_OPERATOR_XOR};
    plutovg_canvas_set_operator(s->cv, s->op < OP_LIGHTER ? map[s->op] : PLUTOVG_OPERATOR_SRC_OVER);
}

// 'lighter' (additive) is not a plutovg operator: draw into a scratch surface
// with the same transform/opacity/line style, then add it onto the target.
typedef struct {
    plutovg_surface_t *surf;
    plutovg_canvas_t *cv;
} Scratch;

static plutovg_canvas_t *op_begin(Surface *s, Scratch *t) {
    t->surf = NULL;
    t->cv = NULL;
    if (s->op != OP_LIGHTER) return s->cv;
    t->surf = plutovg_surface_create(s->w, s->h);
    t->cv = plutovg_canvas_create(t->surf);
    plutovg_matrix_t m;
    plutovg_canvas_get_matrix(s->cv, &m);
    plutovg_canvas_set_matrix(t->cv, &m);
    plutovg_canvas_set_opacity(t->cv, plutovg_canvas_get_opacity(s->cv));
    plutovg_canvas_set_line_width(t->cv, plutovg_canvas_get_line_width(s->cv));
    plutovg_canvas_set_line_join(t->cv, plutovg_canvas_get_line_join(s->cv));
    plutovg_canvas_set_line_cap(t->cv, plutovg_canvas_get_line_cap(s->cv));
    plutovg_canvas_set_miter_limit(t->cv, plutovg_canvas_get_miter_limit(s->cv));
    // carry over the current path for fill()/stroke()
    plutovg_canvas_add_path(t->cv, plutovg_canvas_get_path(s->cv));
    return t->cv;
}

static void op_end(Surface *s, Scratch *t) {
    if (!t->surf) return;
    uint8_t *dst = plutovg_surface_get_data(s->surf);
    const uint8_t *src = plutovg_surface_get_data(t->surf);
    size_t n = (size_t)plutovg_surface_get_stride(s->surf) * (size_t)s->h;
    for (size_t i = 0; i < n; i++) {
        unsigned v = dst[i] + src[i];
        dst[i] = (uint8_t)(v > 255 ? 255 : v);
    }
    plutovg_canvas_destroy(t->cv);
    plutovg_surface_destroy(t->surf);
}

// ---------------------------------------------------------------------------
// surface methods

#define SURF_OR_RETURN(ret)            \
    Surface *s = S(ctx, this);         \
    if (!s) return JS_EXCEPTION;       \
    if (!s->cv) return (ret);

static JSValue js_create(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    Surface *s = calloc(1, sizeof *s);
    surface_alloc(s, rt_arg_i(ctx, argv[0]), rt_arg_i(ctx, argv[1]));
    s->font = fallback_font;
    s->font_size = 10;
    return surface_wrap(ctx, s);
}

static JSValue js_decode_image(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    size_t len;
    uint8_t *b = rt_get_bytes(ctx, argv[0], &len);
    if (!b) return JS_NULL;
    plutovg_surface_t *img = plutovg_surface_load_from_image_data(b, (int)len);
    if (!img) return JS_NULL;
    Surface *s = calloc(1, sizeof *s);
    s->surf = img;
    s->w = plutovg_surface_get_width(img);
    s->h = plutovg_surface_get_height(img);
    s->cv = plutovg_canvas_create(img);
    s->font = fallback_font;
    s->font_size = 10;
    return surface_wrap(ctx, s);
}

// Asynchronous image loading: decodeImageAsync(pathOrBytes, cb) reads (for a
// string path) and decodes on a worker thread, then calls cb(surface | null) from
// the main loop. Keeps PNG decoding off the main thread during map transitions.
typedef struct ImageJob {
    struct ImageJob *next;
    char *path;          // virtual path, or NULL when bytes were given
    uint8_t *bytes;
    size_t len;
    JSValue cb;          // only touched on the main thread
    plutovg_surface_t *img;
} ImageJob;

#define IMAGE_WORKERS 2
static SDL_mutex *img_lock;
static SDL_cond *img_cond;
static ImageJob *img_head, *img_tail;

static void image_done(JSContext *ctx, void *data) {
    ImageJob *j = data;
    JSValue arg = JS_NULL;
    if (j->img) {
        Surface *s = calloc(1, sizeof *s);
        s->surf = j->img;
        s->w = plutovg_surface_get_width(j->img);
        s->h = plutovg_surface_get_height(j->img);
        s->cv = plutovg_canvas_create(j->img);
        s->font = fallback_font;
        s->font_size = 10;
        arg = surface_wrap(ctx, s);
    }
    JSValue r = JS_Call(ctx, j->cb, JS_UNDEFINED, 1, &arg);
    if (JS_IsException(r)) rt_dump_exception(ctx);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, j->cb);
    free(j);
}

static int image_worker(void *arg) {
    (void)arg;
    for (;;) {
        SDL_LockMutex(img_lock);
        while (!img_head) SDL_CondWait(img_cond, img_lock);
        ImageJob *j = img_head;
        img_head = j->next;
        if (!img_head) img_tail = NULL;
        SDL_UnlockMutex(img_lock);

        if (j->path) {
            j->bytes = rt_fs_read(j->path, &j->len);
            free(j->path);
        }
        j->img = j->bytes ? plutovg_surface_load_from_image_data(j->bytes, (int)j->len) : NULL;
        free(j->bytes);
        rt_post(image_done, j);
    }
    return 0;
}

static JSValue js_decode_image_async(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    if (!JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "decodeImageAsync: callback expected");
    ImageJob *j = calloc(1, sizeof *j);
    if (JS_IsString(argv[0])) {
        const char *p = JS_ToCString(ctx, argv[0]);
        j->path = strdup(p ? p : "");
        JS_FreeCString(ctx, p);
    } else {
        size_t len;
        uint8_t *b = rt_get_bytes(ctx, argv[0], &len);
        if (b && (j->bytes = malloc(len ? len : 1))) {
            memcpy(j->bytes, b, len);
            j->len = len;
        }
    }
    j->cb = JS_DupValue(ctx, argv[1]);
    if (!img_lock) {
        img_lock = SDL_CreateMutex();
        img_cond = SDL_CreateCond();
        for (int i = 0; i < IMAGE_WORKERS; i++) {
            SDL_Thread *t = SDL_CreateThread(image_worker, "image", NULL);
            if (t) SDL_DetachThread(t);
        }
    }
    SDL_LockMutex(img_lock);
    if (img_tail) img_tail->next = j;
    else img_head = j;
    img_tail = j;
    SDL_CondSignal(img_cond);
    SDL_UnlockMutex(img_lock);
    return JS_UNDEFINED;
}

static JSValue js_resize(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc;
    Surface *s = S(ctx, this);
    if (!s) return JS_EXCEPTION;
    surface_alloc(s, rt_arg_i(ctx, argv[0]), rt_arg_i(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue js_get_w(JSContext *ctx, JSValueConst this) {
    Surface *s = S(ctx, this);
    return s ? JS_NewInt32(ctx, s->w) : JS_EXCEPTION;
}
static JSValue js_get_h(JSContext *ctx, JSValueConst this) {
    Surface *s = S(ctx, this);
    return s ? JS_NewInt32(ctx, s->h) : JS_EXCEPTION;
}

static JSValue js_save(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    SURF_OR_RETURN(JS_UNDEFINED);
    plutovg_canvas_save(s->cv);
    s->depth++;
    return JS_UNDEFINED;
}

static JSValue js_restore(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    SURF_OR_RETURN(JS_UNDEFINED);
    if (s->depth > 0) {
        plutovg_canvas_restore(s->cv);
        s->depth--;
    }
    return JS_UNDEFINED;
}

// setTransform(a,b,c,d,e,f) / transform(...) / translate / scale / rotate
static JSValue js_set_transform(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv, int magic) {
    SURF_OR_RETURN(JS_UNDEFINED);
    float v[6] = {0};
    for (int i = 0; i < argc && i < 6; i++) v[i] = (float)rt_arg_f(ctx, argv[i]);
    plutovg_matrix_t m;
    switch (magic) {
    case 0: plutovg_matrix_init(&m, v[0], v[1], v[2], v[3], v[4], v[5]); plutovg_canvas_set_matrix(s->cv, &m); break;
    case 1: plutovg_matrix_init(&m, v[0], v[1], v[2], v[3], v[4], v[5]); plutovg_canvas_transform(s->cv, &m); break;
    case 2: plutovg_canvas_translate(s->cv, v[0], v[1]); break;
    case 3: plutovg_canvas_scale(s->cv, v[0], v[1]); break;
    case 4: plutovg_canvas_rotate(s->cv, v[0]); break;
    }
    return JS_UNDEFINED;
}

static JSValue js_get_transform(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    Surface *s = S(ctx, this);
    if (!s) return JS_EXCEPTION;
    plutovg_matrix_t m;
    if (s->cv) plutovg_canvas_get_matrix(s->cv, &m);
    else plutovg_matrix_init_identity(&m);
    JSValue a = JS_NewArray(ctx);
    float v[6] = {m.a, m.b, m.c, m.d, m.e, m.f};
    for (uint32_t i = 0; i < 6; i++) JS_SetPropertyUint32(ctx, a, i, JS_NewFloat64(ctx, v[i]));
    return a;
}

static JSValue js_set_alpha(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc;
    SURF_OR_RETURN(JS_UNDEFINED);
    plutovg_canvas_set_opacity(s->cv, (float)rt_arg_f(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_set_op(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc;
    SURF_OR_RETURN(JS_UNDEFINED);
    s->op = rt_arg_i(ctx, argv[0]);
    apply_op(s);
    return JS_UNDEFINED;
}

// setLine(width, cap, join, miter)
static JSValue js_set_line(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc;
    SURF_OR_RETURN(JS_UNDEFINED);
    plutovg_canvas_set_line_width(s->cv, (float)rt_arg_f(ctx, argv[0]));
    plutovg_canvas_set_line_cap(s->cv, (plutovg_line_cap_t)rt_arg_i(ctx, argv[1]));
    plutovg_canvas_set_line_join(s->cv, (plutovg_line_join_t)rt_arg_i(ctx, argv[2]));
    plutovg_canvas_set_miter_limit(s->cv, (float)rt_arg_f(ctx, argv[3]));
    return JS_UNDEFINED;
}

// setFont(familyList, size, bold, italic)
static JSValue js_set_font(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc;
    Surface *s = S(ctx, this);
    if (!s) return JS_EXCEPTION;
    const char *fam = JS_ToCString(ctx, argv[0]);
    s->font = font_resolve(fam ? fam : "", JS_ToBool(ctx, argv[2]), JS_ToBool(ctx, argv[3]));
    JS_FreeCString(ctx, fam);
    s->font_size = (float)rt_arg_f(ctx, argv[1]);
    return JS_UNDEFINED;
}

// path building: magic selects the op
static JSValue js_path(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv, int magic) {
    SURF_OR_RETURN(JS_UNDEFINED);
    float v[8] = {0};
    for (int i = 0; i < argc && i < 8; i++) v[i] = (float)rt_arg_f(ctx, argv[i]);
    plutovg_canvas_t *c = s->cv;
    switch (magic) {
    case 0: plutovg_canvas_new_path(c); break;
    case 1: plutovg_canvas_move_to(c, v[0], v[1]); break;
    case 2: plutovg_canvas_line_to(c, v[0], v[1]); break;
    case 3: plutovg_canvas_close_path(c); break;
    case 4: plutovg_canvas_rect(c, v[0], v[1], v[2], v[3]); break;
    case 5: plutovg_canvas_arc(c, v[0], v[1], v[2], v[3], v[4], argc > 5 && JS_ToBool(ctx, argv[5])); break;
    case 6: plutovg_canvas_cubic_to(c, v[0], v[1], v[2], v[3], v[4], v[5]); break;
    case 7: plutovg_canvas_quad_to(c, v[0], v[1], v[2], v[3]); break;
    }
    return JS_UNDEFINED;
}

// fill(paint, evenodd) / stroke(paint) / clip(evenodd) — keep the path like canvas does
static JSValue js_fill(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv, int magic) {
    SURF_OR_RETURN(JS_UNDEFINED);
    if (magic == 2) {
        plutovg_canvas_set_fill_rule(s->cv, argc > 0 && JS_ToBool(ctx, argv[0]) ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
        plutovg_canvas_clip_preserve(s->cv);
        return JS_UNDEFINED;
    }
    Scratch t;
    plutovg_canvas_t *c = op_begin(s, &t);
    apply_paint_to(ctx, c, argv[0]);
    if (magic == 0) {
        plutovg_canvas_set_fill_rule(c, argc > 1 && JS_ToBool(ctx, argv[1]) ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
        plutovg_canvas_fill_preserve(c);
    } else {
        plutovg_canvas_stroke_preserve(c);
    }
    op_end(s, &t);
    return JS_UNDEFINED;
}

// fillRect(x,y,w,h,paint) / strokeRect(x,y,w,h,paint) / clearRect(x,y,w,h)
static JSValue js_rect_op(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv, int magic) {
    (void)argc;
    SURF_OR_RETURN(JS_UNDEFINED);
    float x = (float)rt_arg_f(ctx, argv[0]), y = (float)rt_arg_f(ctx, argv[1]);
    float w = (float)rt_arg_f(ctx, argv[2]), h = (float)rt_arg_f(ctx, argv[3]);
    Scratch t;
    plutovg_canvas_t *c = magic == 2 ? s->cv : op_begin(s, &t);
    plutovg_canvas_save(c);
    plutovg_canvas_new_path(c);
    plutovg_canvas_rect(c, x, y, w, h);
    if (magic == 2) {
        plutovg_canvas_set_operator(c, PLUTOVG_OPERATOR_CLEAR);
        plutovg_canvas_set_opacity(c, 1);
        plutovg_canvas_set_rgba(c, 0, 0, 0, 0);
        plutovg_canvas_fill(c);
    } else {
        apply_paint_to(ctx, c, argv[4]);
        plutovg_canvas_set_fill_rule(c, PLUTOVG_FILL_RULE_NON_ZERO);
        if (magic == 0) plutovg_canvas_fill(c);
        else plutovg_canvas_stroke(c);
    }
    plutovg_canvas_restore(c);
    if (magic != 2) op_end(s, &t);
    return JS_UNDEFINED;
}

// drawImage(src, sx, sy, sw, sh, dx, dy, dw, dh)
static JSValue js_draw_image(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc;
    SURF_OR_RETURN(JS_UNDEFINED);
    Surface *src = S(ctx, argv[0]);
    if (!src) return JS_EXCEPTION;
    if (!src->surf) return JS_UNDEFINED;
    float sx = (float)rt_arg_f(ctx, argv[1]), sy = (float)rt_arg_f(ctx, argv[2]);
    float sw = (float)rt_arg_f(ctx, argv[3]), sh = (float)rt_arg_f(ctx, argv[4]);
    float dx = (float)rt_arg_f(ctx, argv[5]), dy = (float)rt_arg_f(ctx, argv[6]);
    float dw = (float)rt_arg_f(ctx, argv[7]), dh = (float)rt_arg_f(ctx, argv[8]);
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0) return JS_UNDEFINED;
    // normalise negative sizes
    if (sw < 0) { sx += sw; sw = -sw; }
    if (sh < 0) { sy += sh; sh = -sh; }
    if (dw < 0) { dx += dw; dw = -dw; }
    if (dh < 0) { dy += dh; dh = -dh; }
    float kx = dw / sw, ky = dh / sh;
    // clip the source rect to the image, adjusting the destination
    if (sx < 0) { dx -= sx * kx; sw += sx; sx = 0; }
    if (sy < 0) { dy -= sy * ky; sh += sy; sy = 0; }
    if (sx + sw > src->w) sw = (float)src->w - sx;
    if (sy + sh > src->h) sh = (float)src->h - sy;
    if (sw <= 0 || sh <= 0) return JS_UNDEFINED;
    dw = sw * kx;
    dh = sh * ky;

    plutovg_surface_t *img = src->surf;
    bool copied = false;
    if (src == s) {  // drawing a canvas onto itself: sample a snapshot
        img = plutovg_surface_create(src->w, src->h);
        memcpy(plutovg_surface_get_data(img), plutovg_surface_get_data(src->surf),
               (size_t)plutovg_surface_get_stride(src->surf) * (size_t)src->h);
        copied = true;
    }
    Scratch t;
    plutovg_canvas_t *c = op_begin(s, &t);
    plutovg_canvas_save(c);
    plutovg_matrix_t m;
    plutovg_matrix_init(&m, kx, 0, 0, ky, dx - sx * kx, dy - sy * ky);
    plutovg_canvas_set_texture(c, img, PLUTOVG_TEXTURE_TYPE_PLAIN, 1.0f, &m);
    plutovg_canvas_set_fill_rule(c, PLUTOVG_FILL_RULE_NON_ZERO);
    plutovg_canvas_new_path(c);
    plutovg_canvas_rect(c, dx, dy, dw, dh);
    plutovg_canvas_fill(c);
    plutovg_canvas_restore(c);
    op_end(s, &t);
    if (copied) plutovg_surface_destroy(img);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// text

static uint32_t utf8_next(const char **p, const char *end) {
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t c = *s++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) c &= (0x3F >> extra);
    while (extra-- && (const char *)s < end) c = (c << 6) | (*s++ & 0x3F);
    *p = (const char *)s;
    return c;
}

static Font *font_for(Font *f, uint32_t cp) {
    if (f && stbtt_FindGlyphIndex(&f->info, (int)cp)) return f;
    if (fallback_font && stbtt_FindGlyphIndex(&fallback_font->info, (int)cp)) return fallback_font;
    return f ? f : fallback_font;
}

// Adds glyphs to the current path (if add) and returns the advance width.
static float layout_text(Surface *s, const char *text, size_t len, float x, float y, bool add) {
    const char *p = text, *end = text + len;
    float pen = x;
    Font *cur = NULL;
    while (p < end) {
        uint32_t cp = utf8_next(&p, end);
        Font *f = font_for(s->font, cp);
        if (!f) continue;
        if (add) {
            if (f != cur) plutovg_canvas_set_font(s->cv, f->face, s->font_size);
            cur = f;
            pen += plutovg_canvas_add_glyph(s->cv, cp, pen, y);
        } else {
            float adv;
            plutovg_font_face_get_glyph_metrics(f->face, s->font_size, cp, &adv, NULL, NULL);
            pen += adv;
        }
    }
    return pen - x;
}

static void font_vmetrics(Surface *s, float *ascent, float *descent) {
    *ascent = s->font_size * 0.8f;
    *descent = s->font_size * 0.2f;
    if (s->font) {
        float a, d, g;
        plutovg_font_face_get_metrics(s->font->face, s->font_size, &a, &d, &g, NULL);
        *ascent = a;
        *descent = d < 0 ? -d : d;
    }
}

// measureText(text) -> [width, ascent, descent]
static JSValue js_measure_text(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc;
    Surface *s = S(ctx, this);
    if (!s) return JS_EXCEPTION;
    size_t len;
    const char *t = JS_ToCStringLen(ctx, &len, argv[0]);
    float w = t ? layout_text(s, t, len, 0, 0, false) : 0;
    JS_FreeCString(ctx, t);
    float a, d;
    font_vmetrics(s, &a, &d);
    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewFloat64(ctx, w));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewFloat64(ctx, a));
    JS_SetPropertyUint32(ctx, r, 2, JS_NewFloat64(ctx, d));
    return r;
}

// drawText(text, x, y, maxWidth|-1, align, baseline, paint, stroke)
// align: 0 left/start 1 center 2 right/end; baseline: 0 alphabetic 1 top 2 middle 3 bottom 4 hanging 5 ideographic
static JSValue js_draw_text(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc;
    SURF_OR_RETURN(JS_UNDEFINED);
    if (!s->font) return JS_UNDEFINED;
    size_t len;
    const char *t = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!t) return JS_EXCEPTION;
    float x = (float)rt_arg_f(ctx, argv[1]), y = (float)rt_arg_f(ctx, argv[2]);
    float maxw = (float)rt_arg_f(ctx, argv[3]);
    int align = rt_arg_i(ctx, argv[4]), baseline = rt_arg_i(ctx, argv[5]);
    bool stroke = JS_ToBool(ctx, argv[7]);

    float w = layout_text(s, t, len, 0, 0, false);
    float a, d;
    font_vmetrics(s, &a, &d);
    switch (baseline) {
    case 1: case 4: y += a; break;
    case 2: y += (a - d) / 2; break;
    case 3: case 5: y -= d; break;
    }
    float anchor = x;
    if (align == 1) x -= w / 2;
    else if (align == 2) x -= w;

    plutovg_canvas_t *c = s->cv;
    plutovg_canvas_save(c);
    if (maxw >= 0 && w > maxw && w > 0) {
        float k = maxw / w;
        plutovg_canvas_translate(c, anchor, 0);
        plutovg_canvas_scale(c, k, 1);
        plutovg_canvas_translate(c, -anchor, 0);
    }
    plutovg_canvas_new_path(c);
    layout_text(s, t, len, x, y, true);
    apply_paint(ctx, s, argv[6]);
    plutovg_canvas_set_fill_rule(c, PLUTOVG_FILL_RULE_NON_ZERO);
    if (stroke) plutovg_canvas_stroke(c);
    else plutovg_canvas_fill(c);
    plutovg_canvas_restore(c);
    JS_FreeCString(ctx, t);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// pixel access

static inline uint8_t unpremul(uint32_t c, uint32_t a) {
    return a ? (uint8_t)((c * 255 + a / 2) / a) : 0;
}

// getImageData(x, y, w, h) -> ArrayBuffer RGBA (straight alpha)
static JSValue js_get_image_data(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc;
    Surface *s = S(ctx, this);
    if (!s) return JS_EXCEPTION;
    int x0 = rt_arg_i(ctx, argv[0]), y0 = rt_arg_i(ctx, argv[1]);
    int w = rt_arg_i(ctx, argv[2]), h = rt_arg_i(ctx, argv[3]);
    if (w <= 0 || h <= 0) return JS_ThrowRangeError(ctx, "getImageData: bad size");
    uint8_t *out = calloc((size_t)w * (size_t)h, 4);
    if (s->surf) {
        const uint8_t *data = plutovg_surface_get_data(s->surf);
        int stride = plutovg_surface_get_stride(s->surf);
        for (int y = 0; y < h; y++) {
            int sy = y0 + y;
            if (sy < 0 || sy >= s->h) continue;
            const uint32_t *row = (const uint32_t *)(data + (size_t)sy * (size_t)stride);
            uint8_t *o = out + (size_t)y * (size_t)w * 4;
            for (int x = 0; x < w; x++) {
                int sx = x0 + x;
                if (sx < 0 || sx >= s->w) continue;
                uint32_t p = row[sx], a = p >> 24;
                o[x * 4 + 0] = unpremul((p >> 16) & 0xFF, a);
                o[x * 4 + 1] = unpremul((p >> 8) & 0xFF, a);
                o[x * 4 + 2] = unpremul(p & 0xFF, a);
                o[x * 4 + 3] = (uint8_t)a;
            }
        }
    }
    return rt_new_ab(ctx, out, (size_t)w * (size_t)h * 4);
}

// putImageData(bytes, dx, dy, w, h)
static JSValue js_put_image_data(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc;
    SURF_OR_RETURN(JS_UNDEFINED);
    size_t len;
    const uint8_t *in = rt_get_bytes(ctx, argv[0], &len);
    int dx = rt_arg_i(ctx, argv[1]), dy = rt_arg_i(ctx, argv[2]);
    int w = rt_arg_i(ctx, argv[3]), h = rt_arg_i(ctx, argv[4]);
    if (!in || len < (size_t)w * (size_t)h * 4) return JS_ThrowRangeError(ctx, "putImageData: short buffer");
    uint8_t *data = plutovg_surface_get_data(s->surf);
    int stride = plutovg_surface_get_stride(s->surf);
    for (int y = 0; y < h; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= s->h) continue;
        uint32_t *row = (uint32_t *)(data + (size_t)ty * (size_t)stride);
        const uint8_t *src = in + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= s->w) continue;
            uint32_t a = src[x * 4 + 3];
            uint32_t r = (src[x * 4] * a + 127) / 255, g = (src[x * 4 + 1] * a + 127) / 255, b = (src[x * 4 + 2] * a + 127) / 255;
            row[tx] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    return JS_UNDEFINED;
}

// encodePNG() -> ArrayBuffer (for toDataURL)
static void png_write(void *closure, void *data, int size) {
    uint8_t **buf = closure;
    size_t *len = (size_t *)(buf + 1);
    *buf = realloc(*buf, *len + (size_t)size);
    memcpy(*buf + *len, data, (size_t)size);
    *len += (size_t)size;
}

// boxBlur(passes): RPG Maker's Bitmap#blur done natively. Each pass is a 3x3 box
// filter with clamped edges, drawn over black, so the result is opaque. (The JS
// version takes 28 full-surface draws per call.)
static JSValue js_box_blur(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    SURF_OR_RETURN(JS_UNDEFINED);
    int passes = argc > 0 ? rt_arg_i(ctx, argv[0]) : 1;
    int w = s->w, h = s->h, stride = plutovg_surface_get_stride(s->surf) / 4;
    uint32_t *px = (uint32_t *)plutovg_surface_get_data(s->surf);
    uint16_t *row = malloc((size_t)w * h * 3 * sizeof *row);  // horizontal sums, per channel
    if (!row) return JS_UNDEFINED;
    for (int p = 0; p < passes; p++) {
        for (int y = 0; y < h; y++) {
            const uint32_t *r = px + (size_t)y * stride;
            uint16_t *o = row + (size_t)y * w * 3;
            for (int x = 0; x < w; x++) {
                uint32_t a = r[x > 0 ? x - 1 : 0], b = r[x], c = r[x < w - 1 ? x + 1 : w - 1];
                o[x * 3 + 0] = (uint16_t)(((a >> 16) & 255) + ((b >> 16) & 255) + ((c >> 16) & 255));
                o[x * 3 + 1] = (uint16_t)(((a >> 8) & 255) + ((b >> 8) & 255) + ((c >> 8) & 255));
                o[x * 3 + 2] = (uint16_t)((a & 255) + (b & 255) + (c & 255));
            }
        }
        for (int y = 0; y < h; y++) {
            const uint16_t *a = row + (size_t)(y > 0 ? y - 1 : 0) * w * 3, *b = row + (size_t)y * w * 3,
                           *c = row + (size_t)(y < h - 1 ? y + 1 : h - 1) * w * 3;
            uint32_t *o = px + (size_t)y * stride;
            for (int x = 0; x < w * 3; x += 3) {
                uint32_t R = (a[x] + b[x] + c[x] + 4) / 9, G = (a[x + 1] + b[x + 1] + c[x + 1] + 4) / 9,
                         B = (a[x + 2] + b[x + 2] + c[x + 2] + 4) / 9;
                o[x / 3] = 0xFF000000u | R << 16 | G << 8 | B;
            }
        }
    }
    free(row);
    return JS_UNDEFINED;
}

static JSValue js_encode_png(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    SURF_OR_RETURN(JS_NULL);
    void *state[2] = {NULL, 0};
    plutovg_surface_write_to_png_stream(s->surf, png_write, state);
    return rt_new_ab(ctx, state[0], (size_t)state[1]);
}

// ---------------------------------------------------------------------------
// font registration

// registerFont(family, bytes, bold, italic) -> bool
static JSValue js_register_font(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this;
    const char *fam = JS_ToCString(ctx, argv[0]);
    size_t len;
    uint8_t *b = rt_get_bytes(ctx, argv[1], &len);
    bool ok = false;
    if (fam && b) {
        uint8_t *copy = malloc(len);
        memcpy(copy, b, len);
        Font *f = font_add(fam, argc > 2 && JS_ToBool(ctx, argv[2]), argc > 3 && JS_ToBool(ctx, argv[3]), copy, len);
        ok = f != NULL;
        if (!ok) free(copy);
        else if (!fallback_font) fallback_font = f;
    }
    JS_FreeCString(ctx, fam);
    return JS_NewBool(ctx, ok);
}

// setFallbackFont(family): generic families and missing glyphs use this font
static JSValue js_set_fallback(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    const char *fam = JS_ToCString(ctx, argv[0]);
    Font *f = fam ? font_find(fam, false, false) : NULL;
    if (f) fallback_font = f;
    JS_FreeCString(ctx, fam);
    return JS_NewBool(ctx, f != NULL);
}

static JSValue js_has_font(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    const char *fam = JS_ToCString(ctx, argv[0]);
    bool ok = fam && font_find(fam, false, false);
    JS_FreeCString(ctx, fam);
    return JS_NewBool(ctx, ok);
}

// ---------------------------------------------------------------------------

static const JSCFunctionListEntry surface_funcs[] = {
    JS_CGETSET_DEF("width", js_get_w, NULL),
    JS_CGETSET_DEF("height", js_get_h, NULL),
    JS_CFUNC_DEF("resize", 2, js_resize),
    JS_CFUNC_DEF("save", 0, js_save),
    JS_CFUNC_DEF("restore", 0, js_restore),
    JS_CFUNC_MAGIC_DEF("setTransform", 6, js_set_transform, 0),
    JS_CFUNC_MAGIC_DEF("transform", 6, js_set_transform, 1),
    JS_CFUNC_MAGIC_DEF("translate", 2, js_set_transform, 2),
    JS_CFUNC_MAGIC_DEF("scale", 2, js_set_transform, 3),
    JS_CFUNC_MAGIC_DEF("rotate", 1, js_set_transform, 4),
    JS_CFUNC_DEF("getTransform", 0, js_get_transform),
    JS_CFUNC_DEF("setAlpha", 1, js_set_alpha),
    JS_CFUNC_DEF("setOp", 1, js_set_op),
    JS_CFUNC_DEF("setLine", 4, js_set_line),
    JS_CFUNC_DEF("setFont", 4, js_set_font),
    JS_CFUNC_MAGIC_DEF("beginPath", 0, js_path, 0),
    JS_CFUNC_MAGIC_DEF("moveTo", 2, js_path, 1),
    JS_CFUNC_MAGIC_DEF("lineTo", 2, js_path, 2),
    JS_CFUNC_MAGIC_DEF("closePath", 0, js_path, 3),
    JS_CFUNC_MAGIC_DEF("rect", 4, js_path, 4),
    JS_CFUNC_MAGIC_DEF("arc", 6, js_path, 5),
    JS_CFUNC_MAGIC_DEF("bezierCurveTo", 6, js_path, 6),
    JS_CFUNC_MAGIC_DEF("quadraticCurveTo", 4, js_path, 7),
    JS_CFUNC_MAGIC_DEF("fill", 2, js_fill, 0),
    JS_CFUNC_MAGIC_DEF("stroke", 1, js_fill, 1),
    JS_CFUNC_MAGIC_DEF("clip", 1, js_fill, 2),
    JS_CFUNC_MAGIC_DEF("fillRect", 5, js_rect_op, 0),
    JS_CFUNC_MAGIC_DEF("strokeRect", 5, js_rect_op, 1),
    JS_CFUNC_MAGIC_DEF("clearRect", 4, js_rect_op, 2),
    JS_CFUNC_DEF("drawImage", 9, js_draw_image),
    JS_CFUNC_DEF("measureText", 1, js_measure_text),
    JS_CFUNC_DEF("drawText", 8, js_draw_text),
    JS_CFUNC_DEF("getImageData", 4, js_get_image_data),
    JS_CFUNC_DEF("putImageData", 5, js_put_image_data),
    JS_CFUNC_DEF("encodePNG", 0, js_encode_png),
    JS_CFUNC_DEF("boxBlur", 1, js_box_blur),
};

void rt_init_canvas(JSContext *ctx, JSValueConst ns) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &surface_class);
    JS_NewClass(rt, surface_class, &(JSClassDef){.class_name = "NativeSurface", .finalizer = surface_finalizer});
    JS_NewClassID(rt, &gradient_class);
    JS_NewClass(rt, gradient_class, &(JSClassDef){.class_name = "NativeGradient", .finalizer = gradient_finalizer});

    surface_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, surface_proto, surface_funcs, sizeof surface_funcs / sizeof surface_funcs[0]);
    JS_SetClassProto(ctx, surface_class, JS_DupValue(ctx, surface_proto));

    JSValue cv = JS_NewObject(ctx);
    rt_set_func(ctx, cv, "create", js_create, 2);
    rt_set_func(ctx, cv, "decodeImage", js_decode_image, 1);
    rt_set_func(ctx, cv, "decodeImageAsync", js_decode_image_async, 2);
    rt_set_func(ctx, cv, "linearGradient", js_linear_gradient, 5);
    rt_set_func(ctx, cv, "radialGradient", js_radial_gradient, 7);
    rt_set_func(ctx, cv, "registerFont", js_register_font, 4);
    rt_set_func(ctx, cv, "setFallbackFont", js_set_fallback, 1);
    rt_set_func(ctx, cv, "hasFont", js_has_font, 1);
    JS_SetPropertyStr(ctx, cv, "surfaceProto", JS_DupValue(ctx, surface_proto));
    JS_SetPropertyStr(ctx, ns, "canvas", cv);
}
