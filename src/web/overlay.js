let cancelWait = null;

// Saved server URL comes from the native side via IPC. Fire the request at
// script load; the reply arrives through _onSavedServerUrl (defined below)
// and resolves savedServerUrlReady.
let savedServerUrl = null;
const savedServerUrlReady = new Promise((resolve) => {
    window._onSavedServerUrl = (url) => {
        savedServerUrl = url || null;
        resolve(savedServerUrl);
    };
});
window.jmpNative.getSavedServerUrl();

// True whenever the main browser is loading the URL we currently care about.
// Set by the auto-connect path once we know a saved URL exists (main.cpp
// has already pre-loaded it), by navigateMain on user-initiated success,
// cleared whenever native resets main (cancel or user edits URL).
let mainLoaded = false;

// Sleep for `ms` milliseconds, rejecting if cancelWait() is called first.
// Stores the cancel hook in the module-level `cancelWait` so a single cancel
// path handles whichever wait is currently outstanding.
function cancellableDelay(ms, label) {
    return new Promise((resolve, reject) => {
        const t = setTimeout(() => { cancelWait = null; resolve(); }, ms);
        cancelWait = () => {
            console.log('Cancelling ' + label + ' timer', t);
            clearTimeout(t);
            cancelWait = null;
            reject(new Error('cancelled'));
        };
    });
}

async function tryConnect(server, spinnerStartTime = Date.now()) {
    try {
        console.log("Checking connectivity to:", server);

        const resolvedUrl = await window.jmpCheckServerConnectivity(server);
        console.log("Server connectivity check passed");
        console.log("Resolved URL:", resolvedUrl);

        if (!isConnecting) return false;

        // Save the normalized URL returned by native, not the raw user input.
        savedServerUrl = resolvedUrl;
        if (window.jmpNative && window.jmpNative.saveServerUrl) {
            window.jmpNative.saveServerUrl(resolvedUrl);
        }

        // Kick off main-browser navigation immediately, then wait long enough
        // to satisfy both constraints simultaneously: spinner visible ≥1s AND
        // main browser has ≥1s to render after navigate. For fast probes this
        // saves up to ~900ms compared to running the two waits sequentially.
        // Skip when main is already loading (startup pre-load or prior nav).
        if (!mainLoaded) {
            if (window.jmpNative && window.jmpNative.navigateMain) {
                window.jmpNative.navigateMain(resolvedUrl);
                mainLoaded = true;
            } else {
                console.error("navigateMain IPC not available");
                return false;
            }
        }

        const elapsed = Date.now() - spinnerStartTime;
        const waitMs = Math.max(1000 - elapsed, 1000);
        await cancellableDelay(waitMs, 'pre-fade');
        if (!isConnecting) return false;

        if (window.jmpNative && window.jmpNative.dismissOverlay) {
            window.jmpNative.dismissOverlay();
        }
        return true;
    } catch (e) {
        if (/cancel/i.test(e && e.message)) {
            console.log("Connection cancelled");
        } else {
            console.error("Server connectivity check failed:", e);
        }
        return false;
    }
}

let isConnecting = false;

const updateButtonState = () => {
    const address = document.getElementById('address');
    const button = document.getElementById('connect-button');
    const hasValue = address.value.trim().length > 0;

    if (!isConnecting) {
        button.disabled = !hasValue;
    }
};

const cancelOnEscape = (e) => {
    if (isConnecting && e.key === 'Escape') {
        cancelConnection();
    }
};

const startConnecting = async () => {
    const address = document.getElementById('address');
    const title = document.getElementById('title');
    const spinner = document.getElementById('spinner');
    const button = document.getElementById('connect-button');
    const server = address.value;

    isConnecting = true;
    title.textContent = '';
    title.style.visibility = 'hidden';
    address.classList.add('connecting');
    address.style.visibility = 'hidden';
    address.disabled = true;
    spinner.style.display = 'block';
    const spinnerStart = Date.now();
    button.style.visibility = 'hidden';
    document.addEventListener('keydown', cancelOnEscape);

    // C++ handles retries, just wait for result
    const connected = await tryConnect(server, spinnerStart);

    if (!connected) {
        isConnecting = false;
        title.textContent = document.getElementById('title').getAttribute('data-original-text');
        title.style.visibility = 'visible';
        address.classList.remove('connecting');
        address.style.visibility = 'visible';
        address.disabled = false;
        spinner.style.display = 'none';
        button.style.visibility = 'visible';
        document.removeEventListener('keydown', cancelOnEscape);
        updateButtonState();
    }
};

