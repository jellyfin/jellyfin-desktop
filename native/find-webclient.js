async function tryConnect(server) {
    try {
        console.log("Checking connectivity to:", server);

        const resolvedUrl = await window.jmpCheckServerConnectivity(server);

        // The user may have cancelled while the check was still starting.
        if (!isConnecting) {
            return { success: false };
        }

        console.log("Server connectivity check passed");
        console.log("Resolved URL:", resolvedUrl);

        // Save original URL but navigate to fully-resolved redirect
        window.jmpInfo.settings.main.userWebClient = server;

        // Navigation will clean up handlers, but do it explicitly
        window.location = resolvedUrl;

        return { success: true };
    } catch (e) {
        console.error("Server connectivity check failed:", e);
        return { success: false, error: e && e.message ? e.message : '' };
    }
}

let isConnecting = false;

const conStatus = document.getElementById('status');
const address = document.getElementById('address');
const title = document.getElementById('title');
const spinner = document.getElementById('spinner');
const button = document.getElementById('connect-button');

const showStatus = (text, isError = false) => {
    conStatus.textContent = text || '';
    conStatus.className = isError ? 'error' : '';
    conStatus.style.display = text ? 'block' : 'none';
};

const restoreForm = () => {
    isConnecting = false;
    title.textContent = title.getAttribute('data-original-text') || 'Server Address';
    title.style.visibility = 'visible';
    address.classList.remove('connecting');
    address.style.display = '';
    address.style.visibility = 'visible';
    address.disabled = false;
    spinner.style.display = 'none';
    button.classList.remove('cancel');
    button.textContent = button.getAttribute('data-original-text') || 'Connect';
    button.style.visibility = 'visible';
    document.removeEventListener('keydown', cancelOnEscape);
    showStatus('');
    updateButtonState();
    address.focus();
};

const updateButtonState = () => {
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

const startConnecting = async (server) => {
    if (isConnecting) {
        return;
    }

    server = (server || address.value).trim();
    if (!server) {
        return;
    }
    if (!/^https?:\/\//i.test(server)) {
        server = 'http://' + server;
    }
    address.value = server;

    isConnecting = true;
    title.textContent = '';
    title.style.visibility = 'hidden';
    address.classList.add('connecting');
    address.style.display = 'none';
    address.style.visibility = 'hidden';
    address.disabled = true;
    spinner.style.display = 'block';
    button.classList.add('cancel');
    button.textContent = window.cancelButtonText || 'Cancel';
    button.disabled = false;
    button.style.visibility = 'visible';
    button.focus();
    document.addEventListener('keydown', cancelOnEscape);
    showStatus('Connecting to ' + server + '…');

    const result = await tryConnect(server);

    if (result.success) {
        return;
    }

    // Cancelled while waiting; cancelConnection() already restored the form.
    if (!isConnecting) {
        return;
    }

    restoreForm();
    showStatus('Unable to connect to ' + server + (result.error ? '\n' + result.error : ''), true);
};

const cancelConnection = () => {
    if (!isConnecting) return;

    console.log("Cancelling connection");
    isConnecting = false;

    // Cancel C++ connectivity check and abort JS promise
    if (window.api && window.api.system) {
        window.api.system.cancelServerConnectivity();
    }
    if (window.jmpCheckServerConnectivity.abort) {
        window.jmpCheckServerConnectivity.abort();
    }

    restoreForm();
};

// Button click handler
document.getElementById('connect-button').addEventListener('click', (e) => {
    e.preventDefault();
    e.stopPropagation();

    if (isConnecting) {
        cancelConnection();
    } else if (!e.target.disabled) {
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
    // The button handles its own Enter activation.
    if (e.target === button) {
        return;
    }

    const address = document.getElementById('address');
    if (e.key === 'Enter' && !isConnecting && !address.disabled && address.value.trim()) {
        e.preventDefault();
        startConnecting();
    }
});

// Auto-connect on load
(() => {
    console.log('Auto-connect: starting');

    const savedServer = window.jmpInfo?.settings?.main?.userWebClient;
    console.log('Auto-connect: savedServer =', savedServer);

    if (savedServer) {
        console.log('Auto-connect: checking saved server', savedServer);
        address.value = savedServer;
        startConnecting(savedServer);
    } else {
        restoreForm();
    }
})();
