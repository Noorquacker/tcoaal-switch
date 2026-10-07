// Minimal DOM: just enough document/element behaviour for RPG Maker MV, Pixi and the game.
'use strict';
(function (global) {
    const sys = __native.sys;
    const rt = global.__rt;
    const { EventTarget } = rt;

    // ---- nodes ---------------------------------------------------------------
    class ClassList {
        constructor(el) { this._el = el; }
        _get() { return (this._el.className || '').split(/\s+/).filter(Boolean); }
        add(...c) { const s = new Set(this._get()); c.forEach((x) => s.add(x)); this._el.className = [...s].join(' '); }
        remove(...c) { this._el.className = this._get().filter((x) => !c.includes(x)).join(' '); }
        contains(c) { return this._get().includes(c); }
        toggle(c) { if (this.contains(c)) { this.remove(c); return false; } this.add(c); return true; }
    }

    class Node extends EventTarget {
        constructor() {
            super();
            this.childNodes = [];
            this.parentNode = null;
        }
        get children() { return this.childNodes.filter((n) => n instanceof Element); }
        get firstChild() { return this.childNodes[0] || null; }
        get lastChild() { return this.childNodes[this.childNodes.length - 1] || null; }
        get parentElement() { return this.parentNode; }
        appendChild(n) {
            if (n.parentNode) n.parentNode.removeChild(n);
            this.childNodes.push(n);
            n.parentNode = this;
            if (n._onInserted) n._onInserted();
            return n;
        }
        insertBefore(n, ref) {
            if (n.parentNode) n.parentNode.removeChild(n);
            const i = ref ? this.childNodes.indexOf(ref) : -1;
            if (i < 0) this.childNodes.push(n);
            else this.childNodes.splice(i, 0, n);
            n.parentNode = this;
            if (n._onInserted) n._onInserted();
            return n;
        }
        removeChild(n) {
            const i = this.childNodes.indexOf(n);
            if (i >= 0) this.childNodes.splice(i, 1);
            n.parentNode = null;
            return n;
        }
        replaceChild(n, old) { this.insertBefore(n, old); return this.removeChild(old); }
        remove() { if (this.parentNode) this.parentNode.removeChild(this); }
        contains(n) { for (; n; n = n.parentNode) if (n === this) return true; return false; }
        hasChildNodes() { return this.childNodes.length > 0; }
        get textContent() { return this._text !== undefined ? this._text : this.childNodes.map((c) => c.textContent).join(''); }
        set textContent(v) { this.childNodes = []; this._text = String(v); }
        dispatchEvent(ev) {
            const r = super.dispatchEvent(ev);
            if (ev.bubbles && !ev._stop && this.parentNode) this.parentNode.dispatchEvent(ev);
            return r;
        }
    }

    class Text extends Node {
        constructor(t) { super(); this._text = String(t); this.nodeType = 3; }
        get data() { return this._text; }
        set data(v) { this._text = String(v); }
    }

    class Element extends Node {
        constructor(tag) {
            super();
            this.tagName = this.nodeName = tag.toUpperCase();
            this.localName = tag.toLowerCase();
            this.nodeType = 1;
            this.style = { setProperty() {}, removeProperty() {}, getPropertyValue() { return ''; } };
            this.attributes = {};
            this.id = '';
            this.className = '';
            this.classList = new ClassList(this);
            this.dataset = {};
            this._html = '';
        }
        setAttribute(k, v) { this.attributes[k] = String(v); if (k === 'id') this.id = String(v); else if (k in this && typeof this[k] !== 'function') this[k] = v; }
        getAttribute(k) { return k in this.attributes ? this.attributes[k] : (k === 'id' ? this.id || null : null); }
        hasAttribute(k) { return k in this.attributes; }
        removeAttribute(k) { delete this.attributes[k]; }
        get innerHTML() { return this._html; }
        set innerHTML(v) { this._html = String(v); this.childNodes = []; }
        get innerText() { return this.textContent; }
        set innerText(v) { this.textContent = v; }
        get outerHTML() { return ''; }
        getBoundingClientRect() {
            const w = this.width || this.offsetWidth, h = this.height || this.offsetHeight;
            return { left: 0, top: 0, x: 0, y: 0, right: w, bottom: h, width: w, height: h };
        }
        get offsetWidth() { return this.width || 0; }
        get offsetHeight() { return this.height || 0; }
        get clientWidth() { return this.offsetWidth; }
        get clientHeight() { return this.offsetHeight; }
        get offsetLeft() { return 0; }
        get offsetTop() { return 0; }
        get offsetParent() { return this.parentNode; }
        focus() {}
        blur() {}
        click() { this.dispatchEvent(new MouseEvent('click', { bubbles: true })); }
        requestFullscreen() { return Promise.resolve(); }
        getElementsByTagName(t) { return queryAll(this, (e) => t === '*' || e.localName === t.toLowerCase()); }
        getElementsByClassName(c) { return queryAll(this, (e) => e.classList.contains(c)); }
        querySelector(sel) { return querySelectorAll(this, sel)[0] || null; }
        querySelectorAll(sel) { return querySelectorAll(this, sel); }
        cloneNode() { return document.createElement(this.localName); }
    }
    Element.prototype.webkitRequestFullScreen = Element.prototype.requestFullscreen;
    Element.prototype.mozRequestFullScreen = Element.prototype.requestFullscreen;
    Element.prototype.msRequestFullscreen = Element.prototype.requestFullscreen;

    function queryAll(root, pred, out) {
        out = out || [];
        for (const c of root.childNodes) {
            if (c instanceof Element) {
                if (pred(c)) out.push(c);
                queryAll(c, pred, out);
            }
        }
        return out;
    }
    function querySelectorAll(root, sel) {
        sel = sel.trim();
        if (sel[0] === '#') return queryAll(root, (e) => e.id === sel.slice(1));
        if (sel[0] === '.') return queryAll(root, (e) => e.classList.contains(sel.slice(1)));
        return queryAll(root, (e) => e.localName === sel.toLowerCase());
    }

    // ---- canvas -------------------------------------------------------------
    class HTMLCanvasElement extends Element {
        constructor() {
            super('canvas');
            this._w = 300;
            this._h = 150;
            this._surface = null;
            this._ctx2d = null;
            this._gl = null;
        }
        get width() { return this._w; }
        set width(v) { this._w = Math.max(0, v | 0); this._resized(); }
        get height() { return this._h; }
        set height(v) { this._h = Math.max(0, v | 0); this._resized(); }
        _resized() {
            if (this._surface) this._surface.resize(this._w, this._h);
            if (this._ctx2d) this._ctx2d._reset();
        }
        _ensureSurface() {
            if (!this._surface) this._surface = __native.canvas.create(this._w, this._h);
            return this._surface;
        }
        getContext(type, attrs) {
            if (type === '2d') {
                if (this._gl) return null;
                if (!this._ctx2d) this._ctx2d = new global.CanvasRenderingContext2D(this);
                return this._ctx2d;
            }
            if (type === 'webgl' || type === 'experimental-webgl') {
                if (this._ctx2d) return null;
                if (!this._gl) this._gl = rt.createWebGLContext(this, attrs || {});
                return this._gl;
            }
            return null;
        }
        toDataURL(type) {
            const s = this._ensureSurface();
            const png = s.encodePNG();
            if (!png) return 'data:,';
            const u8 = new Uint8Array(png);
            let bin = '';
            for (let i = 0; i < u8.length; i += 0x8000) bin += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
            return 'data:image/png;base64,' + btoa(bin);
        }
        toBlob(cb) {
            const s = this._ensureSurface();
            const b = new Blob([s.encodePNG() || new ArrayBuffer(0)], { type: 'image/png' });
            rt.queueTask(() => cb(b));
        }
    }

    // ---- images -------------------------------------------------------------
    class HTMLImageElement extends Element {
        constructor(w, h) {
            super('img');
            this._src = '';
            this._surface = null;
            this.complete = true;
            this.crossOrigin = null;
            this.decoding = 'auto';
            this._w = w || 0;
            this._h = h || 0;
        }
        get width() { return this._surface ? this._surface.width : this._w; }
        set width(v) { this._w = v; }
        get height() { return this._surface ? this._surface.height : this._h; }
        set height(v) { this._h = v; }
        get naturalWidth() { return this._surface ? this._surface.width : 0; }
        get naturalHeight() { return this._surface ? this._surface.height : 0; }
        get src() { return this._src; }
        set src(url) {
            this._src = String(url);
            this._surface = null;
            this.complete = false;
            const token = (this._token = {});
            if (!this._src) return;
            const done = (surf) => {
                if (this._token !== token) return;
                this.complete = true;
                if (surf) {
                    this._surface = surf;
                    this.dispatchEvent(new Event('load'));
                } else {
                    console.warn('image failed: ' + this._src);
                    this.dispatchEvent(new Event('error'));
                }
            };
            // read + decode on a worker thread; load fires from a later frame
            let src = null;
            try {
                const r = rt.resolveUrl(this._src);
                src = r.blob !== undefined || r.data ? rt.loadBytes(this._src) : r.path;
            } catch (e) {}
            if (src) __native.canvas.decodeImageAsync(src, done);
            else rt.queueTask(() => done(null));
        }
        decode() {
            return new Promise((res, rej) => {
                if (this.complete) return this._surface ? res() : rej(new Error('decode failed'));
                this.addEventListener('load', () => res(), { once: true });
                this.addEventListener('error', () => rej(new Error('decode failed')), { once: true });
            });
        }
    }

    // ---- media stubs -------------------------------------------------------
    class HTMLMediaElement extends Element {
        constructor(tag) {
            super(tag);
            this.src = '';
            this.volume = 1;
            this.muted = false;
            this.paused = true;
            this.currentTime = 0;
            this.duration = NaN;
            this.loop = false;
            this.autoplay = false;
            this.readyState = 0;
        }
        canPlayType(t) { return /ogg/.test(t) ? 'probably' : ''; }
        play() {
            const p = Promise.reject(new Error('media playback is not supported'));
            p.catch(() => {});
            rt.queueTask(() => this.dispatchEvent(new Event('error')));
            return p;
        }
        pause() { this.paused = true; }
        load() {}
        setAttribute(k, v) { super.setAttribute(k, v); }
    }
    class HTMLVideoElement extends HTMLMediaElement {
        constructor() { super('video'); this.videoWidth = 0; this.videoHeight = 0; }
        canPlayType() { return ''; }
    }
    class HTMLAudioElement extends HTMLMediaElement {
        constructor(src) { super('audio'); if (src) this.src = src; }
    }

    // ---- scripts / styles ----------------------------------------------------
    const pendingScripts = [];
    rt.pendingScripts = pendingScripts;
    rt.currentScript = null;

    rt.execScriptFile = function (el, vpath) {
        const prev = rt.currentScript;
        rt.currentScript = el;
        let ok = true;
        try {
            sys.evalScript(vpath);
        } catch (e) {
            ok = false;
            rt.reportError(e);
        }
        rt.currentScript = prev;
        return ok;
    };

    rt.runPendingScripts = function () {
        while (pendingScripts.length) {
            const el = pendingScripts.shift();
            const r = rt.resolveUrl(el.src);
            if (!r.path || !sys.stat(r.path)) {
                console.error('script not found: ' + el.src);
                el.dispatchEvent(new Event('error'));
                continue;
            }
            if (rt.execScriptFile(el, r.path)) el.dispatchEvent(new Event('load'));
        }
    };
    rt.frameHooks.push(rt.runPendingScripts);

    class HTMLScriptElement extends Element {
        constructor() { super('script'); this.src = ''; this.type = 'text/javascript'; this.async = true; this.defer = false; }
        _onInserted() {
            if (this._started || this._noExec) return;
            this._started = true;
            if (this.src) {
                pendingScripts.push(this);
            } else if (this.textContent) {
                const prev = rt.currentScript;
                rt.currentScript = this;
                try {
                    sys.evalSource(this.textContent, 'inline-script');
                } catch (e) {
                    rt.reportError(e);
                }
                rt.currentScript = prev;
            }
        }
    }

    // Parses @font-face rules and registers the fonts.
    rt.loadCss = function (css, baseDir) {
        const re = /@font-face\s*\{([^}]*)\}/g;
        let m;
        while ((m = re.exec(css))) {
            const body = m[1];
            const fam = /font-family\s*:\s*['"]?([^;'"]+)['"]?/i.exec(body);
            const src = /url\(\s*['"]?([^'")]+)['"]?\s*\)/i.exec(body);
            if (!fam || !src) continue;
            const url = src[1].startsWith('/') || /^[a-z]+:/.test(src[1]) ? src[1] : baseDir + src[1];
            const bytes = rt.loadBytes(url);
            if (bytes && __native.canvas.registerFont(fam[1].trim(), bytes, /bold/i.test(body), /italic/i.test(body)))
                rt.loadedFonts.add(fam[1].trim().toLowerCase());
            else console.warn('font failed: ' + url);
        }
    };
    rt.loadedFonts = new Set();

    class HTMLStyleElement extends Element {
        constructor() { super('style'); }
        _onInserted() { if (this.textContent) rt.loadCss(this.textContent, ''); }
    }
    class HTMLLinkElement extends Element {
        constructor() { super('link'); this.rel = ''; this.href = ''; }
        _onInserted() {
            if (!/stylesheet/i.test(this.rel) || !this.href) return;
            const bytes = rt.loadBytes(this.href);
            if (bytes) rt.loadCss(sys.utf8Decode(bytes), this.href.replace(/[^/]*$/, ''));
            rt.queueTask(() => this.dispatchEvent(new Event(bytes ? 'load' : 'error')));
        }
    }

    // ---- document --------------------------------------------------------------
    const tagClasses = {
        canvas: HTMLCanvasElement, img: HTMLImageElement, image: HTMLImageElement, video: HTMLVideoElement,
        audio: HTMLAudioElement, script: HTMLScriptElement, style: HTMLStyleElement, link: HTMLLinkElement,
    };

    class Document extends Node {
        constructor() {
            super();
            this.nodeType = 9;
            this.documentElement = new Element('html');
            this.head = new Element('head');
            this.body = new Element('body');
            this.documentElement.appendChild(this.head);
            this.documentElement.appendChild(this.body);
            this.childNodes = [this.documentElement];
            this.documentElement.parentNode = this;
            this.title = '';
            this.readyState = 'loading';
            this.visibilityState = 'visible';
            this.hidden = false;
            this.cookie = '';
            this.fullscreenElement = null;
            this.fullscreenEnabled = true;
        }
        createElement(tag) {
            const C = tagClasses[String(tag).toLowerCase()];
            return C ? new C() : new Element(String(tag));
        }
        createElementNS(ns, tag) { return this.createElement(tag); }
        createTextNode(t) { return new Text(t); }
        createDocumentFragment() { return new Element('fragment'); }
        createEvent() { return new Event(''); }
        getElementById(id) { return queryAll(this, (e) => e.id === id)[0] || null; }
        getElementsByTagName(t) { return queryAll(this, (e) => t === '*' || e.localName === t.toLowerCase()); }
        getElementsByClassName(c) { return queryAll(this, (e) => e.classList.contains(c)); }
        querySelector(sel) { return querySelectorAll(this, sel)[0] || null; }
        querySelectorAll(sel) { return querySelectorAll(this, sel); }
        get currentScript() { return rt.currentScript; }
        get activeElement() { return this.body; }
        hasFocus() { return true; }
        exitFullscreen() { return Promise.resolve(); }
        execCommand() { return false; }
    }

    const document = new Document();
    global.document = document;
    Object.assign(global, {
        Node, Element, HTMLElement: Element, Text, Document, HTMLCanvasElement, HTMLImageElement,
        HTMLVideoElement, HTMLAudioElement, HTMLMediaElement, HTMLScriptElement,
    });
    global.Image = function Image(w, h) { return new HTMLImageElement(w, h); };
    global.Image.prototype = HTMLImageElement.prototype;
    global.Audio = function Audio(src) { return new HTMLAudioElement(src); };
    global.Audio.prototype = HTMLAudioElement.prototype;

    // ---- fonts ------------------------------------------------------------
    class FontFace {
        constructor(family, source, desc) {
            this.family = String(family).replace(/^['"]|['"]$/g, '');
            this._source = source;
            this.status = 'unloaded';
            this.weight = (desc && desc.weight) || 'normal';
            this.style = (desc && desc.style) || 'normal';
            this.loaded = new Promise((res, rej) => { this._res = res; this._rej = rej; });
            this.loaded.catch(() => {});
        }
        load() {
            if (this.status === 'loaded' || this.status === 'error') return this.loaded;
            let bytes = null;
            if (typeof this._source === 'string') {
                const m = /url\(\s*['"]?([^'")]+)['"]?\s*\)/.exec(this._source);
                bytes = m ? rt.loadBytes(m[1]) : null;
            } else if (this._source) {
                bytes = this._source instanceof ArrayBuffer ? this._source : this._source.buffer.slice(this._source.byteOffset, this._source.byteOffset + this._source.byteLength);
            }
            const ok = bytes && __native.canvas.registerFont(this.family, bytes, /bold|[6-9]00/.test(this.weight), /italic/.test(this.style));
            this.status = ok ? 'loaded' : 'error';
            if (ok) { rt.loadedFonts.add(this.family.toLowerCase()); this._res(this); }
            else this._rej(new Error('font load failed: ' + this.family));
            return this.loaded;
        }
    }
    global.FontFace = FontFace;

    function fontFamilies(font) {
        const m = /(?:\d+(?:\.\d+)?(?:px|pt|em|%)\S*\s+)(.+)$/.exec(font || '');
        return (m ? m[1] : font || '').split(',').map((s) => s.trim().replace(/^['"]|['"]$/g, ''));
    }
    const fontSet = new Set();
    document.fonts = {
        status: 'loaded',
        ready: null,
        add(ff) { fontSet.add(ff); if (ff.status === 'unloaded') ff.load(); return this; },
        delete(ff) { return fontSet.delete(ff); },
        has(ff) { return fontSet.has(ff); },
        clear() { fontSet.clear(); },
        forEach(cb) { fontSet.forEach(cb); },
        get size() { return fontSet.size; },
        check(font) {
            return fontFamilies(font).every((f) => /^(sans-serif|serif|monospace)$/i.test(f) || __native.canvas.hasFont(f));
        },
        load(font) { return Promise.resolve([...fontSet].filter((f) => fontFamilies(font).includes(f.family))); },
        addEventListener() {}, removeEventListener() {},
        [Symbol.iterator]() { return fontSet.values(); },
    };
    document.fonts.ready = Promise.resolve(document.fonts);

    // ---- XMLHttpRequest ------------------------------------------------------
    class XMLHttpRequest extends EventTarget {
        constructor() {
            super();
            this.readyState = 0;
            this.status = 0;
            this.statusText = '';
            this.responseType = '';
            this.response = null;
            this.timeout = 0;
            this.withCredentials = false;
            this.upload = new EventTarget();
            this._mime = null;
        }
        open(method, url, async) {
            this._method = method;
            this._url = url;
            this._async = async !== false;
            this.readyState = 1;
        }
        overrideMimeType(m) { this._mime = m; }
        setRequestHeader() {}
        getResponseHeader() { return null; }
        getAllResponseHeaders() { return ''; }
        abort() { this._aborted = true; }
        get responseText() {
            if (this.responseType && this.responseType !== 'text') throw new Error('responseText unavailable');
            return this.response;
        }
        get responseURL() { return this._url; }
        send() {
            const run = () => {
                if (this._aborted) return;
                let bytes = null;
                try {
                    bytes = rt.loadBytes(this._url);
                } catch (e) {}
                this.readyState = 4;
                if (!bytes) {
                    this.status = 0;
                    this.dispatchEvent(new Event('readystatechange'));
                    this.dispatchEvent(new Event('error'));
                    this.dispatchEvent(new Event('loadend'));
                    return;
                }
                this.status = 200;
                this.statusText = 'OK';
                switch (this.responseType) {
                case 'arraybuffer': this.response = bytes; break;
                case 'blob': this.response = new Blob([bytes]); break;
                case 'json':
                    try { this.response = JSON.parse(sys.utf8Decode(bytes)); } catch (e) { this.response = null; }
                    break;
                default: this.response = sys.utf8Decode(bytes);
                }
                this.dispatchEvent(new Event('readystatechange'));
                this.dispatchEvent(new Event('load'));
                this.dispatchEvent(new Event('loadend'));
            };
            if (this._async) rt.queueTask(run);
            else run();
        }
    }
    global.XMLHttpRequest = XMLHttpRequest;

    // ---- fetch --------------------------------------------------------------
    class Response {
        constructor(body, init) {
            this.status = init && init.status !== undefined ? init.status : 200;
            this.ok = this.status >= 200 && this.status < 300;
            this.statusText = '';
            this.headers = { get() { return null; }, has() { return false; } };
            this.body = null;  // no streaming: consumers fall back to arrayBuffer()
            if (body instanceof Blob) this._bytes = body._bytes;
            else if (body instanceof ArrayBuffer) this._bytes = new Uint8Array(body);
            else if (ArrayBuffer.isView(body)) this._bytes = new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
            else if (body == null) this._bytes = new Uint8Array(0);
            else this._bytes = new Uint8Array(sys.utf8Encode(String(body)));
        }
        arrayBuffer() { return Promise.resolve(this._bytes.slice().buffer); }
        text() { return Promise.resolve(sys.utf8Decode(this._bytes)); }
        json() { return this.text().then(JSON.parse); }
        blob() { return Promise.resolve(new Blob([this._bytes])); }
        clone() { return new Response(this._bytes.slice(), { status: this.status }); }
    }
    global.Response = Response;
    global.Headers = class Headers { get() { return null; } has() { return false; } set() {} append() {} };
    global.Request = class Request { constructor(url) { this.url = url; } };
    global.fetch = function (url) {
        url = url && url.url ? url.url : url;
        return new Promise((resolve, reject) => {
            rt.queueTask(() => {
                const r = rt.resolveUrl(url);
                if (r.path) {
                    const st = sys.stat(r.path);
                    if (st && st.dir) return resolve(new Response(null, { status: 200 }));
                }
                let bytes = null;
                try {
                    bytes = rt.loadBytes(url);
                } catch (e) {}
                if (!bytes) return reject(new TypeError('Failed to fetch: ' + url));
                resolve(new Response(bytes, { status: 200 }));
            });
        });
    };

    // ---- storage ------------------------------------------------------------
    const LS_PATH = '/save/localStorage.json';
    let lsData = {};
    try {
        const t = sys.readText(LS_PATH);
        if (t) lsData = JSON.parse(t);
    } catch (e) {
        console.warn('localStorage unreadable, starting empty');
    }
    const persist = () => sys.writeFile(LS_PATH, JSON.stringify(lsData));
    const storage = {
        getItem(k) { k = String(k); return Object.prototype.hasOwnProperty.call(lsData, k) ? lsData[k] : null; },
        setItem(k, v) { lsData[String(k)] = String(v); persist(); },
        removeItem(k) { delete lsData[String(k)]; persist(); },
        clear() { lsData = {}; persist(); },
        key(i) { return Object.keys(lsData)[i] || null; },
        get length() { return Object.keys(lsData).length; },
    };
    global.localStorage = storage;
    const session = {};
    global.sessionStorage = {
        getItem(k) { return k in session ? session[k] : null; },
        setItem(k, v) { session[k] = String(v); },
        removeItem(k) { delete session[k]; },
        clear() { for (const k in session) delete session[k]; },
        key(i) { return Object.keys(session)[i] || null; },
        get length() { return Object.keys(session).length; },
    };

    // ---- window ------------------------------------------------------------------
    Object.assign(global, {
        innerWidth: 1280, innerHeight: 720, outerWidth: 1280, outerHeight: 720,
        devicePixelRatio: 1, screenX: 0, screenY: 0, scrollX: 0, scrollY: 0, pageXOffset: 0, pageYOffset: 0,
        screen: { width: 1280, height: 720, availWidth: 1280, availHeight: 720, colorDepth: 24, orientation: { angle: 0, type: 'landscape-primary' } },
        location: {
            href: 'file:///game/www/index.html', protocol: 'file:', host: '', hostname: '', pathname: '/game/www/index.html',
            search: '', hash: '', origin: 'file://',
            reload() { console.warn('location.reload() is not supported'); },
            assign() {}, replace() {},
        },
        history: { pushState() {}, replaceState() {}, back() {}, length: 1 },
        alert(m) { sys.log('[alert] ' + m); },
        confirm(m) { sys.log('[confirm] ' + m); return false; },
        prompt() { return null; },
        open() { return null; },
        close() { sys.quit(); },
        focus() {}, blur() {}, scrollTo() {}, scroll() {}, moveTo() {}, moveBy() {}, resizeTo() {}, resizeBy() {}, print() {},
        postMessage() {},
        getComputedStyle(el) { return Object.assign({ getPropertyValue() { return ''; } }, el && el.style); },
        matchMedia() { return { matches: false, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {} }; },
        getSelection() { return { removeAllRanges() {}, addRange() {} }; },
        onerror: null,
    });

    global.navigator = {
        userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/78.0.3904.97 Safari/537.36',
        appVersion: '5.0 (Windows NT 10.0; Win64; x64)',
        platform: 'Win32', vendor: 'Google Inc.', language: 'en-US', languages: ['en-US', 'en'],
        onLine: true, cookieEnabled: false, standalone: false, maxTouchPoints: 0, hardwareConcurrency: 4,
        getGamepads: () => __native.input.getGamepads(),
        vibrate() { return false; },
        mediaDevices: undefined,
    };

    // keyboard events from the native layer
    const keyNames = { 8: 'Backspace', 9: 'Tab', 13: 'Enter', 16: 'Shift', 17: 'Control', 18: 'Alt', 27: 'Escape', 32: ' ',
        33: 'PageUp', 34: 'PageDown', 35: 'End', 36: 'Home', 37: 'ArrowLeft', 38: 'ArrowUp', 39: 'ArrowRight', 40: 'ArrowDown',
        45: 'Insert', 46: 'Delete' };
    const keyMods = { shiftKey: false, ctrlKey: false, altKey: false };
    global.__dispatchKey = function (type, keyCode, repeat) {
        const down = type === 'keydown';
        if (keyCode === 16) keyMods.shiftKey = down;
        if (keyCode === 17) keyMods.ctrlKey = down;
        if (keyCode === 18) keyMods.altKey = down;
        let key = keyNames[keyCode];
        if (!key) key = keyCode >= 112 && keyCode <= 123 ? 'F' + (keyCode - 111) : String.fromCharCode(keyCode).toLowerCase();
        const ev = new KeyboardEvent(type, Object.assign({
            bubbles: true, cancelable: true, keyCode, which: keyCode, key, code: '', repeat, metaKey: false,
        }, keyMods));
        ev.target = document.body;
        document.body.dispatchEvent(ev);  // bubbles to document
        if (!ev._stop) {
            ev.currentTarget = global;
            global.dispatchEvent(ev);
        }
    };
})(globalThis);
