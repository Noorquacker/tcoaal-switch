// Minimal Web Audio engine: buffer sources, gain, (equal-power) panner and a
// master destination, mixed on the SDL audio thread. Plus threaded Ogg Vorbis
// decoding that replaces the game's asm.js decoder.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "rt.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#define OUT_RATE 48000
#define BLOCK 512

// ---------------------------------------------------------------------------
// audio buffers

typedef struct ABuf {
    int refs;
    int channels, length, rate;
    float *data[2];
} ABuf;

static SDL_mutex *lock;
static SDL_AudioDeviceID dev;
static uint64_t frames_done;  // output frames rendered (guarded by lock)

static ABuf *abuf_new(int ch, int len, int rate) {
    ABuf *b = calloc(1, sizeof *b);
    b->refs = 1;
    b->channels = ch < 1 ? 1 : ch > 2 ? 2 : ch;
    b->length = len;
    b->rate = rate;
    for (int i = 0; i < b->channels; i++) b->data[i] = calloc((size_t)(len ? len : 1), sizeof(float));
    return b;
}

static void abuf_unref(ABuf *b) {
    if (b && --b->refs == 0) {
        for (int i = 0; i < b->channels; i++) free(b->data[i]);
        free(b);
    }
}

// ---------------------------------------------------------------------------
// params with automation

enum { EV_SET, EV_LINEAR, EV_EXP, EV_TARGET };
typedef struct { int type; double time, value, tc; } PEvent;

#define MAX_EV 16
typedef struct {
    double value;
    PEvent ev[MAX_EV];
    int nev;
} Param;

static void param_insert(Param *p, PEvent e) {
    // keep sorted by time, drop the oldest when full
    if (p->nev == MAX_EV) {
        memmove(p->ev, p->ev + 1, sizeof(PEvent) * (MAX_EV - 1));
        p->nev--;
    }
    int i = p->nev;
    while (i > 0 && p->ev[i - 1].time > e.time) {
        p->ev[i] = p->ev[i - 1];
        i--;
    }
    p->ev[i] = e;
    p->nev++;
}

// Value at time t; consumes events that are fully in the past.
static double param_eval(Param *p, double t) {
    while (p->nev > 0) {
        PEvent *e = &p->ev[0];
        if (e->time > t) return p->value;  // ramps towards it are handled in aparam_eval
        if (e->type == EV_TARGET) {
            // setTargetAtTime: exponential approach, never "completes"
            double v = e->value + (p->value - e->value) * exp(-(t - e->time) / (e->tc > 0 ? e->tc : 1e-3));
            if (p->nev > 1 && p->ev[1].time <= t) {
                p->value = v;
                memmove(p->ev, p->ev + 1, sizeof(PEvent) * (size_t)(--p->nev));
                continue;
            }
            return v;
        }
        p->value = e->value;
        memmove(p->ev, p->ev + 1, sizeof(PEvent) * (size_t)(--p->nev));
    }
    return p->value;
}

// Ramps need the start point: we store it in `ramp_t0/ramp_v0` when the ramp
// event becomes the next pending event.
typedef struct {
    Param p;
    double ramp_t0, ramp_v0;
    bool ramp_armed;
} AParam;

static double aparam_eval(AParam *a, double t) {
    double v = param_eval(&a->p, t);
    if (a->p.nev > 0) {
        PEvent *e = &a->p.ev[0];
        if ((e->type == EV_LINEAR || e->type == EV_EXP) && e->time > t) {
            if (!a->ramp_armed) {
                a->ramp_armed = true;
                a->ramp_t0 = t;
                a->ramp_v0 = v;
            }
            double k = (t - a->ramp_t0) / (e->time - a->ramp_t0);
            if (k < 0) k = 0;
            if (e->type == EV_LINEAR || a->ramp_v0 <= 0 || e->value <= 0)
                return a->ramp_v0 + (e->value - a->ramp_v0) * k;
            return a->ramp_v0 * pow(e->value / a->ramp_v0, k);
        }
    }
    a->ramp_armed = false;
    return v;
}

static void aparam_set_now(AParam *a, double v) {
    a->p.value = v;
    a->p.nev = 0;
    a->ramp_armed = false;
}

// ---------------------------------------------------------------------------
// nodes

enum { N_DEST, N_GAIN, N_PANNER, N_SOURCE };
enum { S_IDLE, S_SCHEDULED, S_PLAYING, S_DONE };

