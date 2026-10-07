// Node.js / NW.js shims: Buffer, process, require('fs' | 'path' | 'crypto' | 'os' | 'zlib' | 'nw.gui' ...).
// The game sees itself installed at /game (romfs) with APPDATA at /save.
'use strict';
(function (global) {
    const sys = __native.sys;

    // ---- Buffer --------------------------------------------------------------
    const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    class Buffer extends Uint8Array {
        static from(v, enc, len) {
            if (typeof v === 'string') return Buffer._fromString(v, enc);
            if (v instanceof ArrayBuffer) return new Buffer(v, enc || 0, len === undefined ? v.byteLength - (enc || 0) : len);
            if (ArrayBuffer.isView(v)) {
                const b = new Buffer(v.byteLength);
                b.set(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
                return b;
            }
            if (Array.isArray(v)) return new Buffer(v);
            if (v && v.type === 'Buffer' && Array.isArray(v.data)) return new Buffer(v.data);
            throw new TypeError('Buffer.from: unsupported argument');
        }
        static _fromString(s, enc) {
            enc = (enc || 'utf8').toLowerCase();
            if (enc === 'utf8' || enc === 'utf-8') return Buffer.from(sys.utf8Encode(s));
            if (enc === 'base64') {
                const bin = atob(s.replace(/[^A-Za-z0-9+/=]/g, ''));
                const b = new Buffer(bin.length);
                for (let i = 0; i < bin.length; i++) b[i] = bin.charCodeAt(i);
                return b;
            }
            if (enc === 'hex') {
                const b = new Buffer(s.length >> 1);
                for (let i = 0; i < b.length; i++) b[i] = parseInt(s.substr(i * 2, 2), 16);
                return b;
            }
            const b = new Buffer(s.length);  // latin1 / binary / ascii
            for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i) & 255;
            return b;
        }
        static alloc(n, fill) { const b = new Buffer(n); if (fill) b.fill(typeof fill === 'string' ? fill.charCodeAt(0) : fill); return b; }
        static allocUnsafe(n) { return new Buffer(n); }
        static isBuffer(b) { return b instanceof Buffer; }
        static byteLength(s, enc) { return typeof s === 'string' ? Buffer._fromString(s, enc).length : s.byteLength; }
        static concat(list, total) {
            if (total === undefined) total = list.reduce((n, b) => n + b.length, 0);
            const out = new Buffer(total);
            let o = 0;
            for (const b of list) {
                if (o >= total) break;
                out.set(b.length + o > total ? b.subarray(0, total - o) : b, o);
                o += b.length;
            }
            return out;
        }
        static compare(a, b) { return a.compare(b); }
        toString(enc, start, end) {
            const v = start !== undefined || end !== undefined ? this.subarray(start || 0, end === undefined ? this.length : end) : this;
            enc = (enc || 'utf8').toLowerCase();
            if (enc === 'utf8' || enc === 'utf-8') return sys.utf8Decode(v);
            if (enc === 'hex') {
                let s = '';
                for (let i = 0; i < v.length; i++) s += (v[i] < 16 ? '0' : '') + v[i].toString(16);
                return s;
            }
            let bin = '';
            for (let i = 0; i < v.length; i += 0x8000) bin += String.fromCharCode.apply(null, v.subarray(i, i + 0x8000));
            if (enc === 'base64') return btoa(bin);
            return bin;
        }
        // Node semantics: slice shares memory
        slice(a, b) { return this.subarray(a, b); }
        subarray(a, b) {
            const u = Uint8Array.prototype.subarray.call(this, a, b);
            return new Buffer(u.buffer, u.byteOffset, u.length);
        }
        equals(o) {
            if (this.length !== o.length) return false;
            for (let i = 0; i < this.length; i++) if (this[i] !== o[i]) return false;
            return true;
        }
        compare(o) {
            const n = Math.min(this.length, o.length);
            for (let i = 0; i < n; i++) if (this[i] !== o[i]) return this[i] < o[i] ? -1 : 1;
            return this.length === o.length ? 0 : this.length < o.length ? -1 : 1;
        }
        copy(target, tStart, sStart, sEnd) {
            const src = this.subarray(sStart || 0, sEnd === undefined ? this.length : sEnd);
            target.set(src, tStart || 0);
            return src.length;
        }
        write(str, offset, len, enc) {
            const b = Buffer._fromString(str, typeof len === 'string' ? len : enc);
            const n = Math.min(b.length, this.length - (offset || 0), typeof len === 'number' ? len : Infinity);
            this.set(b.subarray(0, n), offset || 0);
            return n;
        }
        toJSON() { return { type: 'Buffer', data: Array.from(this) }; }
        _dv() { return this.__dv || (this.__dv = new DataView(this.buffer, this.byteOffset, this.byteLength)); }
        readUInt8(o) { return this[o]; }
        readInt8(o) { return this._dv().getInt8(o); }
        readUInt16LE(o) { return this._dv().getUint16(o, true); }
        readUInt16BE(o) { return this._dv().getUint16(o, false); }
        readInt16LE(o) { return this._dv().getInt16(o, true); }
        readUInt32LE(o) { return this._dv().getUint32(o, true); }
        readUInt32BE(o) { return this._dv().getUint32(o, false); }
        readInt32LE(o) { return this._dv().getInt32(o, true); }
        readInt32BE(o) { return this._dv().getInt32(o, false); }
        readFloatLE(o) { return this._dv().getFloat32(o, true); }
        readDoubleLE(o) { return this._dv().getFloat64(o, true); }
        writeUInt8(v, o) { this[o] = v; return o + 1; }
        writeUInt16LE(v, o) { this._dv().setUint16(o, v, true); return o + 2; }
        writeUInt32LE(v, o) { this._dv().setUint32(o, v, true); return o + 4; }
        writeInt32LE(v, o) { this._dv().setInt32(o, v, true); return o + 4; }
        writeUInt32BE(v, o) { this._dv().setUint32(o, v, false); return o + 4; }
    }
    global.Buffer = Buffer;

    // ---- path ----------------------------------------------------------------
    // Accepts both separators (the game mixes "/" and "\\"), returns "/" paths.
    // The game assumes Windows paths with a drive letter, so the virtual roots are
    // presented as C:\game and C:\save; "C:" is treated as the root marker.
    function splitPath(p) { return String(p).replace(/\\/g, '/'); }
    const DRIVE = /^[A-Za-z]:(?=\/|$)/;
    function normalize(p) {
        p = splitPath(p);
        const drive = (DRIVE.exec(p) || [''])[0];
        p = p.slice(drive.length);
        const abs = p.startsWith('/') || !!drive;
        const out = [];
        for (const s of p.split('/')) {
            if (!s || s === '.') continue;
            if (s === '..') { if (out.length && out[out.length - 1] !== '..') out.pop(); else if (!abs) out.push('..'); }
            else out.push(s);
        }
        let r = (abs ? '/' : '') + out.join('/');
        if (p.endsWith('/') && r !== '/') r += '/';
        return drive + r || '.';
    }
    const path = {
        sep: '\\', delimiter: ';',
        normalize,
        join(...a) { return normalize(a.filter((x) => x !== '').join('/')); },
        resolve(...a) {
            let r = '';
            for (let i = a.length - 1; i >= 0 && !path.isAbsolute(r); i--) r = splitPath(a[i]) + (r ? '/' + r : '');
            if (!path.isAbsolute(r)) r = 'C:/game/' + r;
            return normalize(r).replace(/(.)\/$/, '$1');
        },
        dirname(p) {
            p = splitPath(p).replace(/\/+$/, '');
            const i = p.lastIndexOf('/');
            if (i < 0) return DRIVE.test(p) ? p : '.';
            return i === 0 ? '/' : DRIVE.test(p) && i === 2 ? p.slice(0, 3) : p.slice(0, i);
        },
        basename(p, ext) {
            let b = splitPath(p).replace(/\/+$/, '');
            b = b.slice(b.lastIndexOf('/') + 1);
            if (ext && b.endsWith(ext)) b = b.slice(0, -ext.length);
            return b;
        },
        extname(p) {
            const b = path.basename(p);
            const i = b.lastIndexOf('.');
            return i <= 0 ? '' : b.slice(i);
        },
        relative(from, to) {
            const f = path.resolve(from).toLowerCase().split('/').filter(Boolean), t0 = path.resolve(to).split('/').filter(Boolean);
            const t = t0.map((x) => x.toLowerCase());
            let i = 0;
            while (i < f.length && i < t.length && f[i] === t[i]) i++;
            return f.slice(i).map(() => '..').concat(t0.slice(i)).join('/');
        },
        isAbsolute(p) { return /^([\\/]|[A-Za-z]:([\\/]|$))/.test(String(p)); },
        parse(p) {
            const base = path.basename(p), ext = path.extname(p);
            return { root: path.isAbsolute(p) ? '/' : '', dir: path.dirname(p), base, ext, name: ext ? base.slice(0, -ext.length) : base };
        },
        format(o) { return (o.dir ? o.dir + '/' : '') + (o.base || (o.name || '') + (o.ext || '')); },
    };
    path.posix = path;
    path.win32 = path;

    // ---- fs -------------------------------------------------------------------
    function enoent(op, p) {
        const e = new Error("ENOENT: no such file or directory, " + op + " '" + p + "'");
        e.code = 'ENOENT';
        e.errno = -2;
        e.syscall = op;
        e.path = p;
        return e;
    }
    function vpath(p) {
        if (p instanceof URL) p = p.pathname;
        p = splitPath(p).replace(DRIVE, '');
        if (!p.startsWith('/')) p = '/game/' + p;  // cwd is the game root
        return p;
    }
    class Stats {
        constructor(st) {
            this.size = st.size;
            this.mtimeMs = this.atimeMs = this.ctimeMs = this.birthtimeMs = st.mtime;
            this.mtime = this.atime = this.ctime = this.birthtime = new Date(st.mtime);
            this._dir = st.dir;
            this.mode = st.dir ? 0o40755 : 0o100644;
        }
        isDirectory() { return this._dir; }
        isFile() { return !this._dir; }
        isSymbolicLink() { return false; }
    }
    function encodingOf(opts) { return typeof opts === 'string' ? opts : opts && opts.encoding; }
    const fs = {
        existsSync(p) { return !!sys.stat(vpath(p)); },
        statSync(p, opts) {
            const st = sys.stat(vpath(p));
            if (!st) { if (opts && opts.throwIfNoEntry === false) return undefined; throw enoent('stat', p); }
            return new Stats(st);
        },
        lstatSync(p, opts) { return fs.statSync(p, opts); },
        accessSync(p) { if (!sys.stat(vpath(p))) throw enoent('access', p); },
        readFileSync(p, opts) {
            const ab = sys.readFile(vpath(p));
            if (!ab) throw enoent('open', p);
            const b = Buffer.from(ab);
            const enc = encodingOf(opts);
            return enc ? b.toString(enc) : b;
        },
        writeFileSync(p, data, opts) {
            const enc = encodingOf(opts);
            let payload = data;
            if (typeof data === 'string' && enc && !/^utf-?8$/i.test(enc)) payload = Buffer._fromString(data, enc);
            else if (typeof data !== 'string' && !ArrayBuffer.isView(data) && !(data instanceof ArrayBuffer)) payload = String(data);
            if (!sys.writeFile(vpath(p), payload)) {
                const e = new Error("EACCES: cannot write '" + p + "'");
                e.code = 'EACCES';
                throw e;
            }
        },
        appendFileSync(p, data) {
            const ab = sys.readFile(vpath(p));
            const prev = ab ? Buffer.from(ab) : Buffer.alloc(0);
            fs.writeFileSync(p, Buffer.concat([prev, typeof data === 'string' ? Buffer.from(data) : Buffer.from(data)]));
        },
        mkdirSync(p) { sys.mkdir(vpath(p)); },
        readdirSync(p, opts) {
            const names = sys.readdir(vpath(p));
            if (!names) throw enoent('scandir', p);
            names.sort();
            if (opts && opts.withFileTypes) {
                return names.map((n) => {
                    const st = sys.stat(vpath(p) + '/' + n) || { dir: false };
                    return { name: n, isDirectory: () => st.dir, isFile: () => !st.dir, isSymbolicLink: () => false };
                });
            }
            return names;
        },
        unlinkSync(p) { if (!sys.unlink(vpath(p))) throw enoent('unlink', p); },
        rmdirSync(p) { sys.unlink(vpath(p)); },
        rmSync(p) { sys.unlink(vpath(p)); },
        renameSync(a, b) { if (!sys.rename(vpath(a), vpath(b))) throw enoent('rename', a); },
        copyFileSync(a, b) { fs.writeFileSync(b, fs.readFileSync(a)); },
        constants: { F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1 },
    };
    // async variants (callback style) built on the sync ones
    for (const name of ['readFile', 'writeFile', 'stat', 'lstat', 'access', 'mkdir', 'readdir', 'unlink', 'rename', 'copyFile', 'appendFile']) {
        const sync = fs[name + 'Sync'];
        fs[name] = function (...args) {
            const cb = typeof args[args.length - 1] === 'function' ? args.pop() : null;
            global.__rt.queueTask(() => {
                let r, err = null;
                try { r = sync.apply(fs, args); } catch (e) { err = e; }
                if (cb) cb(err, r);
            });
        };
    }
    fs.exists = (p, cb) => global.__rt.queueTask(() => cb(fs.existsSync(p)));
    fs.promises = {};
    for (const name of ['readFile', 'writeFile', 'stat', 'lstat', 'access', 'mkdir', 'readdir', 'unlink', 'rename', 'copyFile', 'appendFile']) {
        const sync = fs[name + 'Sync'];
        fs.promises[name] = (...args) => new Promise((res, rej) => {
            global.__rt.queueTask(() => { try { res(sync.apply(fs, args)); } catch (e) { rej(e); } });
        });
    }

    // ---- crypto / zlib / os ------------------------------------------------------
    const crypto = {
        createHash(alg) {
            if (String(alg).toLowerCase() !== 'sha256') throw new Error('crypto: only sha256 is available');
            const parts = [];
            return {
                update(d, enc) { parts.push(typeof d === 'string' ? Buffer.from(d, enc) : Buffer.from(d)); return this; },
                digest(enc) {
                    const hex = sys.sha256(Buffer.concat(parts));
                    return enc === 'hex' ? hex : enc ? Buffer.from(hex, 'hex').toString(enc) : Buffer.from(hex, 'hex');
                },
            };
        },
        randomBytes(n) { const b = Buffer.alloc(n); for (let i = 0; i < n; i++) b[i] = (Math.random() * 256) | 0; return b; },
        getRandomValues(a) { for (let i = 0; i < a.length; i++) a[i] = (Math.random() * 4294967296) >>> 0; return a; },
    };
    if (!global.crypto) global.crypto = { getRandomValues: crypto.getRandomValues, subtle: undefined };

    const zlib = {
        inflateSync(b) { return Buffer.from(sys.inflate(b)); },
        unzipSync(b) { return Buffer.from(sys.inflate(b)); },
    };

    const os = {
        platform: () => 'win32', type: () => 'Windows_NT', arch: () => 'x64', release: () => '10.0',
        hostname: () => 'nintendo-switch', homedir: () => 'C:\\save', tmpdir: () => 'C:\\save\\tmp',
        userInfo: () => ({ username: 'player', homedir: '/save', uid: -1, gid: -1, shell: null }),
        networkInterfaces: () => ({}), cpus: () => [], totalmem: () => 4e9, freemem: () => 2e9,
        EOL: '\r\n', endianness: () => 'LE', uptime: () => sys.now() / 1000,
    };

    // ---- process -----------------------------------------------------------------
    const startNs = sys.now();
    function hrtime(prev) {
        const ms = sys.now() - startNs;
        let s = Math.floor(ms / 1000), ns = Math.floor((ms % 1000) * 1e6);
        if (prev) {
            s -= prev[0];
            ns -= prev[1];
            if (ns < 0) { s--; ns += 1e9; }
        }
        return [s, ns];
    }
    hrtime.bigint = () => BigInt(Math.floor((sys.now() - startNs) * 1e6));
    const procListeners = {};
    global.process = {
        platform: 'win32', arch: 'x64', pid: 1, title: 'nw', version: 'v12.13.0',
        versions: { node: '12.13.0', nw: '0.42.6', 'nw-flavor': 'normal', chromium: '78.0.3904.97', v8: '7.8' },
        env: { APPDATA: 'C:\\save', LOCALAPPDATA: 'C:\\save', USERPROFILE: 'C:\\save', HOME: 'C:\\save', TEMP: 'C:\\save\\tmp' },
        argv: ['C:\\game\\Game.exe', 'C:\\game'],
        execArgv: [],
        execPath: 'C:\\game\\Game.exe',
        mainModule: { filename: 'C:\\game\\www\\index.html' },
        cwd: () => 'C:\\game',
        chdir() {},
        hrtime,
        uptime: () => (sys.now() - startNs) / 1000,
        memoryUsage: () => ({ rss: sys.memoryUsage(), heapUsed: sys.memoryUsage(), heapTotal: sys.memoryUsage(), external: 0 }),
        nextTick: (fn, ...args) => queueMicrotask(() => fn(...args)),
        on(ev, fn) { (procListeners[ev] || (procListeners[ev] = [])).push(fn); return this; },
        once(ev, fn) { return this.on(ev, fn); },
        off() { return this; },
        removeListener() { return this; },
        removeAllListeners() { return this; },
        emit() { return false; },
        exit() { sys.quit(); },
        stdout: { write(s) { sys.log(String(s).replace(/\n$/, '')); } },
        stderr: { write(s) { sys.log(String(s).replace(/\n$/, '')); } },
    };

    // ---- nw.gui ------------------------------------------------------------------
    const win = {
        x: 0, y: 0, width: 1280, height: 720, title: '', isFullscreen: true, isKioskMode: false, menu: null,
        on() { return win; }, once() { return win; }, removeListener() { return win; }, removeAllListeners() { return win; },
        close() { sys.quit(); }, focus() {}, blur() {}, show() {}, hide() {}, minimize() {}, maximize() {}, restore() {},
        enterFullscreen() {}, leaveFullscreen() {}, toggleFullscreen() {}, enterKioskMode() {}, leaveKioskMode() {},
        toggleKioskMode() {}, showDevTools() {}, closeDevTools() {}, isDevToolsOpen: () => false,
        setPosition() {}, moveTo() {}, moveBy() {}, resizeTo() {}, resizeBy() {}, setResizable() {}, setAlwaysOnTop() {},
        setMinimumSize() {}, setMaximumSize() {}, reload() {}, reloadIgnoringCache() {}, requestAttention() {},
        get window() { return global; },
    };
    const nwGui = {
        Window: { get: () => win, open: () => win },
        App: {
            argv: [], fullArgv: [], dataPath: '/save', manifest: {},
            quit() { sys.quit(); }, closeAllWindows() { sys.quit(); }, clearCache() {},
            on() {}, registerGlobalHotKey() {}, unregisterGlobalHotKey() {},
        },
        Screen: { Init() {}, screens: [{ bounds: { x: 0, y: 0, width: 1280, height: 720 }, scaleFactor: 1 }], on() {} },
        Shell: { openExternal() {}, openItem() {}, showItemInFolder() {} },
        Menu: function Menu() { return { append() {}, createMacBuiltin() {}, items: [] }; },
        MenuItem: function MenuItem() { return {}; },
        Clipboard: { get: () => ({ get: () => '', set() {}, clear() {} }) },
    };
    global.nw = nwGui;

    // Steam is not available on the console; any API call is a harmless no-op.
    const noSteam = new Proxy({}, {
        get(t, k) {
            if (k === 'then') return undefined;
            if (k === 'initAPI' || k === 'init') return () => false;
            return function () {
                const cb = Array.prototype.slice.call(arguments).reverse().find((a) => typeof a === 'function');
                void cb;
                return undefined;
            };
        },
    });

    const modules = {
        fs, path, crypto, zlib, os, 'nw.gui': nwGui, buffer: { Buffer },
        child_process: { exec(cmd, opts, cb) { cb = typeof opts === 'function' ? opts : cb; if (cb) global.__rt.queueTask(() => cb(new Error('not supported'))); } },
        events: { EventEmitter: class EventEmitter { on() { return this; } emit() { return false; } removeListener() { return this; } } },
        util: { inspect: (o) => { try { return JSON.stringify(o); } catch (e) { return String(o); } }, format: (...a) => a.join(' ') },
    };
    global.require = function (name) {
        name = String(name);
        if (Object.prototype.hasOwnProperty.call(modules, name)) return modules[name];
        if (/greenworks/i.test(name)) return noSteam;
        const e = new Error("Cannot find module '" + name + "'");
        e.code = 'MODULE_NOT_FOUND';
        throw e;
    };
    global.require.cache = {};
    global.require.resolve = (n) => n;
    global.global = global;
})(globalThis);
