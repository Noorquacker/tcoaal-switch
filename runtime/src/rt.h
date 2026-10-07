// Shared declarations for the TCOAAL QuickJS runtime.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "quickjs.h"

#define RT_W 1280
#define RT_H 720

extern JSRuntime *rt_runtime;
extern JSContext *rt_ctx;

// ---- logging -------------------------------------------------------------
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_dump_exception(JSContext *ctx);

// ---- virtual filesystem ----------------------------------------------------
// Virtual paths: "/game/..." is the read-only romfs, "/save/..." is writable
// storage. Backslashes are accepted, relative paths resolve under /game.
void rt_fs_set_roots(const char *game_root, const char *save_root);
bool rt_fs_resolve(const char *vpath, char *out, size_t outsz);
uint8_t *rt_fs_read(const char *vpath, size_t *len);  // malloc'd, NUL-terminated

// ---- scripts ---------------------------------------------------------------
// Evaluates a global script, preferring precompiled "<path>bc" bytecode.
JSValue rt_eval_file(JSContext *ctx, const char *vpath);

// ---- cross-thread events -------------------------------------------------
typedef void (*rt_event_fn)(JSContext *ctx, void *data);
void rt_post(rt_event_fn fn, void *data);  // thread-safe
void rt_drain_events(JSContext *ctx);

// ---- helpers -----------------------------------------------------------
static inline double rt_arg_f(JSContext *ctx, JSValueConst v) {
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    return d;
}
static inline int32_t rt_arg_i(JSContext *ctx, JSValueConst v) {
    int32_t i = 0;
    JS_ToInt32(ctx, &i, v);
    return i;
}
// Bytes of an ArrayBuffer or any typed array/DataView view.
uint8_t *rt_get_bytes(JSContext *ctx, JSValueConst v, size_t *len);
void rt_set_func(JSContext *ctx, JSValueConst obj, const char *name, JSCFunction *fn, int argc);
// realloc-style ArrayBuffer data callback for malloc'd buffers (size 0 frees)
void *rt_free_ab(JSRuntime *rt, void *opaque, void *ptr, size_t size);
// Wraps a malloc'd buffer in an ArrayBuffer that takes ownership.
JSValue rt_new_ab(JSContext *ctx, uint8_t *buf, size_t len);

// ---- modules ---------------------------------------------------------------
void rt_init_sys(JSContext *ctx, JSValueConst ns);
void rt_init_canvas(JSContext *ctx, JSValueConst ns);
void rt_init_gl(JSContext *ctx, JSValueConst ns);
void rt_init_audio(JSContext *ctx, JSValueConst ns);
void rt_init_input(JSContext *ctx, JSValueConst ns);

// canvas <-> gl: pixel access for texture uploads (premultiplied BGRA, plutovg layout)
bool rt_canvas_pixels(JSContext *ctx, JSValueConst obj, const uint8_t **data, int *w, int *h, int *stride);

// gl -> main loop: whether the default framebuffer was drawn since last call
bool rt_gl_take_dirty(void);

// audio
bool rt_audio_open(void);
void rt_audio_shutdown(void);

// input (implemented per platform in input.c)
void rt_input_handle_event(JSContext *ctx, const void *sdl_event);
void rt_input_update(void);

// main loop control
void rt_request_quit(void);
void rt_set_fullscreen(bool on);
