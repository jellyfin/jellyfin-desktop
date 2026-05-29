// Client-side window decorations (CSD).
//
// Draws a titlebar (drag region + minimize / maximize / close) as a Shadow DOM
// overlay in the page. Server-sent themes / Custom CSS can't reach into the
// shadow root, so they can't accidentally break the chrome. Window operations
// (move / minimize / maximize / close) are performed natively via jmpNative;
// the compositor takes over interactive moves.
//
// Enabled by the native side via window.__jmpCsd.setEnabled() — it stays
// dormant on backends that draw their own decorations (X11 WMs, macOS,
// Windows) and when the user disables it.
(function() {
    const HOST_TAG = 'jmp-titlebar';
    const SVG_NS = 'http://www.w3.org/2000/svg';

    const state = {
        enabled: false,     // native says CSD should be drawn at all
        fullscreen: false,  // OS window is fullscreen → no titlebar
        videoActive: false, // a video is playing
        osdVisible: false,  // jellyfin video OSD is showing
    };

    let host = null;        // shadow host element (light DOM)
    let insetStyle = null;  // <style> that shifts page content below the bar

    function call(name, ...args) {
        if (window.jmpNative && typeof window.jmpNative[name] === 'function') {
            window.jmpNative[name](...args);
        }
    }

    const EDGE = 8;    // edge grab thickness (px)
    const CORNER = 20; // corner grab box (px)

    const CSS = `
        /* Full-window host so it can also host perimeter resize grips. It is
           click-through (pointer-events:none); only the bar and resize zones
           opt back in, so page content underneath stays interactive. */
        :host {
            all: initial;
            position: fixed; inset: 0;
            z-index: 2147483647;
            display: none;
            pointer-events: none;
        }
        :host([data-visible="1"]) { display: block; }
        /* Transparent titlebar: the strip only reserves drag space and floats
           the controls; page content (inset below) shows through. */
        .bar {
            position: absolute; top: 0; left: 0; right: 0;
            height: var(--jmp-csd-height, 32px);
            display: flex; align-items: center;
            pointer-events: auto;
            background: transparent; color: #f0f0f0;
            font: 13px/1 system-ui, sans-serif;
            -webkit-user-select: none; user-select: none;
        }
        .drag { flex: 1 1 auto; height: 100%; display: flex; align-items: center;
                padding-left: 12px; overflow: hidden; }
        .title { white-space: nowrap; text-overflow: ellipsis; overflow: hidden; opacity: 0.85; }
        .controls { flex: 0 0 auto; display: flex; height: 100%; }
        button {
            all: unset; width: 46px; height: 100%;
            display: flex; align-items: center; justify-content: center;
            color: #f0f0f0; cursor: default;
        }
        button:hover { background: rgba(127, 127, 127, 0.28); }
        button.close:hover { background: #c42b1c; color: #fff; }
        /* Drop-shadow keeps the line icons legible over any content/theme,
           since there's no bar background behind them. */
        svg { width: 11px; height: 11px; fill: none; stroke: currentColor; stroke-width: 1.2;
              filter: drop-shadow(0 0 1px rgba(0, 0, 0, 0.9)); }
        /* Invisible resize grips around the window perimeter. Listed after the
           bar so corners/edges win the hit-test at overlaps. */
        .rz { position: absolute; pointer-events: auto; }
        .rz-t { top: 0; left: 0; right: 0; height: ${EDGE}px; cursor: n-resize; }
        .rz-b { bottom: 0; left: 0; right: 0; height: ${EDGE}px; cursor: s-resize; }
        .rz-l { top: 0; bottom: 0; left: 0; width: ${EDGE}px; cursor: w-resize; }
        .rz-r { top: 0; bottom: 0; right: 0; width: ${EDGE}px; cursor: e-resize; }
        .rz-tl { top: 0; left: 0; width: ${CORNER}px; height: ${CORNER}px; cursor: nw-resize; }
        .rz-tr { top: 0; right: 0; width: ${CORNER}px; height: ${CORNER}px; cursor: ne-resize; }
        .rz-bl { bottom: 0; left: 0; width: ${CORNER}px; height: ${CORNER}px; cursor: sw-resize; }
        .rz-br { bottom: 0; right: 0; width: ${CORNER}px; height: ${CORNER}px; cursor: se-resize; }`;

    // xdg_toplevel resize-edge bitmask: top=1 bottom=2 left=4 right=8.
    const RESIZE_EDGES = [
        ['rz-t', 1], ['rz-b', 2], ['rz-l', 4], ['rz-r', 8],
        ['rz-tl', 5], ['rz-tr', 9], ['rz-bl', 6], ['rz-br', 10],
    ];

    function svgIcon(lines) {
        const svg = document.createElementNS(SVG_NS, 'svg');
        svg.setAttribute('viewBox', '0 0 11 11');
        for (const l of lines) {
            const el = document.createElementNS(SVG_NS, l.tag);
            for (const [k, v] of Object.entries(l.attrs)) el.setAttribute(k, v);
            svg.appendChild(el);
        }
        return svg;
    }

    function makeButton(cls, label, icon) {
        const b = document.createElement('button');
        b.className = cls;
        b.setAttribute('part', 'button');
        b.title = label;
        b.setAttribute('aria-label', label);
        b.appendChild(icon);
        return b;
    }

    function buildHost() {
        if (host) return;
        host = document.createElement(HOST_TAG);
        const root = host.attachShadow({ mode: 'open' });

        const style = document.createElement('style');
        style.textContent = CSS;
        root.appendChild(style);

        const bar = document.createElement('div');
        bar.className = 'bar';
        bar.setAttribute('part', 'bar');

        const drag = document.createElement('div');
        drag.className = 'drag';
        drag.setAttribute('part', 'drag');
        const title = document.createElement('span');
        title.className = 'title';
        title.setAttribute('part', 'title');
        drag.appendChild(title);

        const controls = document.createElement('div');
        controls.className = 'controls';
        const minBtn = makeButton('min', 'Minimize',
            svgIcon([{ tag: 'line', attrs: { x1: 1, y1: 6, x2: 10, y2: 6 } }]));
        const maxBtn = makeButton('max', 'Maximize',
            svgIcon([{ tag: 'rect', attrs: { x: 1.5, y: 1.5, width: 8, height: 8 } }]));
        const closeBtn = makeButton('close', 'Close', svgIcon([
            { tag: 'line', attrs: { x1: 1.5, y1: 1.5, x2: 9.5, y2: 9.5 } },
            { tag: 'line', attrs: { x1: 9.5, y1: 1.5, x2: 1.5, y2: 9.5 } },
        ]));
        controls.append(minBtn, maxBtn, closeBtn);

        bar.append(drag, controls);
        root.appendChild(bar);

        // Perimeter resize grips (appended after the bar so they win overlaps).
        for (const [cls, edge] of RESIZE_EDGES) {
            const z = document.createElement('div');
            z.className = 'rz ' + cls;
            z.addEventListener('mousedown', (e) => {
                if (e.button !== 0) return;
                e.preventDefault();
                call('windowStartResize', edge);
            });
            root.appendChild(z);
        }

        // Interactive move on press; manual double-click → maximize toggle
        // (OSR doesn't always deliver a native dblclick reliably).
        let lastDown = 0;
        drag.addEventListener('mousedown', (e) => {
            if (e.button !== 0) return;
            const now = Date.now();
            if (now - lastDown < 400) {
                lastDown = 0;
                call('windowToggleMaximize');
                return;
            }
            lastDown = now;
            call('windowStartMove');
        });
        minBtn.addEventListener('click', () => call('windowMinimize'));
        maxBtn.addEventListener('click', () => call('windowToggleMaximize'));
        closeBtn.addEventListener('click', () => call('windowClose'));

        const attach = () => {
            if (host.isConnected) return;
            (document.documentElement || document.body).appendChild(host);
        };
        attach();
        document.addEventListener('DOMContentLoaded', attach);
    }

    function buildInsetStyle() {
        if (insetStyle) return;
        insetStyle = document.createElement('style');
        // Shift jellyfin-web's own chrome below the bar so it isn't covered.
        // Applied only while .jmp-csd-inset is set (browsing, not video).
        insetStyle.textContent = `
            :root { --jmp-csd-height: 32px; }
            html.jmp-csd-inset .skinHeader,
            html.jmp-csd-inset .touch-menu-la,
            html.jmp-csd-inset .MuiAppBar-positionFixed,
            html.jmp-csd-inset .MuiDrawer-paper,
            html.jmp-csd-inset .formDialogHeader { padding-top: var(--jmp-csd-height) !important; }
            html.jmp-csd-inset .mainAnimatedPage { top: var(--jmp-csd-height) !important; }`;
        const add = () => { if (document.head) document.head.appendChild(insetStyle); };
        if (document.head) add(); else document.addEventListener('DOMContentLoaded', add);
    }

    // Resolve visibility/inset from current state. Mirrors the macOS titlebar:
    // the top inset is held constant whenever decorated and windowed (so the
    // layout doesn't jump when playback starts); only the controls follow the
    // OSD during video. Fullscreen drops both (immersive).
    function update() {
        if (!host) return;
        const active = state.enabled && !state.fullscreen;
        const inset = active;
        const visible = active && (!state.videoActive || state.osdVisible);
        host.setAttribute('data-visible', visible ? '1' : '0');
        document.documentElement.classList.toggle('jmp-csd-inset', inset);
    }

    function wireSignals() {
        // Fullscreen: native pushes via _nativeFullscreenChanged; also the
        // HTML5 fullscreenchange path in native-shim.
        const origFsc = window._nativeFullscreenChanged;
        window._nativeFullscreenChanged = function(fs) {
            if (origFsc) origFsc(fs);
            state.fullscreen = !!fs;
            update();
        };
        document.addEventListener('fullscreenchange', () => {
            state.fullscreen = !!document.fullscreenElement;
            update();
        });

        // Video playback lifecycle from the shim's player signals.
        const player = window.api && window.api.player;
        if (player) {
            player.playing.connect(() => { state.videoActive = true; update(); });
            player.stopped.connect(() => { state.videoActive = false; state.osdVisible = false; update(); });
            player.finished.connect(() => { state.videoActive = false; state.osdVisible = false; update(); });
        }

        // OSD visibility: jellyfin-web fires SHOW_VIDEO_OSD through its internal
        // Events system (obj._callbacks), not DOM events. Hook it directly, the
        // same way native-shim does for the macOS traffic lights.
        document._callbacks = document._callbacks || {};
        document._callbacks['SHOW_VIDEO_OSD'] = document._callbacks['SHOW_VIDEO_OSD'] || [];
        document._callbacks['SHOW_VIDEO_OSD'].push((_e, visible) => {
            state.videoActive = true;
            state.osdVisible = !!visible;
            update();
        });
    }

    window.__jmpCsd = {
        setEnabled(on) {
            state.enabled = !!on;
            if (state.enabled) {
                buildInsetStyle();
                buildHost();
            }
            update();
        },
    };

    wireSignals();
    // Ask the native side whether CSD applies (setting + backend).
    call('csdReady');
})();
