// Core natives: virtual filesystem, script loading, event queue, misc helpers.
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <SDL.h>
#include <zlib.h>
#include "rt.h"

// ---------------------------------------------------------------------------
// logging

static FILE *log_file;

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (log_file) {
        va_start(ap, fmt);
        vfprintf(log_file, fmt, ap);
        va_end(ap);
        fputc('\n', log_file);
        fflush(log_file);
    }
}

void rt_dump_exception(JSContext *ctx) {
    JSValue exc = JS_GetException(ctx);
    const char *msg = JS_ToCString(ctx, exc);
    rt_log("JS exception: %s", msg ? msg : "(unknown)");
    JS_FreeCString(ctx, msg);
    if (JS_IsObject(exc)) {
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (JS_IsString(stack)) {
            const char *s = JS_ToCString(ctx, stack);
            rt_log("%s", s);
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, exc);
}

// ---------------------------------------------------------------------------
// virtual filesystem

static char game_root[512], save_root[512];

void rt_fs_set_roots(const char *g, const char *s) {
    snprintf(game_root, sizeof game_root, "%s", g);
    snprintf(save_root, sizeof save_root, "%s", s);
    char logpath[600];
    snprintf(logpath, sizeof logpath, "%s/runtime.log", save_root);
    mkdir(save_root, 0777);
    log_file = fopen(logpath, "w");
}

// Normalise "\\game\\www\\..", "data/x", "/save/a/../b" into a host path.
bool rt_fs_resolve(const char *vpath, char *out, size_t outsz) {
    char buf[1024];
    size_t n = strlen(vpath);
    if (n >= sizeof buf - 8) return false;
    // strip a "file://" prefix some code paths produce
    if (!strncmp(vpath, "file://", 7)) vpath += 7;
    // the game believes it lives on a Windows drive: "C:\\game\\..." == "/game/..."
    if (((vpath[0] | 32) >= 'a' && (vpath[0] | 32) <= 'z') && vpath[1] == ':') vpath += 2;
    // relative paths are relative to /game
    const char *prefix = (vpath[0] == '/' || vpath[0] == '\\') ? "" : "/game/";
    snprintf(buf, sizeof buf, "%s%s", prefix, vpath);
    for (char *p = buf; *p; p++)
        if (*p == '\\') *p = '/';

    // split into segments, resolving "." and ".."
    char *segs[128];
    int ns = 0;
    for (char *tok = strtok(buf, "/"); tok; tok = strtok(NULL, "/")) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) {
            if (ns > 1) ns--;
            continue;
        }
        if (ns < 128) segs[ns++] = tok;
    }
    if (ns == 0) return false;
    const char *root;
    if (!strcmp(segs[0], "game")) root = game_root;
    else if (!strcmp(segs[0], "save")) root = save_root;
    else return false;

    size_t pos = (size_t)snprintf(out, outsz, "%s", root);
    for (int i = 1; i < ns && pos < outsz; i++)
        pos += (size_t)snprintf(out + pos, outsz - pos, "/%s", segs[i]);
    return pos < outsz;
}

uint8_t *rt_fs_read(const char *vpath, size_t *len) {
    char path[1100];
    if (!rt_fs_resolve(vpath, path, sizeof path)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)sz + 1);
    if (data && fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) {
        data[sz] = 0;
        if (len) *len = (size_t)sz;
    }
    return data;
}