typedef struct Node {
    int type;
    int refs;            // JS wrapper + incoming connections + active list
    int id;              // JS-side id for onended
    struct Node *out;
    AParam gain;         // gain node: gain; source: playbackRate
    double px, py, pz;   // panner position
    // source
    ABuf *buf;
    int state;
    double when, offset, duration, stop_at;
    double pos;          // in source frames
    double played;       // source seconds consumed (for duration)
    bool loop;
    double loop_start, loop_end;
    bool notify;
    struct Node *next_active;
} Node;

static JSClassID node_class, abuf_class;
static Node *active;  // playing/scheduled sources
static Node *dest;

static void node_unref(Node *n);

static void node_release_out(Node *n) {
    if (n->out) {
        Node *o = n->out;
        n->out = NULL;
        node_unref(o);
    }
}

static void node_unref(Node *n) {
    if (!n || --n->refs > 0) return;
    node_release_out(n);
    abuf_unref(n->buf);
    free(n);
}

static Node *node_new(int type) {
    Node *n = calloc(1, sizeof *n);
    n->type = type;
    n->refs = 1;
    aparam_set_now(&n->gain, 1.0);
    n->pz = 1;
    n->stop_at = -1;
    n->duration = -1;
    return n;
}

// ---------------------------------------------------------------------------
// mixing

static double now_locked(void) { return (double)frames_done / OUT_RATE; }

static void ended_event(JSContext *ctx, void *data) {
    int id = (int)(intptr_t)data;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, g, "__audioEnded");
    if (JS_IsFunction(ctx, fn)) {
        JSValue arg = JS_NewInt32(ctx, id);
        JSValue r = JS_Call(ctx, fn, g, 1, &arg);
        if (JS_IsException(r)) rt_dump_exception(ctx);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, g);
}

// Renders one source into tmp (stereo interleaved, BLOCK frames). Returns false when it finished.
static bool render_source(Node *n, float *tmp, int nframes, double t0) {
    ABuf *b = n->buf;
    memset(tmp, 0, sizeof(float) * 2 * (size_t)nframes);
    if (!b || b->length == 0) return false;
    double step0 = (double)b->rate / OUT_RATE;
    int i = 0;
    if (n->state == S_SCHEDULED) {
        double start_frame = (n->when - t0) * OUT_RATE;
        if (start_frame >= nframes) return true;
        if (start_frame > 0) i = (int)start_frame;
        n->state = S_PLAYING;
        n->pos = n->offset * b->rate;
    }
    double lend = n->loop_end > 0 ? n->loop_end * b->rate : b->length;
    double lstart = n->loop_start * b->rate;
    if (lend > b->length) lend = b->length;
    if (lstart >= lend) lstart = 0;
    double rate = aparam_eval(&n->gain, t0);
    double step = step0 * rate;
    const float *chl = b->data[0], *chr = b->data[b->channels > 1 ? 1 : 0];
    for (; i < nframes; i++) {
        if (n->stop_at >= 0 && t0 + (double)i / OUT_RATE >= n->stop_at) return false;
        if (n->duration >= 0 && n->played >= n->duration) return false;
        if (n->loop) {
            while (n->pos >= lend && lend > lstart) n->pos -= (lend - lstart);
        } else if (n->pos >= b->length) {
            return false;
        }
        int ip = (int)n->pos;
        float f = (float)(n->pos - ip);
        int ip1 = ip + 1 < b->length ? ip + 1 : ip;
        tmp[i * 2] = chl[ip] + (chl[ip1] - chl[ip]) * f;
        tmp[i * 2 + 1] = chr[ip] + (chr[ip1] - chr[ip]) * f;
        n->pos += step;
        n->played += step / b->rate;
    }
    return true;
}

