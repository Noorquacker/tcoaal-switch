// Entry point: SDL window + GLES2 context, QuickJS runtime, frame loop.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <GLES2/gl2.h>
#ifdef __SWITCH__
#include <switch.h>
#endif
#include "plutovg.h"
#include "rt.h"

JSRuntime *rt_runtime;
JSContext *rt_ctx;

static SDL_Window *window;
static bool quit_requested;

void rt_request_quit(void) { quit_requested = true; }

void rt_set_fullscreen(bool on) {
#ifdef __SWITCH__
    (void)on;
#else
    (void)on;  // the GL canvas is a fixed 1280x720 window on the host build
#endif
}

static void run_jobs(JSContext *ctx) {
    for (;;) {
        JSContext *c;
        int r = JS_ExecutePendingJob(rt_runtime, &c);
        if (r <= 0) {
            if (r < 0) rt_dump_exception(c);
            break;
        }
    }
    (void)ctx;
}

static bool call_global(JSContext *ctx, const char *name, double arg) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, g, name);
    bool ok = true;
    if (JS_IsFunction(ctx, fn)) {
        JSValue a = JS_NewFloat64(ctx, arg);
        JSValue r = JS_Call(ctx, fn, g, 1, &a);
        if (JS_IsException(r)) {
            rt_dump_exception(ctx);
            ok = false;
        }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, g);
    return ok;
}

// Debugging aids, from the environment (host) or <save>/debug.cfg (KEY=value lines):
//   TCOAAL_SHOTS="120:/tmp/a.png,600:/tmp/b.png"  screenshot after the given presented frames
//   TCOAAL_KEYS="300:13,420:27"                    tap a DOM keyCode at the given frames
//   TCOAAL_EXIT=900                                quit after N presented frames
//   TCOAAL_STATS=1                                 log frame timing every 5 seconds
static char debug_cfg[4096];

static void debug_load_cfg(const char *save_root) {
    char path[600];
    snprintf(path, sizeof path, "%s/debug.cfg", save_root);
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(debug_cfg, 1, sizeof debug_cfg - 1, f);
    debug_cfg[n] = 0;
    fclose(f);
    rt_log("debug.cfg loaded");
}

// Returns a stable string per call site use (values are copied out of debug.cfg).
// Values are copied out of debug.cfg, so each returned string stays valid.
static const char *debug_get(const char *key) {
    const char *v = getenv(key);
    if (v) return v;
    size_t kl = strlen(key);
    for (const char *line = debug_cfg; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            size_t n = (size_t)(end - line) - kl - 1;
            if (n && line[kl + n] == '\r') n--;
            char *val = malloc(n + 1);  // looked up once per key and kept for the session
            memcpy(val, line + kl + 1, n);
            val[n] = 0;
            return val;
        }
        line = *end ? end + 1 : end;
    }
    return NULL;
}

static void debug_screenshot(const char *path) {
    uint8_t *px = malloc((size_t)RT_W * RT_H * 4);
    glReadPixels(0, 0, RT_W, RT_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    plutovg_surface_t *s = plutovg_surface_create(RT_W, RT_H);
    uint8_t *d = plutovg_surface_get_data(s);
    int stride = plutovg_surface_get_stride(s);
    for (int y = 0; y < RT_H; y++) {
        const uint8_t *src = px + (size_t)(RT_H - 1 - y) * RT_W * 4;
        uint32_t *row = (uint32_t *)(d + (size_t)y * (size_t)stride);
        for (int x = 0; x < RT_W; x++)
            row[x] = 0xFF000000u | (uint32_t)src[x * 4] << 16 | (uint32_t)src[x * 4 + 1] << 8 | src[x * 4 + 2];
    }
    plutovg_surface_write_to_png(s, path);
    plutovg_surface_destroy(s);
    free(px);
    rt_log("screenshot: %s", path);
}

static void debug_frame(JSContext *ctx, long frame) {
    static bool init;
    static const char *shots, *keys, *ex;
    if (!init) {
        init = true;
        shots = debug_get("TCOAAL_SHOTS");
        keys = debug_get("TCOAAL_KEYS");
        ex = debug_get("TCOAAL_EXIT");
    }
    char buf[2048];
    if (shots) {
        snprintf(buf, sizeof buf, "%s", shots);
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
            char *c = strchr(t, ':');
            if (c && atol(t) == frame) debug_screenshot(c + 1);
        }
    }
    if (keys) {
        snprintf(buf, sizeof buf, "%s", keys);
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
            char *c = strchr(t, ':');
            if (!c) continue;
            long f = atol(t);
            const char *type = f == frame ? "keydown" : f + 6 == frame ? "keyup" : NULL;
            if (!type) continue;
            char js[128];
            snprintf(js, sizeof js, "__dispatchKey('%s', %d, false)", type, atoi(c + 1));
            JSValue r = JS_Eval(ctx, js, strlen(js), "<debug>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(r)) rt_dump_exception(ctx);
            JS_FreeValue(ctx, r);
        }
    }
    if (ex && frame >= atol(ex)) quit_requested = true;
}