static bool write_file(const char *vpath, const void *data, size_t len) {
    char path[1100], tmp[1110];
    if (!rt_fs_resolve(vpath, path, sizeof path)) return false;
    if (strncmp(path, save_root, strlen(save_root))) return false;  // only /save is writable
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// helpers

void *rt_free_ab(JSRuntime *rt, void *opaque, void *ptr, size_t size) {
    (void)rt; (void)opaque;
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

JSValue rt_new_ab(JSContext *ctx, uint8_t *buf, size_t len) {
    return JS_NewArrayBuffer(ctx, buf, len, 0, rt_free_ab, NULL, false);
}

uint8_t *rt_get_bytes(JSContext *ctx, JSValueConst v, size_t *len) {
    uint8_t *p = JS_GetArrayBuffer(ctx, len, v);
    if (p) return p;
    JS_FreeValue(ctx, JS_GetException(ctx));
    size_t off, bpe;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, len, &bpe);
    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        // DataView
        JSValue buf = JS_GetPropertyStr(ctx, v, "buffer");
        if (!JS_IsObject(buf)) { JS_FreeValue(ctx, buf); return NULL; }
        size_t total;
        p = JS_GetArrayBuffer(ctx, &total, buf);
        int32_t o = rt_arg_i(ctx, JS_GetPropertyStr(ctx, v, "byteOffset"));
        int32_t l = rt_arg_i(ctx, JS_GetPropertyStr(ctx, v, "byteLength"));
        JS_FreeValue(ctx, buf);
        if (!p) { JS_FreeValue(ctx, JS_GetException(ctx)); return NULL; }
        *len = (size_t)l;
        return p + o;
    }
    size_t total;
    p = JS_GetArrayBuffer(ctx, &total, ab);
    JS_FreeValue(ctx, ab);  // the view keeps the buffer alive
    return p ? p + off : NULL;
}

void rt_set_func(JSContext *ctx, JSValueConst obj, const char *name, JSCFunction *fn, int argc) {
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, argc));
}

// ---------------------------------------------------------------------------
// scripts

JSValue rt_eval_file(JSContext *ctx, const char *vpath) {
    char bcpath[1100];
    snprintf(bcpath, sizeof bcpath, "%sbc", vpath);
    size_t len;
    uint8_t *bc = rt_fs_read(bcpath, &len);
    if (bc) {
        JSValue fn = JS_ReadObject(ctx, bc, len, JS_READ_OBJ_BYTECODE);
        free(bc);
        if (JS_IsException(fn)) return fn;
        return JS_EvalFunction(ctx, fn);
    }
    uint8_t *src = rt_fs_read(vpath, &len);
    if (!src) return JS_ThrowReferenceError(ctx, "script not found: %s", vpath);
    JSValue r = JS_Eval(ctx, (const char *)src, len, vpath, JS_EVAL_TYPE_GLOBAL);
    free(src);
    return r;
}

// ---------------------------------------------------------------------------
// cross-thread event queue

typedef struct Event {
    rt_event_fn fn;
    void *data;
    struct Event *next;
} Event;

static SDL_mutex *ev_lock;
static Event *ev_head, *ev_tail;

void rt_post(rt_event_fn fn, void *data) {
    Event *e = malloc(sizeof *e);
    e->fn = fn;
    e->data = data;
    e->next = NULL;
    SDL_LockMutex(ev_lock);
    if (ev_tail) ev_tail->next = e;
    else ev_head = e;
    ev_tail = e;
    SDL_UnlockMutex(ev_lock);
}

void rt_drain_events(JSContext *ctx) {
    SDL_LockMutex(ev_lock);
    Event *e = ev_head;
    ev_head = ev_tail = NULL;
    SDL_UnlockMutex(ev_lock);
    while (e) {
        Event *next = e->next;
        e->fn(ctx, e->data);
        free(e);
        e = next;
    }
}

// ---------------------------------------------------------------------------
// sha256 (for Utils.hashString / crypto.createHash)

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha256_block(h, data + i);
    uint8_t tail[128] = {0};
    size_t rem = len - i;
    memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    size_t tl = rem + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) tail[tl - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha256_block(h, tail);
    if (tl == 128) sha256_block(h, tail + 64);
    for (int k = 0; k < 8; k++) {
        out[k * 4] = (uint8_t)(h[k] >> 24);
        out[k * 4 + 1] = (uint8_t)(h[k] >> 16);
        out[k * 4 + 2] = (uint8_t)(h[k] >> 8);
        out[k * 4 + 3] = (uint8_t)h[k];
    }
}

// ---------------------------------------------------------------------------
// JS bindings

