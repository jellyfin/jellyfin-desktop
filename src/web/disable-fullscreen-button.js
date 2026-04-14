(function() {
    const kioskMode = !!window.kioskMode;
    if (!kioskMode) return;

    const styleId = 'jmp-kiosk-hide-fullscreen-style';
    const selector = '.btnFullscreen, #btn-fullscreen';

    function ensureStyle() {
        if (document.getElementById(styleId)) return;

        const style = document.createElement('style');
        style.id = styleId;
        style.textContent = selector + '{display:none !important;}';
        (document.head || document.documentElement).appendChild(style);
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', ensureStyle, { once: true });
    } else {
        ensureStyle();
    }
})();
