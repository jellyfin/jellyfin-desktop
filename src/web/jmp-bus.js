// JS-side message bus. Available in every CEF browser that injects this
// script. Pairs with the C++ MessageBus (src/cef/message_bus.cpp).
(function() {
    const handlers = Object.create(null);
    window.jmp = {
        send(name, payload) {
            if (!window.jmpNative || !window.jmpNative.send) return;
            window.jmpNative.send(name, payload || {});
        },
        on(name, fn) {
            (handlers[name] = handlers[name] || []).push(fn);
        },
        off(name, fn) {
            const list = handlers[name];
            if (!list) return;
            const i = list.indexOf(fn);
            if (i >= 0) list.splice(i, 1);
        },
        // Invoked by App::OnProcessMessageReceived in cef_app.cpp on the
        // renderer process when the C++ side emits via g_bus.emit.
        onMessage(name, payload) {
            const list = handlers[name];
            if (!list) return;
            for (const fn of list.slice()) {
                try { fn(payload || {}); }
                catch (e) { console.error('[jmp]', name, 'handler error:', e); }
            }
        }
    };
})();