// frame timing: total frame time and time spent in JS
static void stats_frame(double frame_ms, double js_ms) {
    static double acc_frame, acc_js, worst, t_last;
    static long n;
    acc_frame += frame_ms;
    acc_js += js_ms;
    if (frame_ms > worst) worst = frame_ms;
    n++;
    t_last += frame_ms;
    if (frame_ms > 50) rt_log("stats: slow frame %.1f ms (js %.1f ms)", frame_ms, js_ms);
    if (t_last >= 5000) {
        rt_log("stats: %.1f fps, frame %.2f ms avg / %.2f worst, js %.2f ms avg, heap %.1f MB",
               n * 1000.0 / t_last, acc_frame / n, worst, acc_js / n,
               (double)({ JSMemoryUsage mu; JS_ComputeMemoryUsage(rt_runtime, &mu); mu.malloc_size; }) / 1048576.0);
        acc_frame = acc_js = worst = t_last = 0;
        n = 0;
    }
}

#ifdef __SWITCH__
// Clocks. ApmCpuBoostMode_FastLoad raises the CPU to 1785 MHz but drops the GPU
// to 76.8 MHz, so set both explicitly: CPU 1785 MHz, GPU at the official maximum
// for the current mode (460.8 MHz handheld, 768 MHz docked). The OS resets clocks
// when the performance mode changes, so clocks_tick() re-applies them. Original
// rates are restored on exit. TCOAAL_CLOCKS=0 in debug.cfg leaves clocks alone.
enum { CLK_CPU, CLK_GPU, CLK_N };
static const PcvModuleId clk_mod[CLK_N] = {PcvModuleId_CpuBus, PcvModuleId_GPU};
static const PcvModule clk_pcv[CLK_N] = {PcvModule_CpuBus, PcvModule_GPU};  // pre-8.0.0
static ClkrstSession clk_sess[CLK_N];
static u32 clk_orig[CLK_N];
static bool clk_on, clk_rst;  // clk_rst: clkrst (8.0.0+) rather than pcv
static s32 clk_mode = -1;

static u32 clk_get(int i) {
    u32 hz = 0;
    if (clk_rst) clkrstGetClockRate(&clk_sess[i], &hz);
    else pcvGetClockRate(clk_pcv[i], &hz);
    return hz;
}
static void clk_set(int i, u32 hz) {
    if (clk_rst) clkrstSetClockRate(&clk_sess[i], hz);
    else pcvSetClockRate(clk_pcv[i], hz);
}

static void clocks_apply(void) {
    s32 mode = appletGetPerformanceMode();
    clk_set(CLK_CPU, 1785000000);
    clk_set(CLK_GPU, mode == ApmPerformanceMode_Boost ? 768000000 : 460800000);
    if (mode != clk_mode)
        rt_log("clocks: %s, cpu %u MHz, gpu %.1f MHz", mode == ApmPerformanceMode_Boost ? "docked" : "handheld",
               clk_get(CLK_CPU) / 1000000, clk_get(CLK_GPU) / 1e6);
    clk_mode = mode;
}

static void clocks_init(void) {
    const char *v = debug_get("TCOAAL_CLOCKS");
    if (v && atoi(v) == 0) return;
    clk_rst = hosversionAtLeast(8, 0, 0);
    if (clk_rst ? R_FAILED(clkrstInitialize()) : R_FAILED(pcvInitialize())) {
        rt_log("clocks: clkrst/pcv unavailable");
        return;
    }
    for (int i = 0; i < CLK_N; i++)
        if (clk_rst) clkrstOpenSession(&clk_sess[i], clk_mod[i], 3);
    for (int i = 0; i < CLK_N; i++) clk_orig[i] = clk_get(i);
    clk_on = true;
    clocks_apply();
}

static void clocks_tick(long frame) {
    if (clk_on && frame % 60 == 0 && appletGetPerformanceMode() != clk_mode) clocks_apply();
}

