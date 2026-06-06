// In-page replacement for the native <select> popup. 
// A dirty hack, but more succinct+less_finnicky than dealing with layering native popups on X11
(function () {
    var open = null;

    function isDropdown(el) {
        return el && el.tagName === 'SELECT' && !el.multiple && el.size <= 1 && !el.disabled;
    }

    function closeOpen() {
        if (open) open();
    }

    function openMenu(select) {
        closeOpen();

        var host = document.createElement('div');
        host.id = '_jselect';
        host.style.cssText = 'position:fixed;left:0;top:0;width:100vw;height:100vh;z-index:2147483647';
        var shadow = host.attachShadow({mode: 'closed'});

        var style = document.createElement('style');
        style.textContent =
            '*{margin:0;padding:0;box-sizing:border-box;user-select:none}' +
            '.bg{position:fixed;left:0;top:0;width:100vw;height:100vh}' +
            '.m{position:fixed;background:#2b2b2b;border:1px solid #555;' +
              'border-radius:4px;padding:4px 0;overflow-y:auto;' +
              'font:13px/1.4 sans-serif;color:#e0e0e0;' +
              'box-shadow:0 2px 8px rgba(0,0,0,.4);outline:none}' +
            '.i{padding:5px 24px 5px 12px;cursor:default;white-space:nowrap}' +
            '.i:hover,.i.a{background:#3d3d3d}' +
            '.i.sel{font-weight:600}' +
            '.i.off{color:#666;pointer-events:none}' +
            '.g{padding:5px 12px 2px;color:#9a9a9a;font-weight:600;cursor:default;white-space:nowrap}';
        shadow.appendChild(style);

        var bg = document.createElement('div');
        bg.className = 'bg';
        shadow.appendChild(bg);

        var menu = document.createElement('div');
        menu.className = 'm';
        shadow.appendChild(menu);

        // Key rows by opt.index, not row position, so disabled options and
        // optgroup headers don't skew the map back to selectedIndex.
        var rows = [];
        var rowIndex = [];
        function addOption(opt) {
            var el = document.createElement('div');
            var off = opt.disabled || (opt.parentNode && opt.parentNode.tagName === 'OPTGROUP' && opt.parentNode.disabled);
            el.className = 'i' + (off ? ' off' : '') + (opt.index === select.selectedIndex ? ' sel' : '');
            el.textContent = opt.text;
            menu.appendChild(el);
            if (!off) {
                el.dataset.idx = opt.index;
                rows.push(el);
                rowIndex.push(opt.index);
            }
        }
        for (var i = 0; i < select.children.length; i++) {
            var child = select.children[i];
            if (child.tagName === 'OPTGROUP') {
                var g = document.createElement('div');
                g.className = 'g';
                g.textContent = child.label;
                menu.appendChild(g);
                for (var j = 0; j < child.children.length; j++) {
                    if (child.children[j].tagName === 'OPTION') addOption(child.children[j]);
                }
            } else if (child.tagName === 'OPTION') {
                addOption(child);
            }
        }

        var r = select.getBoundingClientRect();
        menu.style.minWidth = r.width + 'px';
        menu.style.maxHeight = Math.max(80, innerHeight - 8) + 'px';
        menu.style.left = r.left + 'px';
        menu.style.top = r.bottom + 'px';

        var active = -1;
        function setActive(n) {
            if (active >= 0) rows[active].classList.remove('a');
            active = n;
            if (active >= 0) {
                rows[active].classList.add('a');
                rows[active].scrollIntoView({block: 'nearest'});
            }
        }

        var done = false;
        function finish(idx) {
            if (done) return;
            done = true;
            open = null;
            window.removeEventListener('keydown', onKeyDown, true);
            window.removeEventListener('blur', onDismiss);
            window.removeEventListener('resize', onDismiss);
            document.removeEventListener('scroll', onDismiss, true);
            host.remove();
            if (idx != null && idx !== select.selectedIndex) {
                select.selectedIndex = idx;
                select.dispatchEvent(new Event('input', {bubbles: true}));
                select.dispatchEvent(new Event('change', {bubbles: true}));
            }
        }
        open = function () { finish(null); };
        function onDismiss() { finish(null); }

        function onKeyDown(e) {
            if (e.key === 'Escape') { e.preventDefault(); finish(null); }
            else if (e.key === 'ArrowDown') { e.preventDefault(); setActive(active < rows.length - 1 ? active + 1 : 0); }
            else if (e.key === 'ArrowUp') { e.preventDefault(); setActive(active > 0 ? active - 1 : rows.length - 1); }
            else if ((e.key === 'Enter' || e.key === ' ') && active >= 0) { e.preventDefault(); finish(rowIndex[active]); }
            else if (e.key === 'Tab') { finish(null); }
        }

        menu.addEventListener('mousedown', function (e) {
            e.preventDefault();
            var t = e.target.closest('.i:not(.off)');
            if (t) finish(parseInt(t.dataset.idx));
        });
        bg.addEventListener('mousedown', function (e) {
            e.preventDefault();
            finish(null);
        });
        window.addEventListener('keydown', onKeyDown, true);
        window.addEventListener('blur', onDismiss);
        window.addEventListener('resize', onDismiss);
        document.addEventListener('scroll', onDismiss, true);

        document.body.appendChild(host);

        requestAnimationFrame(function () {
            var mr = menu.getBoundingClientRect();
            if (mr.bottom > innerHeight && r.top - mr.height >= 0) {
                menu.style.top = (r.top - mr.height) + 'px';
            } else if (mr.bottom > innerHeight) {
                menu.style.top = Math.max(0, innerHeight - mr.height) + 'px';
            }
            if (mr.right > innerWidth) {
                menu.style.left = Math.max(0, innerWidth - mr.width - 4) + 'px';
            }
        });

        for (var k = 0; k < rowIndex.length; k++) {
            if (rowIndex[k] === select.selectedIndex) { setActive(k); break; }
        }
    }

    // Capture phase so we intercept before the engine opens the native popup.
    document.addEventListener('mousedown', function (e) {
        if (e.button !== 0) return;
        var select = e.target.closest && e.target.closest('select');
        if (!isDropdown(select)) return;
        e.preventDefault();
        if (open) { closeOpen(); return; }
        select.focus();
        openMenu(select);
    }, true);

    document.addEventListener('keydown', function (e) {
        // While open, the menu's own capture-phase handler owns the keyboard.
        if (open) return;
        if (!isDropdown(document.activeElement)) return;
        var opens = e.key === ' ' || e.key === 'Enter' || e.key === 'F4' ||
            (e.altKey && (e.key === 'ArrowDown' || e.key === 'ArrowUp'));
        if (!opens) return;
        e.preventDefault();
        openMenu(document.activeElement);
    }, true);
})();
