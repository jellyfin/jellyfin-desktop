// Certificate management on jellyfin-web's server pages, loaded after nativeshell.js.
// find-webclient.html is shown as an overlay for it, so this page stays loaded.
(() => {
    const isPage = (name) => window.location.hash.toLowerCase().includes(name);
    let lastServerId = null;
    let onAddServerPage = false;

    const findServer = (id) => {
        try {
            const credentials = JSON.parse(window.localStorage.getItem('jellyfin_credentials') || '{}');
            const server = (credentials.Servers || []).find(s => s.Id === id);
            if (server) {
                return {
                    name: server.Name || '',
                    url: server.ManualAddress || server.LocalAddress || server.RemoteAddress || ''
                };
            }
        } catch (e) {
            console.error('Failed to read server list', e);
        }
        return null;
    };

    const addCertificatesMenuItem = (sheet) => {
        if (!lastServerId || sheet.querySelector('[data-id="jmp-certificates"]')) return;
        const first = sheet.querySelector('.actionSheetMenuItem');
        const server = findServer(lastServerId);
        if (!first || !server || !/^https:\/\//i.test(server.url)) return;

        // Same markup as the other entries; the action sheet closes itself on click
        const item = first.cloneNode(true);
        item.setAttribute('data-id', 'jmp-certificates');
        item.removeAttribute('autofocus');
        const text = item.querySelector('.actionSheetItemText');
        (text || item).textContent = 'Certificates';
        item.addEventListener('click', () => {
            window.api.certificates.openOverlay('certs', server.url, server.name);
        });
        first.parentNode.appendChild(item);
    };

    // jellyfin-web's "Add Server" page is replaced by find-webclient in add mode
    const checkAddServerRoute = () => {
        const entered = isPage('addserver') && !onAddServerPage;
        onAddServerPage = isPage('addserver');
        if (entered) {
            window.api.certificates.openOverlay('add', '', '');
        }
    };

    // Let jellyfin-web add and connect to the server tested in the overlay with its own form
    const submitAddServer = (url, attempts = 50) => {
        const form = document.querySelector('.addServerForm');
        if (!form) {
            if (attempts > 0) setTimeout(() => submitAddServer(url, attempts - 1), 100);
            return;
        }
        form.querySelector('#txtServerHost').value = url;
        form.requestSubmit();
    };

    // Remember which server card opened the action sheet
    document.addEventListener('click', (e) => {
        const card = e.target.closest && e.target.closest('.servers .card');
        if (card) {
            lastServerId = card.getAttribute('data-id');
        }
    }, true);

    document.addEventListener('DOMContentLoaded', async () => {
        await window.initCompleted;

        // While the overlay covers this page it gets no mouse events and jellyfin-web goes
        // idle; nativeshell.js keeps the cursor visible while jmpOverlayOpen is set
        window.api.certificates.overlayRequested.connect(() => {
            window.jmpOverlayOpen = true;
            window.api.window.setCursorVisibility(true);
        });
        window.api.certificates.overlayClosed.connect(() => {
            window.jmpOverlayOpen = false;
            window.api.window.setCursorVisibility(true);
        });

        window.api.certificates.overlayClosed.connect((url) => {
            if (!isPage('addserver')) return;
            if (url) {
                submitAddServer(url);
            } else {
                window.history.back();
            }
        });

        // jellyfin-web's router uses pushState, which fires no hashchange, so also check on DOM updates
        window.addEventListener('hashchange', checkAddServerRoute);
        window.addEventListener('popstate', checkAddServerRoute);
        checkAddServerRoute();

        new MutationObserver(() => {
            checkAddServerRoute();
            if (isPage('selectserver')) {
                const sheet = document.querySelector('.actionSheet');
                if (sheet) addCertificatesMenuItem(sheet);
            }
        }).observe(document.body, { childList: true, subtree: true });
    });
})();