static JSValue js_log(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (s) rt_log("%s", s);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue js_now(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc; (void)argv;
    static uint64_t t0;
    uint64_t c = SDL_GetPerformanceCounter();
    if (!t0) t0 = c;
    return JS_NewFloat64(ctx, (double)(c - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
}

static JSValue js_read_file(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    const char *p = JS_ToCString(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    size_t len;
    uint8_t *data = rt_fs_read(p, &len);
    JS_FreeCString(ctx, p);
    if (!data) return JS_NULL;
    return rt_new_ab(ctx, data, len);
}

static JSValue js_read_text(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    const char *p = JS_ToCString(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    size_t len;
    uint8_t *data = rt_fs_read(p, &len);
    JS_FreeCString(ctx, p);
    if (!data) return JS_NULL;
    size_t skip = (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) ? 3 : 0;
    JSValue s = JS_NewStringLen(ctx, (const char *)data + skip, len - skip);
    free(data);
    return s;
}

static JSValue js_write_file(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    const char *p = JS_ToCString(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    bool ok;
    if (JS_IsString(argv[1])) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
        ok = write_file(p, s, len);
        JS_FreeCString(ctx, s);
    } else {
        size_t len;
        uint8_t *b = rt_get_bytes(ctx, argv[1], &len);
        ok = b && write_file(p, b, len);
    }
    if (!ok) rt_log("writeFile failed: %s", p);
    JS_FreeCString(ctx, p);
    return JS_NewBool(ctx, ok);
}

static JSValue js_stat(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    const char *p = JS_ToCString(ctx, argv[0]);
    char path[1100];
    struct stat st;
    bool ok = p && rt_fs_resolve(p, path, sizeof path) && stat(path, &st) == 0;
    JS_FreeCString(ctx, p);
    if (!ok) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "size", JS_NewFloat64(ctx, (double)st.st_size));
    JS_SetPropertyStr(ctx, o, "dir", JS_NewBool(ctx, S_ISDIR(st.st_mode)));
    JS_SetPropertyStr(ctx, o, "mtime", JS_NewFloat64(ctx, (double)st.st_mtime * 1000.0));
    return o;
}

static JSValue js_readdir(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    const char *p = JS_ToCString(ctx, argv[0]);
    char path[1100];
    bool ok = p && rt_fs_resolve(p, path, sizeof path);
    JS_FreeCString(ctx, p);
    DIR *d = ok ? opendir(path) : NULL;
    if (!d) return JS_NULL;
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, e->d_name));
    }
    closedir(d);
    return arr;
}

static JSValue js_mkdir(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    const char *p = JS_ToCString(ctx, argv[0]);
    char path[1100];
    bool ok = p && rt_fs_resolve(p, path, sizeof path);
    JS_FreeCString(ctx, p);
    if (!ok) return JS_FALSE;
    // recursive
    for (char *c = path + 1; *c; c++) {
        if (*c == '/') {
            *c = 0;
            mkdir(path, 0777);
            *c = '/';
        }
    }
    return JS_NewBool(ctx, mkdir(path, 0777) == 0 || errno == EEXIST);
}

static JSValue js_unlink(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    const char *p = JS_ToCString(ctx, argv[0]);
    char path[1100];
    bool ok = p && rt_fs_resolve(p, path, sizeof path) && !strncmp(path, save_root, strlen(save_root)) && remove(path) == 0;
    JS_FreeCString(ctx, p);
    return JS_NewBool(ctx, ok);
}

static JSValue js_rename(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    const char *a = JS_ToCString(ctx, argv[0]), *b = JS_ToCString(ctx, argv[1]);
    char pa[1100], pb[1100];
    bool ok = a && b && rt_fs_resolve(a, pa, sizeof pa) && rt_fs_resolve(b, pb, sizeof pb);
    if (ok) {
        remove(pb);
        ok = rename(pa, pb) == 0;
    }
    JS_FreeCString(ctx, a);
    JS_FreeCString(ctx, b);
    return JS_NewBool(ctx, ok);
}

static JSValue js_eval_script(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    const char *p = JS_ToCString(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    JSValue r = rt_eval_file(ctx, p);
    JS_FreeCString(ctx, p);
    return r;
}

static JSValue js_eval_source(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    size_t len;
    const char *src = JS_ToCStringLen(ctx, &len, argv[0]);
    const char *name = JS_ToCString(ctx, argv[1]);
    JSValue r = JS_Eval(ctx, src, len, name ? name : "<eval>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(ctx, src);
    JS_FreeCString(ctx, name);
    return r;
}

static JSValue js_sha256(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    uint8_t out[32];
    if (JS_IsString(argv[0])) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
        sha256((const uint8_t *)s, len, out);
        JS_FreeCString(ctx, s);
    } else {
        size_t len = 0;
        uint8_t *b = rt_get_bytes(ctx, argv[0], &len);
        sha256(b ? b : (const uint8_t *)"", b ? len : 0, out);
    }
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", out[i]);
    return JS_NewStringLen(ctx, hex, 64);
}

static JSValue js_inflate(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    size_t len;
    uint8_t *in = rt_get_bytes(ctx, argv[0], &len);
    if (!in) return JS_ThrowTypeError(ctx, "inflate: expected bytes");
    z_stream zs = {0};
    if (inflateInit(&zs) != Z_OK) return JS_ThrowInternalError(ctx, "inflateInit");
    size_t cap = len * 4 + 1024, outlen = 0;
    uint8_t *out = malloc(cap);
    zs.next_in = in;
    zs.avail_in = (uInt)len;
    int ret;
    do {
        if (outlen == cap) out = realloc(out, cap *= 2);
        zs.next_out = out + outlen;
        zs.avail_out = (uInt)(cap - outlen);
        ret = inflate(&zs, Z_NO_FLUSH);
        outlen = cap - zs.avail_out;
    } while (ret == Z_OK);
    inflateEnd(&zs);
    if (ret != Z_STREAM_END) {
        free(out);
        return JS_ThrowInternalError(ctx, "inflate failed (%d)", ret);
    }
    return rt_new_ab(ctx, out, outlen);
}

// TextDecoder fast path: utf8Decode(bytes) -> string
static JSValue js_utf8_decode(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    size_t len;
    uint8_t *b = rt_get_bytes(ctx, argv[0], &len);
    if (!b) return JS_NewString(ctx, "");
    size_t skip = (len >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
    return JS_NewStringLen(ctx, (const char *)b + skip, len - skip);
}

// TextEncoder: utf8Encode(string) -> ArrayBuffer
static JSValue js_utf8_encode(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)s, len);
    JS_FreeCString(ctx, s);
    return ab;
}

static JSValue js_quit(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)ctx; (void)this; (void)argc; (void)argv;
    rt_request_quit();
    return JS_UNDEFINED;
}

