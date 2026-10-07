// Web Audio API subset over the native mixer, plus the native `stbvorbis` decoder.
'use strict';
(function (global) {
    const A = __native.audio;
    const rt = global.__rt;
    const { EventTarget } = rt;

    let nodeSeq = 1;
    const playing = new Map();  // id -> source node, kept alive until ended

    global.__audioEnded = function (id) {
        const n = playing.get(id);
        if (!n) return;
        playing.delete(id);
        n._ended = true;
        n.dispatchEvent(new Event('ended'));
    };

    class AudioParam {
        constructor(native, def) {
            this._n = native;
            this.defaultValue = def;
            this.minValue = -3.4e38;
            this.maxValue = 3.4e38;
        }
        get value() { return A.param(this._n, -1, 0, 0, 0); }
        set value(v) { A.param(this._n, 0, +v, 0, 0); }
        setValueAtTime(v, t) { A.param(this._n, 1, +v, +t, 0); return this; }
        linearRampToValueAtTime(v, t) { A.param(this._n, 2, +v, +t, 0); return this; }
        exponentialRampToValueAtTime(v, t) { A.param(this._n, 3, +v, +t, 0); return this; }
        setTargetAtTime(v, t, tc) { A.param(this._n, 4, +v, +t, +tc); return this; }
        cancelScheduledValues(t) { A.param(this._n, 5, 0, +t, 0); return this; }
        cancelAndHoldAtTime(t) { return this.cancelScheduledValues(t); }
        setValueCurveAtTime(curve, t, dur) {
            if (curve.length) this.setValueAtTime(curve[0], t).linearRampToValueAtTime(curve[curve.length - 1], t + dur);
            return this;
        }
    }

    class AudioNode extends EventTarget {
        constructor(ctx, type) {
            super();
            this.context = ctx;
            this._id = nodeSeq++;
            this._n = type === 0 ? A.destination() : A.createNode(type, this._id);
            this.numberOfInputs = 1;
            this.numberOfOutputs = 1;
            this.channelCount = 2;
        }
        connect(dest) {
            if (dest instanceof AudioNode) A.connect(this._n, dest._n);
            return dest;
        }
        disconnect() { A.connect(this._n); }
    }

    class GainNode extends AudioNode {
        constructor(ctx) { super(ctx, 1); this.gain = new AudioParam(this._n, 1); }
    }

    class PannerNode extends AudioNode {
        constructor(ctx) {
            super(ctx, 2);
            this.panningModel = 'equalpower';
            this.distanceModel = 'inverse';
            this.refDistance = 1;
            this.maxDistance = 10000;
            this.rolloffFactor = 1;
            this.coneInnerAngle = 360;
            this.coneOuterAngle = 0;
            this.coneOuterGain = 0;
            this._pos = [0, 0, 0];
            const self = this;
            const axis = (i) => ({ get value() { return self._pos[i]; }, set value(v) { self._pos[i] = +v; self._apply(); },
                setValueAtTime(v) { this.value = v; return this; } });
            this.positionX = axis(0);
            this.positionY = axis(1);
            this.positionZ = axis(2);
            this._apply();
        }
        _apply() { A.setPosition(this._n, this._pos[0], this._pos[1], this._pos[2]); }
        setPosition(x, y, z) { this._pos = [+x, +y, +z]; this._apply(); }
        setOrientation() {}
    }

    class StereoPannerNode extends PannerNode {
        constructor(ctx) {
            super(ctx);
            const self = this;
            let pan = 0;
            this.pan = {
                get value() { return pan; },
                set value(v) { pan = Math.max(-1, Math.min(1, +v)); self.setPosition(pan, 0, 1 - Math.abs(pan)); },
                setValueAtTime(v) { this.value = v; return this; },
                linearRampToValueAtTime(v) { this.value = v; return this; },
            };
        }
    }

    class AudioBuffer {
        constructor(opts, native) {
            this._b = native || A.createBuffer(opts.numberOfChannels || 1, opts.length, opts.sampleRate);
            const info = A.bufferInfo(this._b);
            this.numberOfChannels = info[0];
            this.length = info[1];
            this.sampleRate = info[2];
            this.duration = this.length / this.sampleRate;
            this._views = [];
        }
        getChannelData(ch) {
            return this._views[ch] || (this._views[ch] = new Float32Array(A.channelData(this._b, ch)));
        }
        copyToChannel(src, ch, start) { A.copyToChannel(this._b, src, ch, start | 0); }
        copyFromChannel(dst, ch, start) {
            start = start | 0;
            dst.set(this.getChannelData(ch).subarray(start, start + dst.length));
        }
    }

    class AudioBufferSourceNode extends AudioNode {
        constructor(ctx) {
            super(ctx, 3);
            this.playbackRate = new AudioParam(this._n, 1);
            this.detune = new AudioParam(A.createNode(1, 0), 0);
            this._buffer = null;
            this._loop = false;
            this._loopStart = 0;
            this._loopEnd = 0;
            this._started = false;
        }
        _sync() { A.setSource(this._n, this._buffer ? this._buffer._b : null, this._loop, this._loopStart, this._loopEnd); }
        get buffer() { return this._buffer; }
        set buffer(b) { this._buffer = b || null; this._sync(); }
        get loop() { return this._loop; }
        set loop(v) { this._loop = !!v; this._sync(); }
        get loopStart() { return this._loopStart; }
        set loopStart(v) { this._loopStart = +v || 0; this._sync(); }
        get loopEnd() { return this._loopEnd; }
        set loopEnd(v) { this._loopEnd = +v || 0; this._sync(); }
        start(when, offset, duration) {
            if (this._started) throw new Error('InvalidStateError: start() called twice');
            this._started = true;
            playing.set(this._id, this);
            A.start(this._n, +when || 0, +offset || 0, duration === undefined ? -1 : +duration);
        }
        noteOn(when) { this.start(when); }
        stop(when) {
            if (!this._started) return;
            A.stop(this._n, +when || 0);
        }
        noteOff(when) { this.stop(when); }
    }

    // pending decodes: id -> handler(abufNative|null, eof, err)
    const decodes = new Map();
    let decodeSeq = 1;
    global.__oggChunk = function (id, buf, eof, err) {
        const h = decodes.get(id);
        if (!h) return;
        if (eof || err) decodes.delete(id);
        h(buf, eof, err);
    };

    function bytesOf(data) {
        if (data instanceof ArrayBuffer) return data;
        if (ArrayBuffer.isView(data)) return data;
        throw new TypeError('expected ArrayBuffer');
    }

    class AudioContext extends EventTarget {
        constructor() {
            super();
            this.sampleRate = A.sampleRate;
            this.state = 'running';
            this.destination = new AudioNode(this, 0);
            this.destination.maxChannelCount = 2;
            this.listener = { setPosition() {}, setOrientation() {} };
            this.baseLatency = 0.02;
        }
        get currentTime() { return A.currentTime(); }
        createGain() { return new GainNode(this); }
        createPanner() { return new PannerNode(this); }
        createStereoPanner() { return new StereoPannerNode(this); }
        createBufferSource() { return new AudioBufferSourceNode(this); }
        createBuffer(ch, len, rate) { return new AudioBuffer({ numberOfChannels: ch, length: len, sampleRate: rate }); }
        createDynamicsCompressor() { return new GainNode(this); }
        decodeAudioData(data, ok, fail) {
            return new Promise((resolve, reject) => {
                const id = decodeSeq++;
                let result = null;
                decodes.set(id, (buf, eof, err) => {
                    if (buf) result = new AudioBuffer(null, buf);
                    if (!eof && !err) return;
                    if (result && !err) {
                        if (ok) rt.invoke(ok, null, [result]);
                        resolve(result);
                    } else {
                        const e = new Error('decodeAudioData: ' + (err || 'no audio'));
                        if (fail) rt.invoke(fail, null, [e]);
                        reject(e);
                    }
                });
                A.decodeOgg(bytesOf(data), id, true);
            });
        }
        resume() { this.state = 'running'; return Promise.resolve(); }
        suspend() { return Promise.resolve(); }
        close() { return Promise.resolve(); }
    }
    global.AudioContext = AudioContext;
    global.webkitAudioContext = AudioContext;
    Object.assign(global, { AudioNode, GainNode, PannerNode, StereoPannerNode, AudioBuffer, AudioBufferSourceNode, AudioParam });

    // Native replacement for the game's asm.js `stbvorbis` decoder (same interface).
    // Data is collected until eof, then decoded on a worker thread and delivered in chunks.
    global.stbvorbis = {
        decodeStream(callback) {
            const parts = [];
            let size = 0;
            return function (input) {
                if (input.data && input.data.length) {
                    parts.push(input.data.slice());
                    size += input.data.length;
                }
                if (!input.eof) return;
                const all = new Uint8Array(size);
                let o = 0;
                for (const p of parts) { all.set(p, o); o += p.length; }
                const id = decodeSeq++;
                decodes.set(id, (buf, eof, err) => {
                    if (err) return callback({ error: err, eof: true });
                    if (buf) {
                        const info = A.bufferInfo(buf);
                        const data = [];
                        for (let c = 0; c < info[0]; c++) data.push(new Float32Array(A.channelData(buf, c)));
                        callback({ data, sampleRate: info[2], eof: false });
                    }
                    if (eof) callback({ data: [], sampleRate: 0, eof: true });
                });
                A.decodeOgg(all, id, false);
            };
        },
        decode(buffer, callback) {
            const push = this.decodeStream(callback);
            push({ data: new Uint8Array(buffer), eof: false });
            push({ eof: true });
        },
    };
})(globalThis);
