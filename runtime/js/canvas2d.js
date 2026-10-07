// CanvasRenderingContext2D over the native plutovg surfaces.
'use strict';
(function (global) {
    const cv = __native.canvas;

    // ---- colours -----------------------------------------------------------
    const NAMED = {
        transparent: [0, 0, 0, 0], black: [0, 0, 0, 1], white: [1, 1, 1, 1], red: [1, 0, 0, 1], lime: [0, 1, 0, 1],
        green: [0, 128 / 255, 0, 1], blue: [0, 0, 1, 1], yellow: [1, 1, 0, 1], cyan: [0, 1, 1, 1], aqua: [0, 1, 1, 1],
        magenta: [1, 0, 1, 1], fuchsia: [1, 0, 1, 1], gray: [128 / 255, 128 / 255, 128 / 255, 1],
        grey: [128 / 255, 128 / 255, 128 / 255, 1], silver: [192 / 255, 192 / 255, 192 / 255, 1],
        maroon: [128 / 255, 0, 0, 1], olive: [128 / 255, 128 / 255, 0, 1], navy: [0, 0, 128 / 255, 1],
        purple: [128 / 255, 0, 128 / 255, 1], teal: [0, 128 / 255, 128 / 255, 1], orange: [1, 165 / 255, 0, 1],
        pink: [1, 192 / 255, 203 / 255, 1], brown: [165 / 255, 42 / 255, 42 / 255, 1], gold: [1, 215 / 255, 0, 1],
        aliceblue: [240 / 255, 248 / 255, 1, 1], darkgray: [169 / 255, 169 / 255, 169 / 255, 1],
        lightgray: [211 / 255, 211 / 255, 211 / 255, 1], darkred: [139 / 255, 0, 0, 1],
    };
    const colorCache = new Map();
    function hsl2rgb(h, s, l) {
        h = ((h % 360) + 360) % 360 / 360;
        const q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        const f = (t) => {
            t = (t + 1) % 1;
            return t < 1 / 6 ? p + (q - p) * 6 * t : t < 1 / 2 ? q : t < 2 / 3 ? p + (q - p) * (2 / 3 - t) * 6 : p;
        };
        return [f(h + 1 / 3), f(h), f(h - 1 / 3)];
    }
    function parseColor(str) {
        if (typeof str !== 'string') return null;
        let c = colorCache.get(str);
        if (c !== undefined) return c;
        const s = str.trim().toLowerCase();
        c = null;
        if (NAMED[s]) c = NAMED[s];
        else if (s[0] === '#') {
            const h = s.slice(1);
            const x = (i, n) => parseInt(n === 1 ? h[i] + h[i] : h.substr(i, 2), 16) / 255;
            if (h.length === 3 || h.length === 4) c = [x(0, 1), x(1, 1), x(2, 1), h.length === 4 ? x(3, 1) : 1];
            else if (h.length === 6 || h.length === 8) c = [x(0, 2), x(2, 2), x(4, 2), h.length === 8 ? x(6, 2) : 1];
        } else {
            const m = /^(rgba?|hsla?)\(\s*([^)]*)\)$/.exec(s);
            if (m) {
                const a = m[2].split(/[\s,/]+/).filter(Boolean);
                const num = (v, scale) => (v.endsWith('%') ? parseFloat(v) / 100 : parseFloat(v) / scale);
                const alpha = a.length > 3 ? num(a[3], 1) : 1;
                if (m[1][0] === 'r') c = [num(a[0], 255), num(a[1], 255), num(a[2], 255), alpha];
                else c = hsl2rgb(parseFloat(a[0]), num(a[1], 100), num(a[2], 100)).concat([alpha]);
                c = c.map((v) => (isNaN(v) ? 0 : Math.max(0, Math.min(1, v))));
            }
        }
        if (colorCache.size > 4096) colorCache.clear();
        colorCache.set(str, c);
        return c;
    }

    // ---- fonts ----------------------------------------------------------------
    const fontCache = new Map();
    function parseFont(str) {
        let f = fontCache.get(str);
        if (f) return f;
        const m = /^\s*((?:(?:italic|oblique|normal|bold|bolder|lighter|small-caps|[1-9]00)\s+)*)(\d+(?:\.\d+)?)(px|pt|em|rem)?(?:\s*\/\s*\S+)?\s+(.+?)\s*$/i.exec(str);
        if (!m) return null;
        let size = parseFloat(m[2]);
        if (m[3] === 'pt') size *= 4 / 3;
        else if (m[3] === 'em' || m[3] === 'rem') size *= 16;
        const pre = m[1].toLowerCase();
        f = { size, family: m[4], bold: /bold|[6-9]00/.test(pre), italic: /italic|oblique/.test(pre), str };
        fontCache.set(str, f);
        return f;
    }

    // ---- composite ops ----------------------------------------------------------
    const OPS = {
        'source-over': 0, copy: 1, 'destination-in': 2, 'destination-out': 3, 'source-atop': 4, 'source-in': 5,
        'source-out': 6, 'destination-over': 7, 'destination-atop': 8, xor: 9, lighter: 10, plus: 10,
    };
    const warnedOps = new Set();

    const ALIGN = { left: 0, start: 0, center: 1, right: 2, end: 2 };
    const BASELINE = { alphabetic: 0, top: 1, middle: 2, bottom: 3, hanging: 4, ideographic: 5 };
    const CAP = { butt: 0, round: 1, square: 2 };
    const JOIN = { miter: 0, round: 1, bevel: 2 };

    class CanvasGradient {
        constructor(kind, args) {
            this._kind = kind;
            this._args = args;
            this._stops = [];
            this._native = null;
        }
        addColorStop(offset, color) {
            const c = parseColor(color) || [0, 0, 0, 0];
            this._stops.push([+offset, c]);
            this._native = null;
        }
        _get() {
            if (!this._native) {
                const st = this._stops.slice().sort((a, b) => a[0] - b[0]);
                const arr = new Float32Array(st.length * 5);
                st.forEach((s, i) => arr.set([s[0], s[1][0], s[1][1], s[1][2], s[1][3]], i * 5));
                this._native = this._kind === 'linear'
                    ? cv.linearGradient(...this._args, arr)
                    : cv.radialGradient(...this._args, arr);
            }
            return this._native;
        }
    }
    class CanvasPattern {
        constructor(src) { this._src = src; }
        setTransform() {}
    }

    class ImageData {
        constructor(a, b, c) {
            if (a instanceof Uint8ClampedArray) {
                this.data = a;
                this.width = b;
                this.height = c || a.length / 4 / b;
            } else {
                this.width = a;
                this.height = b;
                this.data = new Uint8ClampedArray(a * b * 4);
            }
        }
    }

    class TextMetrics {
        constructor(m) {
            this.width = m[0];
            this.actualBoundingBoxAscent = this.fontBoundingBoxAscent = m[1];
            this.actualBoundingBoxDescent = this.fontBoundingBoxDescent = m[2];
            this.actualBoundingBoxLeft = 0;
            this.actualBoundingBoxRight = m[0];
        }
    }

    function defaultState() {
        return {
            fill: '#000000', stroke: '#000000', alpha: 1, op: 'source-over', lineWidth: 1, lineCap: 'butt',
            lineJoin: 'miter', miterLimit: 10, font: '10px sans-serif', textAlign: 'start', textBaseline: 'alphabetic',
            smoothing: true, shadowColor: 'rgba(0, 0, 0, 0)', shadowBlur: 0, shadowOffsetX: 0, shadowOffsetY: 0,
            filter: 'none', dash: [], dashOffset: 0,
        };
    }

    const BLACK = [0, 0, 0, 1];
    function surfaceOf(src) {
        if (!src) return null;
        if (src._surface) return src._surface;
        if (src instanceof global.HTMLCanvasElement) return src._ensureSurface();
        return null;
    }

    class CanvasRenderingContext2D {
        constructor(canvas) {
            this.canvas = canvas;
            this._s = canvas._ensureSurface();
            this._reset();
        }
        _reset() {
            this._st = defaultState();
            this._stack = [];
            this._nativeFont = null;
        }
        _paint(style) {
            if (typeof style === 'string') return parseColor(style) || BLACK;
            if (style instanceof CanvasGradient) return style._get();
            return [0, 0, 0, 0];  // patterns are not supported
        }
        _syncFont() {
            const f = parseFont(this._st.font) || parseFont('10px sans-serif');
            if (this._nativeFont !== f.str) {
                this._s.setFont(f.family, f.size, f.bold, f.italic);
                this._nativeFont = f.str;
            }
        }
        _syncLine() {
            const st = this._st;
            this._s.setLine(st.lineWidth, CAP[st.lineCap] || 0, JOIN[st.lineJoin] || 0, st.miterLimit);
        }

        // state
        save() {
            this._stack.push(Object.assign({}, this._st));
            this._s.save();
        }
        restore() {
            if (!this._stack.length) return;
            this._st = this._stack.pop();
            this._s.restore();
        }
        get fillStyle() { return this._st.fill; }
        set fillStyle(v) { if (typeof v === 'string' || v instanceof CanvasGradient || v instanceof CanvasPattern) this._st.fill = v; }
        get strokeStyle() { return this._st.stroke; }
        set strokeStyle(v) { if (typeof v === 'string' || v instanceof CanvasGradient || v instanceof CanvasPattern) this._st.stroke = v; }
        get globalAlpha() { return this._st.alpha; }
        set globalAlpha(v) {
            v = +v;
            if (!(v >= 0 && v <= 1)) return;
            this._st.alpha = v;
            this._s.setAlpha(v);
        }
        get globalCompositeOperation() { return this._st.op; }
        set globalCompositeOperation(v) {
            let code = OPS[v];
            if (code === undefined) {
                if (!warnedOps.has(v)) {
                    warnedOps.add(v);
                    console.warn('canvas: composite op "' + v + '" not supported, using source-over');
                }
                code = 0;
            }
            this._st.op = v;
            this._s.setOp(code);
        }
        get lineWidth() { return this._st.lineWidth; }
        set lineWidth(v) { if (v > 0) { this._st.lineWidth = +v; this._syncLine(); } }
        get lineCap() { return this._st.lineCap; }
        set lineCap(v) { if (v in CAP) { this._st.lineCap = v; this._syncLine(); } }
        get lineJoin() { return this._st.lineJoin; }
        set lineJoin(v) { if (v in JOIN) { this._st.lineJoin = v; this._syncLine(); } }
        get miterLimit() { return this._st.miterLimit; }
        set miterLimit(v) { if (v > 0) { this._st.miterLimit = +v; this._syncLine(); } }
        get font() { return this._st.font; }
        set font(v) { if (parseFont(String(v))) this._st.font = String(v); }
        get textAlign() { return this._st.textAlign; }
        set textAlign(v) { if (v in ALIGN) this._st.textAlign = v; }
        get textBaseline() { return this._st.textBaseline; }
        set textBaseline(v) { if (v in BASELINE) this._st.textBaseline = v; }
        get imageSmoothingEnabled() { return this._st.smoothing; }
        set imageSmoothingEnabled(v) { this._st.smoothing = !!v; }
        get shadowColor() { return this._st.shadowColor; }
        set shadowColor(v) { this._st.shadowColor = v; }
        get shadowBlur() { return this._st.shadowBlur; }
        set shadowBlur(v) { this._st.shadowBlur = v; }
        get shadowOffsetX() { return this._st.shadowOffsetX; }
        set shadowOffsetX(v) { this._st.shadowOffsetX = v; }
        get shadowOffsetY() { return this._st.shadowOffsetY; }
        set shadowOffsetY(v) { this._st.shadowOffsetY = v; }
        get filter() { return this._st.filter; }
        set filter(v) { this._st.filter = v; }
        get lineDashOffset() { return this._st.dashOffset; }
        set lineDashOffset(v) { this._st.dashOffset = v; }
        setLineDash(d) { this._st.dash = d.slice(); }
        getLineDash() { return this._st.dash.slice(); }

        // transforms
        setTransform(a, b, c, d, e, f) {
            if (a && typeof a === 'object') ({ a, b, c, d, e, f } = a);
            this._s.setTransform(a, b, c, d, e, f);
        }
        resetTransform() { this._s.setTransform(1, 0, 0, 1, 0, 0); }
        transform(a, b, c, d, e, f) { this._s.transform(a, b, c, d, e, f); }
        translate(x, y) { this._s.translate(x, y); }
        scale(x, y) { this._s.scale(x, y); }
        rotate(r) { this._s.rotate(r); }
        getTransform() {
            const m = this._s.getTransform();
            return { a: m[0], b: m[1], c: m[2], d: m[3], e: m[4], f: m[5], is2D: true };
        }

        // paths
        beginPath() { this._s.beginPath(); }
        closePath() { this._s.closePath(); }
        moveTo(x, y) { this._s.moveTo(x, y); }
        lineTo(x, y) { this._s.lineTo(x, y); }
        rect(x, y, w, h) { this._s.rect(x, y, w, h); }
        arc(x, y, r, a0, a1, ccw) { this._s.arc(x, y, r, a0, a1, !!ccw); }
        arcTo(x1, y1, x2, y2) { this._s.lineTo(x1, y1); this._s.lineTo(x2, y2); }
        ellipse(x, y, rx, ry, rot, a0, a1, ccw) {
            this._s.save();
            this._s.translate(x, y);
            this._s.rotate(rot);
            this._s.scale(rx, ry);
            this._s.arc(0, 0, 1, a0, a1, !!ccw);
            this._s.restore();
        }
        bezierCurveTo(a, b, c, d, e, f) { this._s.bezierCurveTo(a, b, c, d, e, f); }
        quadraticCurveTo(a, b, c, d) { this._s.quadraticCurveTo(a, b, c, d); }
        fill(rule) { this._s.fill(this._paint(this._st.fill), rule === 'evenodd'); }
        stroke() { this._s.stroke(this._paint(this._st.stroke)); }
        clip(rule) { this._s.clip(rule === 'evenodd'); }
        isPointInPath() { return false; }
        isPointInStroke() { return false; }

        // rects
        fillRect(x, y, w, h) { this._s.fillRect(x, y, w, h, this._paint(this._st.fill)); }
        strokeRect(x, y, w, h) { this._s.strokeRect(x, y, w, h, this._paint(this._st.stroke)); }
        clearRect(x, y, w, h) { this._s.clearRect(x, y, w, h); }

        // images
        drawImage(img, a, b, c, d, e, f, g, h) {
            const src = surfaceOf(img);
            if (!src) return;
            const sw0 = src.width, sh0 = src.height;
            if (arguments.length === 3) this._s.drawImage(src, 0, 0, sw0, sh0, a, b, sw0, sh0);
            else if (arguments.length === 5) this._s.drawImage(src, 0, 0, sw0, sh0, a, b, c, d);
            else this._s.drawImage(src, a, b, c, d, e, f, g, h);
        }
        createPattern(img) { return new CanvasPattern(img); }
        createLinearGradient(x0, y0, x1, y1) { return new CanvasGradient('linear', [x0, y0, x1, y1]); }
        createRadialGradient(x0, y0, r0, x1, y1, r1) { return new CanvasGradient('radial', [x0, y0, r0, x1, y1, r1]); }

        // text
        _text(text, x, y, maxWidth, stroke) {
            this._syncFont();
            this._s.drawText(String(text), x, y, maxWidth === undefined ? -1 : +maxWidth,
                ALIGN[this._st.textAlign], BASELINE[this._st.textBaseline],
                this._paint(stroke ? this._st.stroke : this._st.fill), stroke);
        }
        fillText(text, x, y, maxWidth) { this._text(text, x, y, maxWidth, false); }
        strokeText(text, x, y, maxWidth) { this._text(text, x, y, maxWidth, true); }
        measureText(text) {
            this._syncFont();
            return new TextMetrics(this._s.measureText(String(text)));
        }

        // pixels
        getImageData(x, y, w, h) {
            const ab = this._s.getImageData(x | 0, y | 0, w | 0, h | 0);
            return new ImageData(new Uint8ClampedArray(ab), w | 0, h | 0);
        }
        putImageData(img, dx, dy, dirtyX, dirtyY, dirtyW, dirtyH) {
            if (dirtyX === undefined) {
                this._s.putImageData(img.data, dx | 0, dy | 0, img.width, img.height);
                return;
            }
            // crop to the dirty rectangle
            const x0 = Math.max(0, dirtyX | 0), y0 = Math.max(0, dirtyY | 0);
            const x1 = Math.min(img.width, (dirtyX + dirtyW) | 0), y1 = Math.min(img.height, (dirtyY + dirtyH) | 0);
            if (x1 <= x0 || y1 <= y0) return;
            const w = x1 - x0, out = new Uint8ClampedArray(w * (y1 - y0) * 4);
            for (let y = y0; y < y1; y++) out.set(img.data.subarray((y * img.width + x0) * 4, (y * img.width + x1) * 4), (y - y0) * w * 4);
            this._s.putImageData(out, (dx + x0) | 0, (dy + y0) | 0, w, y1 - y0);
        }
        createImageData(w, h) {
            if (w && typeof w === 'object') return new ImageData(w.width, w.height);
            return new ImageData(w, h);
        }
    }

    Object.assign(global, { CanvasRenderingContext2D, CanvasGradient, CanvasPattern, ImageData, TextMetrics });
    global.__rt.parseColor = parseColor;
})(globalThis);
