// Core web globals: console, timers, animation frames, events, text codecs, blobs.
'use strict';
(function (global) {
    const sys = __native.sys;
    const rt = (global.__rt = {});

    global.window = global;
    global.self = global;
    global.top = global;
    global.parent = global;

    // ---- console -----------------------------------------------------------
    function fmt(args) {
        return Array.prototype.map.call(args, (a) => {
            if (typeof a === 'string') return a;
            if (a instanceof Error) return a.stack ? a + '\n' + a.stack : String(a);
            try {
                return typeof a === 'object' ? JSON.stringify(a) : String(a);
            } catch (e) {
                return String(a);
            }
        }).join(' ');
    }
    const timers = {};
    global.console = {
        log() { sys.log(fmt(arguments)); },
        info() { sys.log(fmt(arguments)); },
        debug() { sys.log(fmt(arguments)); },
        warn() { sys.log('[warn] ' + fmt(arguments)); },
        error() { sys.log('[error] ' + fmt(arguments)); },
        trace() { sys.log('[trace] ' + fmt(arguments) + '\n' + new Error().stack); },
        assert(c) { if (!c) sys.log('[assert] ' + fmt(Array.prototype.slice.call(arguments, 1))); },
        time(l) { timers[l || 'default'] = sys.now(); },
        timeEnd(l) { l = l || 'default'; sys.log(l + ': ' + (sys.now() - (timers[l] || 0)).toFixed(2) + 'ms'); },
        group() {}, groupEnd() {}, table() { sys.log(fmt(arguments)); }, dir() { sys.log(fmt(arguments)); },
        clear() {},
    };

    // Calls a callback, reporting (not propagating) exceptions like a browser task would.
    rt.invoke = function (fn, thisArg, args) {
        try {
            return fn.apply(thisArg, args || []);
        } catch (e) {
            rt.reportError(e);
        }
    };
    rt.reportError = function (e) {
        sys.log('Uncaught ' + (e && e.stack ? e + '\n' + e.stack : e));
        if (typeof global.onerror === 'function') {
            try {
                global.onerror(String(e && e.message || e), '', 0, 0, e);
            } catch (e2) {
                sys.log('onerror threw: ' + e2);
            }
        }
    };

    // ---- legacy RegExp statics (RegExp.$1-$9, lastMatch, ...) ----------------
    // V8 has these; QuickJS doesn't. RPG Maker and the YEP/Irina plugins read
    // RegExp.$1 after match()/test() (bust positions, message alignment, notetags).
    // match/test/replace/split all go through the `exec` property, so wrapping it
    // records every successful match. The capture strings are built lazily.
    (function () {
        const exec = RegExp.prototype.exec;
        let last = null;
        Object.defineProperty(RegExp.prototype, 'exec', {
            value: function exec_(s) {
                const r = exec.call(this, s);
                if (r !== null) last = r;
                return r;
            },
            writable: true, configurable: true,
        });
        const def = (name, get) => Object.defineProperty(RegExp, name, { get, configurable: true });
        for (let i = 1; i <= 9; i++)
            def('$' + i, () => (last && last[i] !== undefined ? last[i] : ''));
        const lastMatch = () => (last ? last[0] : '');
        const input = () => (last ? last.input : '');
        const left = () => (last ? last.input.slice(0, last.index) : '');
        const right = () => (last ? last.input.slice(last.index + last[0].length) : '');
        const paren = () => (last && last.length > 1 && last[last.length - 1] !== undefined ? last[last.length - 1] : '');
        def('lastMatch', lastMatch); def('$&', lastMatch);
        def('input', input); def('$_', input);
        def('leftContext', left); def('$`', left);
        def('rightContext', right); def("$'", right);
        def('lastParen', paren); def('$+', paren);
    })();

    // ---- performance ------------------------------------------------------
    // replace the built-in (read-only) object so everything shares the runtime clock
    global.performance = {
        now: () => sys.now(),
        timeOrigin: Date.now() - sys.now(),
        mark() {},
        measure() {},
        getEntriesByName() { return []; },
        memory: { get usedJSHeapSize() { return sys.memoryUsage(); }, jsHeapSizeLimit: 2e9 },
    };

    // ---- timers ------------------------------------------------------------
    let timerSeq = 1;
    const timerMap = new Map();
    function addTimer(fn, ms, args, repeat) {
        const id = timerSeq++;
        ms = Math.max(0, +ms || 0);
        timerMap.set(id, { fn, args, at: sys.now() + ms, ms: repeat ? Math.max(ms, 1) : -1 });
        return id;
    }
    global.setTimeout = (fn, ms, ...args) => addTimer(fn, ms, args, false);
    global.setInterval = (fn, ms, ...args) => addTimer(fn, ms, args, true);
    global.setImmediate = (fn, ...args) => addTimer(fn, 0, args, false);
    global.clearTimeout = global.clearInterval = global.clearImmediate = (id) => { timerMap.delete(id); };

    function runTimers() {
        const now = sys.now();
        // fire in due order; timers added while running wait for the next frame
        const due = [];
        for (const [id, t] of timerMap) if (t.at <= now) due.push([id, t]);
        due.sort((a, b) => a[1].at - b[1].at || a[0] - b[0]);
        for (const [id, t] of due) {
            if (!timerMap.has(id)) continue;
            if (t.ms >= 0) t.at = Math.max(t.at + t.ms, now);
            else timerMap.delete(id);
            if (typeof t.fn === 'function') rt.invoke(t.fn, global, t.args);
            else if (typeof t.fn === 'string') rt.invoke(() => sys.evalSource(t.fn, 'timer'));
        }
    }

    // ---- animation frames ---------------------------------------------------
    let rafSeq = 1;
    let rafQueue = new Map();
    global.requestAnimationFrame = (cb) => {
        const id = rafSeq++;
        rafQueue.set(id, cb);
        return id;
    };
    global.cancelAnimationFrame = (id) => { rafQueue.delete(id); };

    // tasks queued by the runtime (XHR completions, image loads, ...)
    const tasks = [];
    rt.queueTask = (fn) => { tasks.push(fn); };
    function runTasks() {
        for (let i = 0; i < tasks.length && i < 4096; i++) rt.invoke(tasks[i]);
        tasks.length = 0;
    }

    rt.frameHooks = [];
    global.__frame = function () {
        for (const h of rt.frameHooks) rt.invoke(h);
        runTasks();
        runTimers();
        runTasks();
        const q = rafQueue;
        rafQueue = new Map();
        const ts = sys.now();
        for (const cb of q.values()) rt.invoke(cb, global, [ts]);
    };

    // ---- events --------------------------------------------------------------
    class Event {
        constructor(type, init) {
            this.type = type;
            this.bubbles = !!(init && init.bubbles);
            this.cancelable = !!(init && init.cancelable);
            this.defaultPrevented = false;
            this.timeStamp = sys.now();
            this.target = null;
            this.currentTarget = null;
            this._stop = false;
            if (init) for (const k in init) if (!(k in this)) this[k] = init[k];
        }
        preventDefault() { this.defaultPrevented = true; }
        stopPropagation() { this._stop = true; }
        stopImmediatePropagation() { this._stop = true; this._stopNow = true; }
    }
    class CustomEvent extends Event {
        constructor(type, init) { super(type, init); this.detail = init && init.detail; }
    }
    class KeyboardEvent extends Event {}
    class MouseEvent extends Event {}
    class TouchEvent extends Event {}
    class PointerEvent extends MouseEvent {}
    class FocusEvent extends Event {}
    Object.assign(global, { Event, CustomEvent, KeyboardEvent, MouseEvent, TouchEvent, FocusEvent });
    void PointerEvent;  // deliberately not global: Pixi then uses mouse events

    class EventTarget {
        addEventListener(type, fn, opts) {
            if (!fn) return;
            const l = (this._listeners || (this._listeners = {}));
            const arr = l[type] || (l[type] = []);
            if (!arr.some((e) => e.fn === fn)) arr.push({ fn, once: !!(opts && opts.once) });
        }
        removeEventListener(type, fn) {
            const arr = this._listeners && this._listeners[type];
            if (!arr) return;
            const i = arr.findIndex((e) => e.fn === fn);
            if (i >= 0) arr.splice(i, 1);
        }
        dispatchEvent(ev) {
            if (!ev.target) ev.target = this;
            ev.currentTarget = this;
            const arr = this._listeners && this._listeners[ev.type];
            if (arr) {
                for (const e of arr.slice()) {
                    if (e.once) this.removeEventListener(ev.type, e.fn);
                    if (typeof e.fn === 'function') rt.invoke(e.fn, this, [ev]);
                    else if (e.fn && typeof e.fn.handleEvent === 'function') rt.invoke(e.fn.handleEvent, e.fn, [ev]);
                    if (ev._stopNow) break;
                }
            }
            const h = this['on' + ev.type];
            if (typeof h === 'function') rt.invoke(h, this, [ev]);
            return !ev.defaultPrevented;
        }
    }
    global.EventTarget = EventTarget;
    rt.EventTarget = EventTarget;
    // make the global object an event target too
    for (const k of ['addEventListener', 'removeEventListener', 'dispatchEvent'])
        global[k] = EventTarget.prototype[k].bind(global);

    // ---- text codecs -----------------------------------------------------------
    class TextDecoder {
        constructor(label) { this.encoding = (label || 'utf-8').toLowerCase(); }
        decode(buf) {
            if (!buf) return '';
            if (this.encoding === 'utf-8' || this.encoding === 'utf8') return sys.utf8Decode(buf);
            const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf.buffer || buf, buf.byteOffset || 0, buf.byteLength);
            let s = '';
            for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
            return s;
        }
    }
    class TextEncoder {
        get encoding() { return 'utf-8'; }
        encode(str) { return new Uint8Array(sys.utf8Encode(String(str === undefined ? '' : str))); }
    }
    global.TextDecoder = TextDecoder;
    global.TextEncoder = TextEncoder;

    // ---- Blob / URL ---------------------------------------------------------
    function toBytes(part) {
        if (part instanceof Blob) return part._bytes;
        if (part instanceof ArrayBuffer) return new Uint8Array(part);
        if (ArrayBuffer.isView(part)) return new Uint8Array(part.buffer, part.byteOffset, part.byteLength);
        return new Uint8Array(sys.utf8Encode(String(part)));
    }
    class Blob {
        constructor(parts, opts) {
            const arrs = (parts || []).map(toBytes);
            const len = arrs.reduce((n, a) => n + a.length, 0);
            const out = new Uint8Array(len);
            let o = 0;
            for (const a of arrs) { out.set(a, o); o += a.length; }
            this._bytes = out;
            this.type = (opts && opts.type) || '';
        }
        get size() { return this._bytes.length; }
        arrayBuffer() { return Promise.resolve(this._bytes.slice().buffer); }
        text() { return Promise.resolve(sys.utf8Decode(this._bytes)); }
        slice(a, b, type) { const r = new Blob([]); r._bytes = this._bytes.slice(a, b); r.type = type || ''; return r; }
    }
    global.Blob = Blob;
    global.File = class File extends Blob {
        constructor(parts, name, opts) { super(parts, opts); this.name = name; }
    };

    const blobs = new Map();
    let blobSeq = 1;
    rt.blobs = blobs;
    global.URL = global.URL || {};
    const URLClass = class URL {
        constructor(url, base) {
            this.href = base && !/^[a-z]+:/.test(url) ? String(base).replace(/[^/]*$/, '') + url : String(url);
            const m = /^([a-z]+:)?(\/\/[^/]*)?([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(this.href) || [];
            this.protocol = m[1] || '';
            this.pathname = m[3] || '';
            this.search = m[4] || '';
            this.hash = m[5] || '';
        }
        toString() { return this.href; }
        static createObjectURL(blob) {
            const url = 'blob:tcoaal/' + blobSeq++;
            blobs.set(url, blob);
            return url;
        }
        static revokeObjectURL(url) { blobs.delete(url); }
    };
    global.URL = URLClass;
    global.webkitURL = URLClass;

    class FileReader extends EventTarget {
        _done(result) {
            this.result = result;
            this.readyState = 2;
            rt.queueTask(() => {
                this.dispatchEvent(new Event('load'));
                this.dispatchEvent(new Event('loadend'));
            });
        }
        readAsArrayBuffer(b) { this._done(b._bytes.slice().buffer); }
        readAsText(b) { this._done(sys.utf8Decode(b._bytes)); }
        readAsDataURL(b) {
            let s = '';
            for (let i = 0; i < b._bytes.length; i++) s += String.fromCharCode(b._bytes[i]);
            this._done('data:' + (b.type || 'application/octet-stream') + ';base64,' + btoa(s));
        }
    }
    global.FileReader = FileReader;

    // ---- resource loading helpers --------------------------------------------
    // Resolves a page-relative URL to a virtual path ("/game/www/...") or a blob.
    rt.resolveUrl = function (url) {
        url = String(url);
        if (url.startsWith('blob:')) return { blob: blobs.get(url) || null };
        if (url.startsWith('data:')) return { data: url };
        url = url.replace(/^file:\/\//, '').replace(/[?#].*$/, '');
        try {
            url = decodeURIComponent(url);
        } catch (e) {}
        if (url.startsWith('/game/') || url.startsWith('/save/')) return { path: url };
        return { path: '/game/www/' + url.replace(/^\.?\//, '') };
    };

    rt.dataUrlBytes = function (url) {
        const comma = url.indexOf(',');
        const meta = url.slice(5, comma);
        const body = url.slice(comma + 1);
        if (/;base64$/.test(meta)) {
            const bin = atob(body);
            const out = new Uint8Array(bin.length);
            for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
            return out.buffer;
        }
        return sys.utf8Encode(decodeURIComponent(body));
    };

    // Returns an ArrayBuffer, or null if missing.
    rt.loadBytes = function (url) {
        const r = rt.resolveUrl(url);
        if (r.blob !== undefined) return r.blob ? r.blob._bytes.slice().buffer : null;
        if (r.data) return rt.dataUrlBytes(r.data);
        return sys.readFile(r.path);
    };

    if (typeof global.structuredClone !== 'function')
        global.structuredClone = (v) => (v === undefined ? v : JSON.parse(JSON.stringify(v)));
})(globalThis);
