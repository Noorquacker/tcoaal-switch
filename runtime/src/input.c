// Input: SDL game controllers exposed as W3C "standard" gamepads, and
// keyboard events forwarded as DOM keydown/keyup with legacy keyCodes.
#include <SDL.h>
#include "rt.h"

#define MAX_PADS 4
static SDL_GameController *pads[MAX_PADS];

static void open_pads(void) {
    for (int i = 0; i < SDL_NumJoysticks() && i < MAX_PADS; i++) {
        if (!pads[i] && SDL_IsGameController(i)) {
            pads[i] = SDL_GameControllerOpen(i);
            if (pads[i]) rt_log("gamepad %d: %s", i, SDL_GameControllerName(pads[i]));
        }
    }
}

void rt_input_update(void) {
    SDL_GameControllerUpdate();
}

static int dom_keycode(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 112 + (k - SDLK_F1);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 97 + (k - SDLK_KP_1);
    switch (k) {
    case SDLK_KP_0: return 96;
    case SDLK_BACKSPACE: return 8;
    case SDLK_TAB: return 9;
    case SDLK_RETURN: case SDLK_KP_ENTER: return 13;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 16;
    case SDLK_LCTRL: case SDLK_RCTRL: return 17;
    case SDLK_LALT: case SDLK_RALT: return 18;
    case SDLK_PAUSE: return 19;
    case SDLK_ESCAPE: return 27;
    case SDLK_SPACE: return 32;
    case SDLK_PAGEUP: return 33;
    case SDLK_PAGEDOWN: return 34;
    case SDLK_END: return 35;
    case SDLK_HOME: return 36;
    case SDLK_LEFT: return 37;
    case SDLK_UP: return 38;
    case SDLK_RIGHT: return 39;
    case SDLK_DOWN: return 40;
    case SDLK_INSERT: return 45;
    case SDLK_DELETE: return 46;
    default: return 0;
    }
}

static void call_global(JSContext *ctx, const char *name, int argc, JSValue *argv) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, g, name);
    if (JS_IsFunction(ctx, fn)) {
        JSValue r = JS_Call(ctx, fn, g, argc, argv);
        if (JS_IsException(r)) rt_dump_exception(ctx);
        JS_FreeValue(ctx, r);
    }
    for (int i = 0; i < argc; i++) JS_FreeValue(ctx, argv[i]);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, g);
}

void rt_input_handle_event(JSContext *ctx, const void *ev) {
    const SDL_Event *e = ev;
    switch (e->type) {
    case SDL_CONTROLLERDEVICEADDED:
    case SDL_JOYDEVICEADDED:
        open_pads();
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        for (int i = 0; i < MAX_PADS; i++) {
            if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == e->cdevice.which) {
                SDL_GameControllerClose(pads[i]);
                pads[i] = NULL;
            }
        }
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        int code = dom_keycode(e->key.keysym.sym);
        if (!code) break;
        JSValue args[3] = {
            JS_NewString(ctx, e->type == SDL_KEYDOWN ? "keydown" : "keyup"),
            JS_NewInt32(ctx, code),
            JS_NewBool(ctx, e->key.repeat != 0),
        };
        call_global(ctx, "__dispatchKey", 3, args);
        break;
    }
    }
}

// getGamepads() -> [{id, index, connected, mapping, buttons:[{pressed,value}], axes:[]}]
static JSValue js_get_gamepads(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc; (void)argv;
    // standard mapping order
    // RPG Maker treats standard button 0 as OK and 1 as cancel. On Switch, SDL reports
    // Nintendo labels, so swap them to get A (right) = OK and B (bottom) = cancel.
    static const int bmap[17] = {
#ifdef __SWITCH__
        SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_X,
#else
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
#endif
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, -1, -2,
        SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_GUIDE};
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i]) {
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NULL);
            continue;
        }
        SDL_GameController *p = pads[i];
        JSValue pad = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, pad, "id", JS_NewString(ctx, SDL_GameControllerName(p) ? SDL_GameControllerName(p) : "pad"));
        JS_SetPropertyStr(ctx, pad, "index", JS_NewInt32(ctx, i));
        JS_SetPropertyStr(ctx, pad, "connected", JS_TRUE);
        JS_SetPropertyStr(ctx, pad, "mapping", JS_NewString(ctx, "standard"));
        JSValue buttons = JS_NewArray(ctx);
        for (uint32_t b = 0; b < 17; b++) {
            double v;
            if (bmap[b] == -1) v = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0;
            else if (bmap[b] == -2) v = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0;
            else v = SDL_GameControllerGetButton(p, (SDL_GameControllerButton)bmap[b]);
            JSValue btn = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, btn, "pressed", JS_NewBool(ctx, v > 0.5));
            JS_SetPropertyStr(ctx, btn, "value", JS_NewFloat64(ctx, v));
            JS_SetPropertyUint32(ctx, buttons, b, btn);
        }
        JS_SetPropertyStr(ctx, pad, "buttons", buttons);
        JSValue axes = JS_NewArray(ctx);
        static const SDL_GameControllerAxis amap[4] = {SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
                                                       SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY};
        for (uint32_t a = 0; a < 4; a++)
            JS_SetPropertyUint32(ctx, axes, a, JS_NewFloat64(ctx, SDL_GameControllerGetAxis(p, amap[a]) / 32767.0));
        JS_SetPropertyStr(ctx, pad, "axes", axes);
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, pad);
    }
    return arr;
}

void rt_init_input(JSContext *ctx, JSValueConst ns) {
    open_pads();
    JSValue in = JS_NewObject(ctx);
    rt_set_func(ctx, in, "getGamepads", js_get_gamepads, 0);
    JS_SetPropertyStr(ctx, ns, "input", in);
}