// Applies gain/panner chain; returns false if the chain doesn't reach the destination.
static bool apply_chain(Node *n, float *tmp, int nframes, double t0, double t1) {
    for (Node *c = n->out; c; c = c->out) {
        if (c->type == N_DEST) return true;
        if (c->type == N_GAIN) {
            double g0 = aparam_eval(&c->gain, t0), g1 = aparam_eval(&c->gain, t1);
            for (int i = 0; i < nframes; i++) {
                float g = (float)(g0 + (g1 - g0) * i / nframes);
                tmp[i * 2] *= g;
                tmp[i * 2 + 1] *= g;
            }
        } else if (c->type == N_PANNER) {
            // Web Audio equal-power panning for stereo input
            double az = atan2(c->px, c->pz) * 180.0 / M_PI;
            if (az > 90) az = 180 - az;
            if (az < -90) az = -180 - az;
            double x = az <= 0 ? (az + 90) / 90 : az / 90;
            float gl = (float)cos(x * M_PI / 2), gr = (float)sin(x * M_PI / 2);
            for (int i = 0; i < nframes; i++) {
                float l = tmp[i * 2], r = tmp[i * 2 + 1];
                if (az <= 0) {
                    tmp[i * 2] = l + r * gl;
                    tmp[i * 2 + 1] = r * gr;
                } else {
                    tmp[i * 2] = l * gl;
                    tmp[i * 2 + 1] = r + l * gr;
                }
            }
        }
    }
    return false;
}

static void audio_callback(void *ud, Uint8 *stream, int bytes) {
    (void)ud;
    float *out = (float *)stream;
    int total = bytes / (int)(sizeof(float) * 2);
    memset(out, 0, (size_t)bytes);
    static float tmp[BLOCK * 2];
    SDL_LockMutex(lock);
    for (int done = 0; done < total;) {
        int nf = total - done > BLOCK ? BLOCK : total - done;
        double t0 = now_locked(), t1 = t0 + (double)nf / OUT_RATE;
        Node **pp = &active;
        while (*pp) {
            Node *n = *pp;
            bool alive = n->state != S_DONE && render_source(n, tmp, nf, t0);
            if (apply_chain(n, tmp, nf, t0, t1))
                for (int i = 0; i < nf * 2; i++) out[done * 2 + i] += tmp[i];
            if (!alive) {
                n->state = S_DONE;
                *pp = n->next_active;
                if (n->notify) rt_post(ended_event, (void *)(intptr_t)n->id);
                node_unref(n);
                continue;
            }
            pp = &n->next_active;
        }
        if (dest) {
            double g0 = aparam_eval(&dest->gain, t0);
            for (int i = 0; i < nf * 2; i++) out[done * 2 + i] *= (float)g0;
        }
        frames_done += (uint64_t)nf;
        done += nf;
    }
    SDL_UnlockMutex(lock);
    for (int i = 0; i < total * 2; i++) {
        if (out[i] > 1) out[i] = 1;
        else if (out[i] < -1) out[i] = -1;
    }
}

bool rt_audio_open(void) {
    lock = SDL_CreateMutex();
    dest = node_new(N_DEST);
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) {
        rt_log("audio: %s", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(dev, 0);
    return true;
}

void rt_audio_shutdown(void) {
    if (dev) SDL_CloseAudioDevice(dev);
}

// ---------------------------------------------------------------------------
// JS: nodes

static void node_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    Node *n = JS_GetOpaque(val, node_class);
    if (!n) return;
    SDL_LockMutex(lock);
    n->notify = false;
    node_unref(n);
    SDL_UnlockMutex(lock);
}

static JSValue wrap_node(JSContext *ctx, Node *n) {
    JSValue o = JS_NewObjectClass(ctx, (int)node_class);
    JS_SetOpaque(o, n);
    return o;
}

static Node *N(JSContext *ctx, JSValueConst v) { return JS_GetOpaque2(ctx, v, node_class); }

// createNode(type, id) — type: 1 gain 2 panner 3 source
static JSValue js_create_node(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    Node *n = node_new(rt_arg_i(ctx, argv[0]));
    n->id = rt_arg_i(ctx, argv[1]);
    return wrap_node(ctx, n);
}

static JSValue js_destination(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc; (void)argv;
    SDL_LockMutex(lock);
    dest->refs++;
    SDL_UnlockMutex(lock);
    return wrap_node(ctx, dest);
}

// connect(src, dst) / disconnect(src)
static JSValue js_connect(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this;
    Node *a = N(ctx, argv[0]);
    Node *b = argc > 1 && !JS_IsUndefined(argv[1]) ? N(ctx, argv[1]) : NULL;
    if (!a) return JS_EXCEPTION;
    SDL_LockMutex(lock);
    node_release_out(a);
    if (b) {
        b->refs++;
        a->out = b;
    }
    SDL_UnlockMutex(lock);
    return JS_UNDEFINED;
}

