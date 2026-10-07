// Loads www/index.html the way a browser would: stylesheets, then scripts in
// order, then dynamically added scripts (plugins), then the window load event.
'use strict';
(function (global) {
    const sys = __native.sys;
    const rt = global.__rt;

    // Used by the build-time patch of the game's hidden-script loader.
    global.__loadScript = function (rel) {
        const el = document.createElement('script');
        el._noExec = true;
        el.src = rel;
        document.body.appendChild(el);
        if (!rt.execScriptFile(el, '/game/www/' + rel)) throw new Error('failed to load ' + rel);
    };

    const html = sys.readText('/game/www/index.html');
    if (!html) throw new Error('www/index.html missing from romfs');
    const m = /<title>([^<]*)<\/title>/i.exec(html);
    if (m) document.title = m[1];

    // stylesheets (fonts)
    for (const link of html.matchAll(/<link\b[^>]*>/gi)) {
        const tag = link[0];
        const rel = /rel\s*=\s*["']([^"']+)/i.exec(tag), href = /href\s*=\s*["']([^"']+)/i.exec(tag);
        if (!rel || !href) continue;
        const el = document.createElement('link');
        el.rel = rel[1];
        el.href = href[1];
        document.head.appendChild(el);
    }

    // static scripts, executed in document order
    for (const s of html.matchAll(/<script\b[^>]*\bsrc\s*=\s*["']([^"']+)["'][^>]*>\s*<\/script>/gi)) {
        const el = document.createElement('script');
        el._noExec = true;
        el.src = s[1];
        document.body.appendChild(el);
        const t0 = sys.now();
        if (rt.execScriptFile(el, '/game/www/' + s[1])) el.dispatchEvent(new Event('load'));
        sys.log('loaded ' + s[1] + ' in ' + (sys.now() - t0).toFixed(1) + 'ms');
    }

    // plugins added by PluginManager.setup()
    rt.runPendingScripts();
    sys.evalScript('/game/runtime/gamefix.js');

    document.readyState = 'complete';
    document.dispatchEvent(new Event('DOMContentLoaded'));
    global.dispatchEvent(new Event('load'));
    sys.log('boot complete');
})(globalThis);
