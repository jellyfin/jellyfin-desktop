// Connectivity helper - uses native C++ for HTTP requests (no CORS issues)
window.jmpCheckServerConnectivity = (() => {
    let pendingResolve = null;
    let pendingReject = null;
    let pendingUrl = null;

    window.jmp.on('overlay.serverConnectivityResult', (p) => {
        if (!p || pendingUrl !== p.url) return;
        if (p.success) {
            pendingResolve(p.baseUrl);
        } else {
            pendingReject(new Error('Connection failed'));
        }
        pendingResolve = null;
        pendingReject = null;
        pendingUrl = null;
    });

    const checkFunc = function(url) {
        return new Promise((resolve, reject) => {
            pendingResolve = resolve;
            pendingReject = reject;
            pendingUrl = url;
            window.jmp.send('overlay.checkServerConnectivity', { url });
        });
    };

    checkFunc.abort = () => {
        window.jmp.send('overlay.cancelServerConnectivity');
        if (pendingReject) {
            pendingReject(new Error('Connection cancelled'));
            pendingResolve = null;
            pendingReject = null;
            pendingUrl = null;
        }
    };

    return checkFunc;
})();