static JSValue js_fullscreen(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    rt_set_fullscreen(JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_gc(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc; (void)argv;
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static JSValue js_mem(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc; (void)argv;
    JSMemoryUsage mu;
    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &mu);
    return JS_NewFloat64(ctx, (double)mu.malloc_size);
}

void rt_init_sys(JSContext *ctx, JSValueConst ns) {
    ev_lock = SDL_CreateMutex();
    JSValue sys = JS_NewObject(ctx);
    rt_set_func(ctx, sys, "log", js_log, 1);
    rt_set_func(ctx, sys, "now", js_now, 0);
    rt_set_func(ctx, sys, "readFile", js_read_file, 1);
    rt_set_func(ctx, sys, "readText", js_read_text, 1);
    rt_set_func(ctx, sys, "writeFile", js_write_file, 2);
    rt_set_func(ctx, sys, "stat", js_stat, 1);
    rt_set_func(ctx, sys, "readdir", js_readdir, 1);
    rt_set_func(ctx, sys, "mkdir", js_mkdir, 1);
    rt_set_func(ctx, sys, "unlink", js_unlink, 1);
    rt_set_func(ctx, sys, "rename", js_rename, 2);
    rt_set_func(ctx, sys, "evalScript", js_eval_script, 1);
    rt_set_func(ctx, sys, "evalSource", js_eval_source, 2);
    rt_set_func(ctx, sys, "sha256", js_sha256, 1);
    rt_set_func(ctx, sys, "inflate", js_inflate, 1);
    rt_set_func(ctx, sys, "utf8Decode", js_utf8_decode, 1);
    rt_set_func(ctx, sys, "utf8Encode", js_utf8_encode, 1);
    rt_set_func(ctx, sys, "quit", js_quit, 0);
    rt_set_func(ctx, sys, "setFullscreen", js_fullscreen, 1);
    rt_set_func(ctx, sys, "gc", js_gc, 0);
    rt_set_func(ctx, sys, "memoryUsage", js_mem, 0);
#ifdef __SWITCH__
    JS_SetPropertyStr(ctx, sys, "platform", JS_NewString(ctx, "switch"));
#else
    JS_SetPropertyStr(ctx, sys, "platform", JS_NewString(ctx, "host"));
#endif
    JS_SetPropertyStr(ctx, ns, "sys", sys);
}