const cancelConnection = () => {
    if (!isConnecting) return;

    console.log("Cancelling connection");
    // Native resets main on cancelServerConnectivity.
    mainLoaded = false;
    isConnecting = false;

    // Cancel C++ connectivity check and abort JS promise.
    // jmpCheckServerConnectivity.abort() calls jmpNative.cancelServerConnectivity
    // internally (see connectivityHelper.js).
    if (window.jmpCheckServerConnectivity.abort) {
        window.jmpCheckServerConnectivity.abort();
    }
    if (cancelWait) cancelWait();

    const address = document.getElementById('address');
    const title = document.getElementById('title');
    const spinner = document.getElementById('spinner');
    const button = document.getElementById('connect-button');

    title.textContent = document.getElementById('title').getAttribute('data-original-text');
    title.style.visibility = 'visible';
    address.classList.remove('connecting');
    address.style.visibility = 'visible';
    address.disabled = false;
    spinner.style.display = 'none';
    button.style.visibility = 'visible';
    document.removeEventListener('keydown', cancelOnEscape);
    updateButtonState();
};

// Button click handler
document.getElementById('connect-button').addEventListener('click', (e) => {
    e.preventDefault();
    e.stopPropagation();

    if (!e.target.disabled) {
        startConnecting();
    }
});

// Form submit handler
document.getElementById('connect-form').addEventListener('submit', (e) => {
    e.preventDefault();
    if (!isConnecting) {
        startConnecting();
    }
});

// Input change handler
document.getElementById('address').addEventListener('input', updateButtonState);


// Enter key handler
document.addEventListener('keydown', (e) => {
    const address = document.getElementById('address');
    if (e.key === 'Enter' && !isConnecting && !address.disabled && address.value.trim()) {
        e.preventDefault();
        startConnecting();
    }
});

// Auto-connect on load
(async () => {
    console.log('Auto-connect: starting');

    const savedServer = await savedServerUrlReady;
    console.log('Auto-connect: savedServer =', savedServer);

    if (savedServer) {
        console.log('Auto-connect: checking saved server', savedServer);

        // main.cpp pre-loads the saved URL into the main browser in parallel
        // with overlay startup, so don't issue a redundant navigateMain.
        mainLoaded = true;

        const address = document.getElementById('address');
        const title = document.getElementById('title');
        const spinner = document.getElementById('spinner');
        const button = document.getElementById('connect-button');

        // Set address value for potential display later
        address.value = savedServer;

        // Show connecting UI
        isConnecting = true;
        title.textContent = '';
        title.style.visibility = 'hidden';
        address.classList.add('connecting');
        address.style.visibility = 'hidden';
        address.disabled = true;
        spinner.style.display = 'block';
        const spinnerStart = Date.now();
        button.style.visibility = 'hidden';
        document.addEventListener('keydown', cancelOnEscape);

        // C++ handles retries, just wait for result
        const connected = await tryConnect(savedServer, spinnerStart);

        if (!connected) {
            // User cancelled or error - show UI
            isConnecting = false;
            title.textContent = document.getElementById('title').getAttribute('data-original-text');
            title.style.visibility = 'visible';
            address.classList.remove('connecting');
            address.style.visibility = 'visible';
            address.disabled = false;
            spinner.style.display = 'none';
            button.style.visibility = 'visible';
            document.removeEventListener('keydown', cancelOnEscape);
            address.focus();
            updateButtonState();
        }
    } else {
        const title = document.getElementById('title');
        const address = document.getElementById('address');
        const button = document.getElementById('connect-button');

        title.style.visibility = 'visible';
        address.style.visibility = 'visible';
        button.style.visibility = 'visible';
        address.focus();
        updateButtonState();
    }
})();
