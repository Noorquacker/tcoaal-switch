// Native replacements for slow paths in the game's own scripts. Runs after the
// game's scripts and plugins have loaded, before the window load event.
'use strict';
(function (global) {
    const Bitmap = global.Bitmap;
    if (!Bitmap) return;

    // Bitmap#blur draws the bitmap 28 times with plutovg; do the same 3x3 box
    // filter (twice) in C. Used on every scene change for the menu background.
    Bitmap.prototype.blur = function () {
        this._canvas._ensureSurface().boxBlur(2);
        this._setDirty();
    };

    // Bitmap.snap reads the render texture back through extract.canvas,
    // getImageData/putImageData and drawImage; read it into the bitmap directly.
    const snap = Bitmap.snap;
    Bitmap.snap = function (stage) {
        if (!stage || !global.Graphics.isWebGL()) return snap.call(this, stage);
        const width = global.Graphics.width, height = global.Graphics.height;
        const bitmap = new Bitmap(width, height);
        const renderer = global.Graphics._renderer;
        const renderTexture = global.PIXI.RenderTexture.create(width, height);
        renderer.render(stage, renderTexture);
        stage.worldTransform.identity();
        renderer.bindRenderTexture(renderTexture);
        renderer.gl.readPixelsSurface(0, 0, width, height, bitmap._canvas._ensureSurface());
        renderTexture.destroy({ destroyBase: true });
        bitmap._setDirty();
        return bitmap;
    };

    // Tilemap#_sortChildren re-sorts every child each frame with a bound
    // comparator (~0.9 ms with the chain's 76 links). The order barely changes
    // between frames, so a stable insertion sort with the same comparator gives
    // the same order in about n comparisons.
    if (global.Tilemap) {
        global.Tilemap.prototype._sortChildren = function () {
            const c = this.children;
            for (let i = 1; i < c.length; i++) {
                const item = c[i];
                let j = i - 1;
                if (!(this._compareChildOrder(c[j], item) > 0)) continue;
                do { c[j + 1] = c[j]; j--; } while (j >= 0 && this._compareChildOrder(c[j], item) > 0);
                c[j + 1] = item;
            }
        };
    }

    // The chain (core script's Chain) relaxes every segment 3 times a frame through
    // Chain.drag, with Pixi setters, trig and allocations per segment: ~1.4 ms a
    // frame on desktop for the long chain, far more on the Switch. Load the
    // segments once, relax all passes natively, write them back once.
    const Chain = global.Chain;
    if (Chain && Chain.update && Chain.process && Chain.solve) {
        const sys = __native.sys;
        let seg = new Float64Array(0);
        // Chain.update(x, y), `iterations` times; x/y in tiles
        const relax = function (x, y, iterations) {
            if (iterations <= 0 || !Chain.exists()) return;
            const segs = Chain.activeSg, n = segs.length;
            if (!Chain.onCanvas) {
                const tilemap = global.SceneManager.currentTilemap();
                if (!tilemap) return;
                Chain.onCanvas = true;
                for (const s of segs) tilemap.addChild(s);
            }
            const tw = global.$gameMap.tileWidth(), half = Chain.segWidth / 2;
            Chain.finalPos.x = (x - 0.08) * tw;
            Chain.finalPos.y = (y - 0.08) * tw;
            if (seg.length < n * 3) seg = new Float64Array(n * 3);
            for (let i = 0; i < n; i++) {
                const s = segs[i];
                seg[3 * i] = s.x; seg[3 * i + 1] = s.y; seg[3 * i + 2] = s.rotation;
            }
            sys.chainRelax(seg, n, Chain.finalPos.x, Chain.finalPos.y, Chain.startPos.x, Chain.startPos.y,
                half, half - Chain.segOffst, iterations);
            for (let i = 0; i < n; i++) {
                const s = segs[i];
                s.x = seg[3 * i]; s.y = seg[3 * i + 1]; s.rotation = seg[3 * i + 2];
            }
        };
        const relaxToTarget = function (iterations) {
            const t = Chain.target();
            if (t) relax(t._realX + 0.5, t._realY + 0.5, iterations);
        };
        Chain.update = function (x, y) { relax(x, y, 1); };
        Chain.process = function () { relaxToTarget(3); };
        Chain.solve = function (iterations) {
            this.initIter = 0;
            if (!this.exists()) this.initIter = iterations;
            else relaxToTarget(iterations);
        };
    }

    // Switch layout: + opens the menu (standard button 9) instead of Y (3).
    if (global.Input && global.Input.gamepadMapper) {
        delete global.Input.gamepadMapper[3];
        global.Input.gamepadMapper[9] = 'menu';
    }

    // Saves (including the autosave on every room transfer) are compressed with
    // LZString; the native port produces identical output, ~10x faster.
    if (global.LZString) {
        const sys = __native.sys;
        global.LZString.compressToBase64 = (s) => (s == null ? '' : sys.lzCompressBase64(String(s)));
    }
})(globalThis);