// param(node, op, value, time, extra) — op: 0 set-now 1 setValueAtTime 2 linearRamp 3 expRamp 4 setTarget 5 cancel; returns current value for op -1
static JSValue js_param(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    Node *n = N(ctx, argv[0]);
    if (!n) return JS_EXCEPTION;
    int op = rt_arg_i(ctx, argv[1]);
    double v = rt_arg_f(ctx, argv[2]), t = rt_arg_f(ctx, argv[3]), x = rt_arg_f(ctx, argv[4]);
    SDL_LockMutex(lock);
    double ret = 0;
    switch (op) {
    case -1: ret = aparam_eval(&n->gain, now_locked()); break;
    case 0: aparam_set_now(&n->gain, v); break;
    case 1: param_insert(&n->gain.p, (PEvent){EV_SET, t, v, 0}); break;
    case 2: param_insert(&n->gain.p, (PEvent){EV_LINEAR, t, v, 0}); break;
    case 3: param_insert(&n->gain.p, (PEvent){EV_EXP, t, v, 0}); break;
    case 4: param_insert(&n->gain.p, (PEvent){EV_TARGET, t, v, x}); break;
    case 5: {
        int k = 0;
        for (int i = 0; i < n->gain.p.nev; i++)
            if (n->gain.p.ev[i].time < t) n->gain.p.ev[k++] = n->gain.p.ev[i];
        n->gain.p.nev = k;
        break;
    }
    }
    SDL_UnlockMutex(lock);
    return JS_NewFloat64(ctx, ret);
}

static JSValue js_set_position(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    Node *n = N(ctx, argv[0]);
    if (!n) return JS_EXCEPTION;
    SDL_LockMutex(lock);
    n->px = rt_arg_f(ctx, argv[1]);
    n->py = rt_arg_f(ctx, argv[2]);
    n->pz = rt_arg_f(ctx, argv[3]);
    SDL_UnlockMutex(lock);
    return JS_UNDEFINED;
}

// setSource(node, buffer|null, loop, loopStart, loopEnd)
static JSValue js_set_source(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    Node *n = N(ctx, argv[0]);
    if (!n) return JS_EXCEPTION;
    ABuf *b = JS_IsNull(argv[1]) || JS_IsUndefined(argv[1]) ? NULL : JS_GetOpaque2(ctx, argv[1], abuf_class);
    SDL_LockMutex(lock);
    if (b != n->buf) {
        if (b) b->refs++;
        abuf_unref(n->buf);
        n->buf = b;
    }
    n->loop = JS_ToBool(ctx, argv[2]);
    n->loop_start = rt_arg_f(ctx, argv[3]);
    n->loop_end = rt_arg_f(ctx, argv[4]);
    SDL_UnlockMutex(lock);
    return JS_UNDEFINED;
}

// start(node, when, offset, duration|-1)
static JSValue js_start(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    Node *n = N(ctx, argv[0]);
    if (!n) return JS_EXCEPTION;
    SDL_LockMutex(lock);
    if (n->state == S_IDLE) {
        double now = now_locked();
        n->when = rt_arg_f(ctx, argv[1]);
        if (n->when < now) n->when = now;
        n->offset = rt_arg_f(ctx, argv[2]);
        if (n->offset < 0) n->offset = 0;
        n->duration = rt_arg_f(ctx, argv[3]);
        n->state = S_SCHEDULED;
        n->notify = true;
        n->refs++;
        n->next_active = active;
        active = n;
    }
    SDL_UnlockMutex(lock);
    return JS_UNDEFINED;
}

// stop(node, when)
static JSValue js_stop(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    Node *n = N(ctx, argv[0]);
    if (!n) return JS_EXCEPTION;
    SDL_LockMutex(lock);
    double now = now_locked(), w = rt_arg_f(ctx, argv[1]);
    n->stop_at = w < now ? now : w;
    SDL_UnlockMutex(lock);
    return JS_UNDEFINED;
}

static JSValue js_current_time(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc; (void)argv;
    SDL_LockMutex(lock);
    double t = now_locked();
    SDL_UnlockMutex(lock);
    return JS_NewFloat64(ctx, t);
}

// ---------------------------------------------------------------------------
// JS: buffers

static void abuf_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    ABuf *b = JS_GetOpaque(val, abuf_class);
    if (!b) return;
    SDL_LockMutex(lock);
    abuf_unref(b);
    SDL_UnlockMutex(lock);
}

static JSValue wrap_abuf(JSContext *ctx, ABuf *b) {
    JSValue o = JS_NewObjectClass(ctx, (int)abuf_class);
    JS_SetOpaque(o, b);
    return o;
}