static void clocks_exit(void) {
    if (!clk_on) return;
    for (int i = 0; i < CLK_N; i++) {
        if (clk_orig[i]) clk_set(i, clk_orig[i]);
        if (clk_rst) clkrstCloseSession(&clk_sess[i]);
    }
    if (clk_rst) clkrstExit();
    else pcvExit();
}
#else
static void clocks_init(void) {}
static void clocks_tick(long frame) { (void)frame; }
static void clocks_exit(void) {}
#endif

static void host_paths(int argc, char **argv, char *game, size_t gsz, char *save, size_t ssz) {
#ifdef __SWITCH__
    (void)argc; (void)argv;
    snprintf(game, gsz, "romfs:");
    snprintf(save, ssz, "sdmc:/switch/tcoaal");
#else
    snprintf(game, gsz, "%s", argc > 1 ? argv[1] : "romfs");
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (argc > 2) snprintf(save, ssz, "%s", argv[2]);
    else if (xdg) snprintf(save, ssz, "%s/tcoaal-switch", xdg);
    else snprintf(save, ssz, "%s/.local/share/tcoaal-switch", home ? home : ".");
#endif
}

int main(int argc, char **argv) {
#ifdef __SWITCH__
    romfsInit();
    socketInitializeDefault();
    nxlinkStdio();
#endif
    char game[512], save[512];
    host_paths(argc, argv, game, sizeof game, save, sizeof save);
    rt_fs_set_roots(game, save);
    rt_log("game root: %s, save root: %s", game, save);
    debug_load_cfg(save);
    clocks_init();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) < 0) {
        rt_log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    window = SDL_CreateWindow("The Coffin of Andy and Leyley", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              RT_W, RT_H, SDL_WINDOW_OPENGL);
    if (!window) {
        rt_log("SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    SDL_GLContext glc = SDL_GL_CreateContext(window);
    if (!glc) {
        rt_log("SDL_GL_CreateContext: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    rt_log("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    rt_audio_open();

    rt_runtime = JS_NewRuntime();
    JS_SetMaxStackSize(rt_runtime, 4 * 1024 * 1024);
    rt_ctx = JS_NewContext(rt_runtime);
    JSContext *ctx = rt_ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ns = JS_NewObject(ctx);
    rt_init_sys(ctx, ns);
    rt_init_canvas(ctx, ns);
    rt_init_gl(ctx, ns);
    rt_init_audio(ctx, ns);
    rt_init_input(ctx, ns);
    JS_SetPropertyStr(ctx, global, "__native", ns);
    JS_FreeValue(ctx, global);

    JSValue r = rt_eval_file(ctx, "/game/runtime/bootstrap.js");
    if (JS_IsException(r)) {
        rt_dump_exception(ctx);
        return 1;
    }
    JS_FreeValue(ctx, r);
    run_jobs(ctx);

    uint64_t freq = SDL_GetPerformanceFrequency(), t0 = SDL_GetPerformanceCounter();
    long frames = 0, ticks = 0;
    bool stats = debug_get("TCOAAL_STATS") != NULL;
    uint64_t last = SDL_GetPerformanceCounter();
    while (!quit_requested) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit_requested = true;
            rt_input_handle_event(ctx, &e);
        }
        rt_input_update();
        rt_drain_events(ctx);
        run_jobs(ctx);

        uint64_t js0 = SDL_GetPerformanceCounter();
        double now = (double)(js0 - t0) * 1000.0 / (double)freq;
        call_global(ctx, "__frame", now);
        run_jobs(ctx);
        double js_ms = (double)(SDL_GetPerformanceCounter() - js0) * 1000.0 / (double)freq;

        if (rt_gl_take_dirty()) {
            debug_frame(ctx, ++frames);
            SDL_GL_SwapWindow(window);
        } else {
            SDL_Delay(4);  // nothing drawn: don't spin, don't present a stale buffer
        }
        uint64_t end = SDL_GetPerformanceCounter();
        clocks_tick(++ticks);
        if (stats) stats_frame((double)(end - last) * 1000.0 / (double)freq, js_ms);
        last = end;
#ifdef __SWITCH__
        if (!appletMainLoop()) quit_requested = true;
#endif
    }

    rt_audio_shutdown();
    clocks_exit();
    // Skip JS_FreeRuntime: the OS reclaims everything and it is slow with a big heap.
    SDL_GL_DeleteContext(glc);
    SDL_DestroyWindow(window);
    SDL_Quit();
#ifdef __SWITCH__
    socketExit();
    romfsExit();
#endif
    return 0;
}
