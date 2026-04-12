// Context menu rendered in a closed shadow DOM for style/DOM isolation.
// Called from C++ RunContextMenu via: window._showContextMenu(items, x, y)
window._showContextMenu = function(items, x, y) {
    var old = document.getElementById('_jctx');
    if (old) old.remove();

    var host = document.createElement('div');
    host.id = '_jctx';
    host.style.cssText = 'position:fixed;left:0;top:0;width:100vw;height:100vh;z-index:2147483647';
    var shadow = host.attachShadow({mode: 'closed'});

    // Build shadow DOM via safe DOM methods — no user content in template
    var style = document.createElement('style');
    style.textContent =
        '*{margin:0;padding:0;box-sizing:border-box;user-select:none}' +
        '.bg{position:fixed;left:0;top:0;width:100vw;height:100vh}' +
        '.m{position:fixed;background:#2b2b2b;border:1px solid #555;' +
          'border-radius:4px;padding:4px 0;min-width:160px;' +
          'font:13px/1.4 sans-serif;color:#e0e0e0;' +
          'box-shadow:0 2px 8px rgba(0,0,0,.4);outline:none}' +
        '.i{padding:5px 24px 5px 12px;cursor:default;white-space:nowrap}' +
        '.i:hover,.i.a{background:#3d3d3d}' +
        '.i.off{color:#666;pointer-events:none}' +
        'hr{border:none;border-top:1px solid #444;margin:4px 8px}';
    shadow.appendChild(style);

    var bg = document.createElement('div');
    bg.className = 'bg';
    shadow.appendChild(bg);

    var menu = document.createElement('div');
    menu.className = 'm';
    menu.style.left = x + 'px';
    menu.style.top = y + 'px';
    shadow.appendChild(menu);

    var enabled = [];
    for (var i = 0; i < items.length; i++) {
        if (items[i].sep) {
            menu.appendChild(document.createElement('hr'));
            continue;
        }
        var el = document.createElement('div');
        el.className = 'i' + (items[i].enabled ? '' : ' off');
        el.textContent = items[i].label;
        el.dataset.id = items[i].id;
        menu.appendChild(el);
        if (items[i].enabled) enabled.push(el);
    }

    var active = -1;
    function setActive(n) {
        if (active >= 0) enabled[active].classList.remove('a');
        active = n;
        if (active >= 0) enabled[active].classList.add('a');
    }

    var done = false;
    function finish(id) {
        if (done) return;
        done = true;
        window.removeEventListener('keydown', onKeyDown, true);
        host.remove();
        if (id != null) { if (window.jmpNative) jmpNative.menuItemSelected(id); }
        else { if (window.jmpNative) jmpNative.menuDismissed(); }
    }

    function onKeyDown(e) {
        if (e.key === 'Escape') { e.preventDefault(); finish(null); }
        else if (e.key === 'ArrowDown') { e.preventDefault(); setActive(active < enabled.length - 1 ? active + 1 : 0); }
        else if (e.key === 'ArrowUp') { e.preventDefault(); setActive(active > 0 ? active - 1 : enabled.length - 1); }
        else if (e.key === 'Enter' && active >= 0) { e.preventDefault(); finish(parseInt(enabled[active].dataset.id)); }
    }

    requestAnimationFrame(function() {
        var r = menu.getBoundingClientRect();
        if (r.right > innerWidth) menu.style.left = Math.max(0, innerWidth - r.width - 4) + 'px';
        if (r.bottom > innerHeight) menu.style.top = Math.max(0, innerHeight - r.height - 4) + 'px';
    });

    // preventDefault on mousedown keeps focus on the text input so edit
    // commands (Paste, etc.) target the right element.
    menu.addEventListener('mousedown', function(e) {
        e.preventDefault();
        var t = e.target.closest('.i:not(.off)');
        if (t) finish(parseInt(t.dataset.id));
    });
    bg.addEventListener('mousedown', function(e) { e.preventDefault(); finish(null); });
    window.addEventListener('keydown', onKeyDown, true);

    document.body.appendChild(host);
};