static JSValue js_create_buffer(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    int ch = rt_arg_i(ctx, argv[0]), len = rt_arg_i(ctx, argv[1]), rate = rt_arg_i(ctx, argv[2]);
    if (ch < 1 || len < 0 || rate < 3000 || rate > 384000) return JS_ThrowRangeError(ctx, "createBuffer: bad args");
    return wrap_abuf(ctx, abuf_new(ch, len, rate));
}

// bufferInfo(buf) -> [channels, length, rate]
static JSValue js_buffer_info(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    ABuf *b = JS_GetOpaque2(ctx, argv[0], abuf_class);
    if (!b) return JS_EXCEPTION;
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, a, 0, JS_NewInt32(ctx, b->channels));
    JS_SetPropertyUint32(ctx, a, 1, JS_NewInt32(ctx, b->length));
    JS_SetPropertyUint32(ctx, a, 2, JS_NewInt32(ctx, b->rate));
    return a;
}

// copyToChannel(buf, Float32Array, ch, start)
static JSValue js_copy_to_channel(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    ABuf *b = JS_GetOpaque2(ctx, argv[0], abuf_class);
    if (!b) return JS_EXCEPTION;
    size_t len;
    float *src = (float *)rt_get_bytes(ctx, argv[1], &len);
    int ch = rt_arg_i(ctx, argv[2]), start = rt_arg_i(ctx, argv[3]);
    if (!src || ch < 0 || ch >= b->channels || start < 0) return JS_UNDEFINED;
    size_t n = len / sizeof(float);
    if (start + (int)n > b->length) n = (size_t)(b->length - start);
    SDL_LockMutex(lock);
    memcpy(b->data[ch] + start, src, n * sizeof(float));
    SDL_UnlockMutex(lock);
    return JS_UNDEFINED;
}

static void *abuf_view_free(JSRuntime *rt, void *opaque, void *ptr, size_t size) {
    (void)rt; (void)ptr;
    if (size) return NULL;  // not resizable
    SDL_LockMutex(lock);
    abuf_unref(opaque);
    SDL_UnlockMutex(lock);
    return NULL;
}

// channelData(buf, ch) -> ArrayBuffer aliasing the native samples
static JSValue js_channel_data(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    ABuf *b = JS_GetOpaque2(ctx, argv[0], abuf_class);
    if (!b) return JS_EXCEPTION;
    int ch = rt_arg_i(ctx, argv[1]);
    if (ch < 0 || ch >= b->channels) return JS_ThrowRangeError(ctx, "bad channel");
    SDL_LockMutex(lock);
    b->refs++;
    SDL_UnlockMutex(lock);
    return JS_NewArrayBuffer(ctx, (uint8_t *)b->data[ch], (size_t)b->length * sizeof(float), 0, abuf_view_free, b, false);
}

// ---------------------------------------------------------------------------
// Ogg Vorbis decoding on worker threads

typedef struct Chunk {
    int id;
    ABuf *buf;      // NULL with eof/err
    bool eof;
    char err[96];
} Chunk;

typedef struct {
    int id;
    uint8_t *data;
    size_t len;
    bool whole;     // decodeAudioData: one buffer instead of chunks
} DecodeJob;

static void chunk_event(JSContext *ctx, void *data) {
    Chunk *c = data;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, g, "__oggChunk");
    JSValue args[4] = {
        JS_NewInt32(ctx, c->id),
        c->buf ? wrap_abuf(ctx, c->buf) : JS_NULL,
        JS_NewBool(ctx, c->eof),
        c->err[0] ? JS_NewString(ctx, c->err) : JS_NULL,
    };
    if (JS_IsFunction(ctx, fn)) {
        JSValue r = JS_Call(ctx, fn, g, 4, args);
        if (JS_IsException(r)) rt_dump_exception(ctx);
        JS_FreeValue(ctx, r);
    } else if (c->buf) {
        abuf_unref(c->buf);  // nobody listening
    }
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, g);
    free(c);
}

static void post_chunk(int id, ABuf *b, bool eof, const char *err) {
    Chunk *c = calloc(1, sizeof *c);
    c->id = id;
    c->buf = b;
    c->eof = eof;
    if (err) snprintf(c->err, sizeof c->err, "%s", err);
    rt_post(chunk_event, c);
}

