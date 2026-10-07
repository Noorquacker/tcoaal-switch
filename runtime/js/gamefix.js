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
})(globalThis);
