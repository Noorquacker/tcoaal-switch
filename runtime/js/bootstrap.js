// Runtime entry: builds a browser + NW.js-like environment, then boots the game.
'use strict';
(function () {
    const sys = __native.sys;
    const parts = ['core', 'dom', 'canvas2d', 'webgl', 'audio', 'node', 'boot'];
    for (const p of parts) {
        try {
            sys.evalScript('/game/runtime/' + p + '.js');
        } catch (e) {
            sys.log('bootstrap: failed in ' + p + '.js: ' + e + '\n' + (e && e.stack));
            throw e;
        }
    }
})();
