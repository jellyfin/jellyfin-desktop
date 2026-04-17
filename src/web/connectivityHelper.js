// Connectivity helper - uses native C++ for HTTP requests (no CORS issues)
window.jmpCheckServerConnectivity = (() => {
    let pendingResolve = null;
    let pendingReject = null;
    let pendingUrl = null;

    // Called by native code when result is ready
    window._onServerConnectivityResult = (url, success, resolvedUrl) => {
        console.log('Connectivity result:', url, success, resolvedUrl);
        if (pendingUrl === url) {
            if (success) {
                pendingResolve(resolvedUrl);
            } else {
                pendingReject(new Error('Connection failed'));
            }
            pendingResolve = null;
            pendingReject = null;
            pendingUrl = null;
        }
    };

    const checkFunc = async function(url) {
        // Wait for jmpNative
        let attempts = 0;
        while (!window.jmpNative?.checkServerConnectivity && attempts < 50) {
            await new Promise(resolve => setTimeout(resolve, 100));
            attempts++;
        }
        if (!window.jmpNative?.checkServerConnectivity) {
            throw new Error('Native connectivity check not available');
        }

        return new Promise((resolve, reject) => {
            pendingResolve = resolve;
            pendingReject = reject;
            pendingUrl = url;

            console.log('Checking connectivity:', url);
            window.jmpNative.checkServerConnectivity(url);
        });
    };

    checkFunc.abort = () => {
        if (window.jmpNative?.cancelServerConnectivity) {
            window.jmpNative.cancelServerConnectivity();
        }
        if (pendingReject) {
            pendingReject(new Error('Connection cancelled'));
            pendingResolve = null;
            pendingReject = null;
            pendingUrl = null;
        }
    };

    return checkFunc;
})();
