// Client settings page — rendered into .mainAnimatedPages so it participates
// in breadcrumb/back-button navigation like Display settings.
//
// viewManager is an ES module we can't import from this script, so we
// replicate its page lifecycle: insert into .mainAnimatedPages, hide siblings,
// dispatch the same events that libraryMenu.js listens on (pageshow via
// pageClassOn) to update the header.
(function() {
    function dispatchPageEvents(target, isRestored) {
        const detail = {
            detail: { type: null, properties: [], params: {}, isRestored: !!isRestored, options: {} },
            bubbles: true
        };
        target.dispatchEvent(new CustomEvent('viewbeforeshow', detail));
        target.dispatchEvent(new CustomEvent('pagebeforeshow', detail));
        target.dispatchEvent(new CustomEvent('viewshow', detail));
        target.dispatchEvent(new CustomEvent('pageshow', detail));
    }

    function showSettingsPage() {
        const mainAnimatedPages = document.querySelector('.mainAnimatedPages');
        if (!mainAnimatedPages) return;

        // Hide all existing legacy pages (viewContainer manages multiple slots)
        const visiblePages = mainAnimatedPages.querySelectorAll('.mainAnimatedPage:not(.hide)');
        for (const p of visiblePages) {
            p.dispatchEvent(new CustomEvent('viewbeforehide', { bubbles: true, cancelable: true }));
            p.classList.add('hide');
            p.dispatchEvent(new CustomEvent('viewhide', { bubbles: true }));
        }

        // Hide the React page container (sibling .skinBody that holds React routes)
        const reactContainer = mainAnimatedPages.nextElementSibling;
        if (reactContainer) reactContainer.classList.add('hide');

        // Build the page element matching jellyfin-web's legacy page structure.
        // Display settings uses: div[data-role=page] > div.settingsContainer > form
        const page = document.createElement('div');
        page.id = 'clientSettingsPage';
        page.setAttribute('data-role', 'page');
        page.setAttribute('data-title', 'Client Settings');
        page.setAttribute('data-backbutton', 'true');
        page.className = 'mainAnimatedPage page libraryPage userPreferencesPage noSecondaryNavPage';
        page.style.overflow = 'auto';

        const settingsContainer = document.createElement('div');
        settingsContainer.className = 'settingsContainer padded-left padded-right padded-bottom-page';
        page.appendChild(settingsContainer);

        const form = document.createElement('form');
        form.style.margin = '0 auto';
        settingsContainer.appendChild(form);

        buildSettingsForm(form);

        mainAnimatedPages.appendChild(page);

        // Push history so the back button navigates away from this page
        history.pushState({ clientSettings: true }, '');

        dispatchPageEvents(page, false);

        // Tear down when navigating away. jellyfin-web's router fires
        // HISTORY_UPDATE on document._callbacks for every navigation.
        function teardown() {
            const cbs = document._callbacks && document._callbacks['HISTORY_UPDATE'];
            if (cbs) {
                const idx = cbs.indexOf(teardown);
                if (idx !== -1) cbs.splice(idx, 1);
            }
            page.dispatchEvent(new CustomEvent('viewbeforehide', { bubbles: true }));
            page.dispatchEvent(new CustomEvent('viewhide', { bubbles: true }));
            page.remove();

            if (reactContainer) reactContainer.classList.remove('hide');
            for (const p of visiblePages) p.classList.remove('hide');

            // The React <Page> component was never unmounted (just CSS-hidden),
            // so its useEffect won't re-fire pageshow to update the header.
            // Find the active React page and re-dispatch pageshow for it.
            if (reactContainer) {
                const activePage = reactContainer.querySelector('[data-role="page"]');
                if (activePage) dispatchPageEvents(activePage, true);
            }
        }
        document._callbacks = document._callbacks || {};
        document._callbacks['HISTORY_UPDATE'] = document._callbacks['HISTORY_UPDATE'] || [];
        document._callbacks['HISTORY_UPDATE'].push(teardown);
    }

    // Populate the settings form with controls driven by window.jmpInfo.
    function buildSettingsForm(form) {
        const jmpInfo = window.jmpInfo;

        const notice = document.createElement('div');
        notice.className = 'infoBanner';
        notice.textContent = 'Changes take effect after restarting the application.';
        form.appendChild(notice);

        for (const sectionOrder of jmpInfo.sections.sort((a, b) => a.order - b.order)) {
            const section = sectionOrder.key;
            const values = jmpInfo.settings[section];
            const descriptions = jmpInfo.settingsDescriptions[section];
            if (!descriptions || !descriptions.length) continue;

            const group = document.createElement('div');
            group.className = 'verticalSection';
            form.appendChild(group);

            const sectionHeader = document.createElement('h2');
            sectionHeader.className = 'sectionTitle';
            sectionHeader.textContent = section.charAt(0).toUpperCase() + section.slice(1);
            group.appendChild(sectionHeader);

            for (const setting of descriptions) {
                const container = document.createElement('div');

                if (setting.options) {
                    container.className = 'selectContainer';
                    const labelText = document.createElement('label');
                    labelText.className = 'inputLabel';
                    labelText.textContent = setting.displayName;
                    container.appendChild(labelText);
                    const control = document.createElement('select');
                    control.className = 'emby-select-withcolor emby-select';
                    control.setAttribute('label', setting.displayName);
                    for (const option of setting.options) {
                        const val = typeof option === 'string' ? option : option.value;
                        const optTitle = typeof option === 'string' ? option : option.title;
                        const opt = document.createElement('option');
                        opt.value = val;
                        opt.selected = String(val) === String(values[setting.key]);
                        opt.textContent = optTitle;
                        control.appendChild(opt);
                    }
                    control.addEventListener('change', () => {
                        jmpInfo.settings[section][setting.key] = control.value;
                        window.api.settings.setValue(section, setting.key, control.value);
                    });
                    container.appendChild(control);
                    if (setting.help) {
                        const helpText = document.createElement('div');
                        helpText.className = 'fieldDescription';
                        helpText.textContent = setting.help;
                        container.appendChild(helpText);
                    }
                } else if (setting.inputType === 'textarea') {
                    container.className = 'inputContainer';
                    const labelText = document.createElement('label');
                    labelText.className = 'inputLabel';
                    labelText.textContent = setting.displayName;
                    container.appendChild(labelText);
                    const control = document.createElement('textarea');
                    control.className = 'emby-input';
                    control.style.resize = 'none';
                    control.value = values[setting.key] || '';
                    control.rows = 2;
                    control.addEventListener('change', () => {
                        jmpInfo.settings[section][setting.key] = control.value;
                        window.api.settings.setValue(section, setting.key, control.value);
                    });
                    container.appendChild(control);
                    if (setting.help) {
                        const helpText = document.createElement('div');
                        helpText.className = 'fieldDescription';
                        helpText.textContent = setting.help;
                        container.appendChild(helpText);
                    }
                } else {
                    container.className = setting.help
                        ? 'checkboxContainer checkboxContainer-withDescription'
                        : 'checkboxContainer';
                    const lbl = document.createElement('label');
                    const control = document.createElement('input');
                    control.type = 'checkbox';
                    control.className = 'emby-checkbox';
                    control.checked = !!values[setting.key];
                    control.addEventListener('change', () => {
                        jmpInfo.settings[section][setting.key] = control.checked;
                        window.api.settings.setValue(section, setting.key, control.checked);
                    });
                    lbl.appendChild(control);
                    const checkSpan = document.createElement('span');
                    checkSpan.className = 'checkboxLabel';
                    checkSpan.textContent = setting.displayName;
                    lbl.appendChild(checkSpan);
                    container.appendChild(lbl);
                    if (setting.help) {
                        const helpText = document.createElement('div');
                        helpText.className = 'fieldDescription checkboxFieldDescription';
                        helpText.textContent = setting.help;
                        container.appendChild(helpText);
                    }
                }

                group.appendChild(container);
            }
        }

        // Reset server button
        if (jmpInfo.settings.main && jmpInfo.settings.main.userWebClient) {
            const group = document.createElement('div');
            group.className = 'verticalSection';
            form.appendChild(group);

            const sectionHeader = document.createElement('h2');
            sectionHeader.className = 'sectionTitle';
            sectionHeader.textContent = 'Server';
            group.appendChild(sectionHeader);

            const btn = document.createElement('button');
            btn.className = 'raised button-cancel block emby-button';
            btn.textContent = 'Reset Saved Server';
            btn.addEventListener('click', () => {
                jmpInfo.settings.main.userWebClient = '';
                if (window.jmpNative && window.jmpNative.saveServerUrl) {
                    window.jmpNative.saveServerUrl('');
                }
                window.location.reload();
            });
            group.appendChild(btn);
        }
    }

    window._openClientSettings = showSettingsPage;
})();
