// About panel. Rendered in a closed Shadow DOM for style/DOM isolation.
// `window._aboutData` is prepended to this file by the browser-side resource
// handler (src/cef/resource_handler.cpp) — do NOT depend on any IPC to
// arrive before first paint.
(function () {
    var data = window._aboutData || {};

    var host = document.createElement('div');
    host.id = '_jabout';
    host.style.cssText =
        'position:fixed;left:0;top:0;width:100vw;height:100vh;' +
        'z-index:2147483647';
    var shadow = host.attachShadow({ mode: 'closed' });

    var style = document.createElement('style');
    style.textContent =
        '*{margin:0;padding:0;box-sizing:border-box}' +
        '.bg{position:fixed;inset:0;background:rgba(0,0,0,.5)}' +
        '.box{position:fixed;left:50%;top:50%;transform:translate(-50%,-50%);' +
          'background:#2b2b2b;border:1px solid #555;border-radius:8px;' +
          'padding:20px 24px 18px;min-width:420px;max-width:80vw;' +
          'font:13px/1.4 sans-serif;color:#e0e0e0;' +
          'box-shadow:0 4px 24px rgba(0,0,0,.6)}' +
        '.head{display:flex;justify-content:center;margin-bottom:16px}' +
        '.logo{max-width:240px;height:auto}' +
        '.x{position:absolute;top:8px;right:10px;cursor:pointer;' +
          'padding:2px 8px;border-radius:4px;font-size:18px;line-height:1;' +
          'color:#aaa;user-select:none}' +
        '.x:hover{background:#3d3d3d;color:#e0e0e0}' +
        '.row{display:flex;margin-top:8px}' +
        '.row .k{flex:0 0 140px;color:#888}' +
        '.row .v{flex:1 1 auto;word-break:break-all}' +
        '.path{cursor:pointer;color:#8ab4f8;text-decoration:underline}' +
        '.path:hover{color:#b3cdff}';
    shadow.appendChild(style);

    var bg = document.createElement('div');
    bg.className = 'bg';
    shadow.appendChild(bg);

    var box = document.createElement('div');
    box.className = 'box';
    shadow.appendChild(box);

    var head = document.createElement('div');
    head.className = 'head';
    var logo = document.createElement('img');
    logo.className = 'logo';
    logo.src = 'logo.png';
    logo.alt = 'Jellyfin';
    var xBtn = document.createElement('div');
    xBtn.className = 'x';
    xBtn.textContent = '\u00D7';
    head.appendChild(logo);
    head.appendChild(xBtn);
    box.appendChild(head);

    function addRow(label, value, isPath) {
        var row = document.createElement('div');
        row.className = 'row';
        var k = document.createElement('div');
        k.className = 'k';
        k.textContent = label;
        var v = document.createElement('div');
        v.className = 'v';
        if (isPath && value) {
            v.className += ' path';
            v.textContent = value;
            v.addEventListener('click', function () {
                if (window.jmpNative) jmpNative.aboutOpenPath(value);
            });
        } else {
            v.textContent = value || '';
        }
        row.appendChild(k);
        row.appendChild(v);
        box.appendChild(row);
    }

    addRow('App version', data.app, false);
    addRow('CEF version', data.cef, false);
    if (data.configDir) addRow('Config directory', data.configDir, true);
    if (data.logFile) addRow('Current log file', data.logFile, true);

    var dismissed = false;
    function dismiss() {
        if (dismissed) return;
        dismissed = true;
        window.removeEventListener('keydown', onKeyDown, true);
        host.remove();
        if (window.jmpNative) jmpNative.aboutDismiss();
    }

    function onKeyDown(e) {
        if (e.key === 'Escape') { e.preventDefault(); dismiss(); }
    }

    xBtn.addEventListener('click', dismiss);
    bg.addEventListener('mousedown', function (e) { e.preventDefault(); dismiss(); });
    // Stop backdrop click-through from firing when clicking inside the box.
    box.addEventListener('mousedown', function (e) { e.stopPropagation(); });
    window.addEventListener('keydown', onKeyDown, true);

    document.body.appendChild(host);
})();
