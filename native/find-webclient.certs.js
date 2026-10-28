// Certificates (CA and client/mTLS) for find-webclient.html, and its overlay modes.
//
// Modes (query parameter "mode"):
//   (none)  first start, connects and navigates to the server
//   add     overlay opened from jellyfin-web's "Add Server" page, reports the server back
//   certs   overlay opened from jellyfin-web's server list, manages certificates of "server"
//
// find-webclient.js calls the hooks in window.findWebClientCerts.
(() => {
    const params = new URLSearchParams(window.location.search);
    const mode = params.get('mode') || '';
    const isOverlay = mode === 'add' || mode === 'certs';
    const certsServer = mode === 'certs' ? params.get('server') || '' : '';

    const certText = {
        advanced: 'Advanced',
        httpsRequired: 'Certificates can only be used with https:// servers.',
        httpServer: 'Certificates are only available for https:// servers.',
        passwordRequired: 'Enter the password for {0}',
        invalidPassword: 'Wrong password for {0}, try again',
        invalidData: 'Unsupported or damaged certificate file.',
        unsupported: 'The file could not be opened with this password, or its format is not supported on this system.',
        writeError: 'Could not save the certificate.',
        missingCn: 'The client certificate has no Common Name (CN) and cannot be used on this system. Load a client certificate that has a CN.',
        save: 'Save',
        newFile: '(new)'
    };

    const $ = (id) => document.getElementById(id);
    const format = (text, arg) => text.replace('{0}', arg);
    const certApi = (method, ...args) => new Promise(resolve => {
        window.api.certificates[method](...args, resolve);
    });

    let stagedCerts = { hasCa: false, hasClient: false };
    // Server whose stored certificates are listed next to the staged ones
    let storedServer = certsServer;

    const hasStagedCerts = () => stagedCerts.hasCa || stagedCerts.hasClient;
    const isConnecting = () => $('address').classList.contains('connecting');

    // Toggles hidden instead of relying on .error:empty: QtWebEngine's Chromium does not
    // lay out the element again when it stops being :empty
    function setMessage(element, text) {
        element.textContent = text || '';
        element.hidden = !text;
    }

    function showError(text) {
        setMessage($('connect-error'), text);
    }

    // Server address from the resolved web client URL, keeping a base path:
    // https://host/jellyfin/web/index.html -> https://host/jellyfin
    function serverBaseUrl(resolvedUrl) {
        const url = new URL(resolvedUrl);
        const path = url.pathname.replace(/\/web(\/.*)?$/i, '').replace(/\/+$/, '');
        return url.origin + path;
    }

    function readFileBase64(file) {
        return new Promise((resolve, reject) => {
            const reader = new FileReader();
            reader.onload = () => resolve(reader.result.substring(reader.result.indexOf(',') + 1));
            reader.onerror = () => reject(reader.error);
            reader.readAsDataURL(file);
        });
    }

    function askPassword(text) {
        const dialog = $('password-dialog');
        const input = $('password');
        $('password-text').textContent = text;
        input.value = '';
        return new Promise(resolve => {
            dialog.addEventListener('close', () => {
                resolve(dialog.returnValue === 'ok' ? input.value : null);
            }, { once: true });
            dialog.returnValue = '';
            dialog.showModal();
            input.focus();
        });
    }

    async function refreshCertificates() {
        stagedCerts = await certApi('info', '');
        const stored = storedServer ? await certApi('info', storedServer) : {};

        for (const row of document.querySelectorAll('.cert-row')) {
            const kind = row.dataset.kind;
            const has = kind === 'ca' ? 'hasCa' : 'hasClient';
            const name = kind === 'ca' ? 'caFileName' : 'clientFileName';
            let text = '';
            if (stagedCerts[has]) {
                text = stagedCerts[name] + (storedServer ? ' ' + certText.newFile : '');
            } else if (stored[has]) {
                text = stored[name];
            }
            row.querySelector('.cert-name').textContent = text;
            row.querySelector('.cert-remove').hidden = !text;
        }
    }

    async function uploadCertificate(kind, file) {
        const message = $('cert-message');
        setMessage(message, '');

        const base64 = await readFileBase64(file);
        let password = '';
        for (;;) {
            const res = await certApi('stage', kind, file.name, base64, password);
            console.info('Certificate stage:', kind, file.name, JSON.stringify(res));
            if (res.success) break;
            if (res.reason === 'PASSWORD_REQUIRED' || res.reason === 'INVALID_PASSWORD') {
                const prompt = res.reason === 'INVALID_PASSWORD' ? certText.invalidPassword : certText.passwordRequired;
                password = await askPassword(format(prompt, file.name));
                if (password === null) break;
                continue;
            }
            setMessage(message, {
                WRITE_ERROR: certText.writeError,
                UNSUPPORTED: certText.unsupported,
                MISSING_COMMON_NAME: certText.missingCn
            }[res.reason] || certText.invalidData);
            break;
        }
        await refreshCertificates();
    }

    async function removeCertificate(kind) {
        // A newly chosen file is removed first, otherwise the stored one
        const staged = kind === 'ca' ? stagedCerts.hasCa : stagedCerts.hasClient;
        await certApi('remove', staged ? '' : storedServer, kind);
        await refreshCertificates();
    }

    function setAdvancedExpanded(expanded) {
        $('advanced').hidden = !expanded;
        $('advanced-toggle').innerHTML = certText.advanced + (expanded ? ' &#9662;' : ' &#9656;');
    }

    function init() {
        for (const row of document.querySelectorAll('.cert-row')) {
            const input = row.querySelector('input[type=file]');
            row.querySelector('.cert-pick').addEventListener('click', () => input.click());
            row.querySelector('.cert-remove').addEventListener('click', () => removeCertificate(row.dataset.kind));
            input.addEventListener('change', async () => {
                if (input.files.length) {
                    await uploadCertificate(row.dataset.kind, input.files[0]);
                }
                input.value = '';
            });
        }

        $('advanced-toggle').addEventListener('click', () => setAdvancedExpanded($('advanced').hidden));
        setAdvancedExpanded(mode === 'certs');

        if (mode === 'certs') {
            const name = params.get('name') || certsServer;
            $('title').textContent = name;
            $('title').setAttribute('data-original-text', name);
            $('address').value = certsServer;
            $('address').readOnly = true;
            $('connect-button').textContent = certText.save;
            $('advanced-toggle').hidden = true;

            if (!certsServer.toLowerCase().startsWith('https://')) {
                setMessage($('cert-message'), certText.httpServer);
                for (const button of document.querySelectorAll('.cert-pick')) {
                    button.disabled = true;
                }
            }
        } else if (mode === 'add') {
            $('address').value = params.get('server') || '';
        }
        // Lets find-webclient.js update the connect button for the address set above
        $('address').dispatchEvent(new Event('input'));

        // Cancels a running connection attempt; in the overlay it also closes it.
        // Visibility comes from find-webclient.css.
        const close = $('close-button');
        close.textContent = window.cancelButtonText || 'Cancel';
        close.hidden = false;
        close.addEventListener('click', () => {
            if (isConnecting()) {
                cancelConnection(); // from find-webclient.js
            } else if (isOverlay) {
                window.api.certificates.closeOverlay('');
            }
        });

        if (isOverlay) {
            document.body.classList.add('overlay');
            document.addEventListener('keydown', (e) => {
                if (e.key === 'Escape' && !isConnecting() && !$('password-dialog').open) {
                    window.api.certificates.closeOverlay('');
                }
            });
        }

        document.body.classList.add('certs-ready');
        refreshCertificates();
    }

    document.addEventListener('DOMContentLoaded', async () => {
        await window.initCompleted;
        init();
    });

    // The check keeps retrying on network errors, show why
    document.addEventListener('jmpconnectivityretry', (e) => {
        if (isConnecting()) {
            showError(e.detail.message);
        }
    });

    window.findWebClientCerts = {
        mode,

        // Server to connect to, or null when it cannot be used with the chosen certificates
        prepareServer(server) {
            server = server.trim();
            if (hasStagedCerts()) {
                if (!/^https?:\/\//i.test(server)) {
                    server = 'https://' + server;
                } else if (!/^https:\/\//i.test(server)) {
                    showError(certText.httpsRequired);
                    return null;
                }
            }
            return server;
        },

        // False when there is nothing to connect for (certificate mode without new files)
        beforeConnect() {
            showError('');
            if (mode === 'certs' && !hasStagedCerts()) {
                window.api.certificates.closeOverlay(certsServer);
                return false;
            }
            return true;
        },

        // Handles a successful check in the overlay, the connectivity check already stored
        // the pending certificates for the server. Returns true when handled.
        connected(server, resolvedUrl) {
            if (!isOverlay) return false;
            window.api.certificates.closeOverlay(mode === 'add' ? serverBaseUrl(resolvedUrl) : certsServer);
            return true;
        },

        // Connection checks only fail for certificate problems (network errors are retried)
        failed(server, error) {
            if (error.message === 'Connection cancelled') return;
            showError(error.message);
            if (!isOverlay) {
                // Show this server's certificates so they can be replaced
                storedServer = server;
                setAdvancedExpanded(true);
                refreshCertificates();
            }
        },

        dialogOpen() {
            return $('password-dialog').open;
        }
    };
})();