static int decode_thread(void *arg) {
    DecodeJob *job = arg;
    int error = 0;
    stb_vorbis *v = stb_vorbis_open_memory(job->data, (int)job->len, &error, NULL);
    if (!v) {
        char msg[64];
        snprintf(msg, sizeof msg, "vorbis open failed (%d)", error);
        post_chunk(job->id, NULL, true, msg);
        goto out;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    int ch = info.channels > 2 ? 2 : info.channels;
    if (job->whole) {
        int total = (int)stb_vorbis_stream_length_in_samples(v);
        ABuf *b = abuf_new(ch, total, (int)info.sample_rate);
        int got = 0;
        while (got < total) {
            float *ptrs[2] = {b->data[0] + got, ch > 1 ? b->data[1] + got : NULL};
            int n = stb_vorbis_get_samples_float(v, ch, ptrs, total - got);
            if (n <= 0) break;
            got += n;
        }
        b->length = got;
        post_chunk(job->id, b, false, NULL);
        post_chunk(job->id, NULL, true, NULL);
    } else {
        // small first chunk so playback can start quickly, then larger ones
        int chunk = (int)info.sample_rate / 4;
        for (;;) {
            ABuf *b = abuf_new(ch, chunk, (int)info.sample_rate);
            int got = 0;
            while (got < chunk) {
                float *ptrs[2] = {b->data[0] + got, ch > 1 ? b->data[1] + got : NULL};
                int n = stb_vorbis_get_samples_float(v, ch, ptrs, chunk - got);
                if (n <= 0) break;
                got += n;
            }
            if (got == 0) {
                abuf_unref(b);
                break;
            }
            b->length = got;
            post_chunk(job->id, b, false, NULL);
            if (got < chunk) break;
            chunk = (int)info.sample_rate * 2;
        }
        post_chunk(job->id, NULL, true, NULL);
    }
    stb_vorbis_close(v);
out:
    free(job->data);
    free(job);
    return 0;
}

// decodeOgg(bytes, id, whole)
static JSValue js_decode_ogg(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc;
    size_t len;
    uint8_t *p = rt_get_bytes(ctx, argv[0], &len);
    if (!p) return JS_ThrowTypeError(ctx, "decodeOgg: expected bytes");
    DecodeJob *job = calloc(1, sizeof *job);
    job->id = rt_arg_i(ctx, argv[1]);
    job->whole = JS_ToBool(ctx, argv[2]);
    job->data = malloc(len);
    memcpy(job->data, p, len);
    job->len = len;
    SDL_Thread *t = SDL_CreateThread(decode_thread, "ogg", job);
    if (t) SDL_DetachThread(t);
    else decode_thread(job);
    return JS_UNDEFINED;
}

void rt_init_audio(JSContext *ctx, JSValueConst ns) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &node_class);
    JS_NewClass(rt, node_class, &(JSClassDef){.class_name = "NativeAudioNode", .finalizer = node_finalizer});
    JS_NewClassID(rt, &abuf_class);
    JS_NewClass(rt, abuf_class, &(JSClassDef){.class_name = "NativeAudioBuffer", .finalizer = abuf_finalizer});
    JSValue a = JS_NewObject(ctx);
    rt_set_func(ctx, a, "createNode", js_create_node, 2);
    rt_set_func(ctx, a, "destination", js_destination, 0);
    rt_set_func(ctx, a, "connect", js_connect, 2);
    rt_set_func(ctx, a, "param", js_param, 5);
    rt_set_func(ctx, a, "setPosition", js_set_position, 4);
    rt_set_func(ctx, a, "setSource", js_set_source, 5);
    rt_set_func(ctx, a, "start", js_start, 4);
    rt_set_func(ctx, a, "stop", js_stop, 2);
    rt_set_func(ctx, a, "currentTime", js_current_time, 0);
    rt_set_func(ctx, a, "createBuffer", js_create_buffer, 3);
    rt_set_func(ctx, a, "bufferInfo", js_buffer_info, 1);
    rt_set_func(ctx, a, "copyToChannel", js_copy_to_channel, 4);
    rt_set_func(ctx, a, "channelData", js_channel_data, 2);
    rt_set_func(ctx, a, "decodeOgg", js_decode_ogg, 3);
    JS_SetPropertyStr(ctx, a, "sampleRate", JS_NewInt32(ctx, OUT_RATE));
    JS_SetPropertyStr(ctx, ns, "audio", a);
}
