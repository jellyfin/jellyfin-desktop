(function() {
    console.log('[Media] Installing native shim...');

    // Fullscreen state tracking via HTML5 Fullscreen API
    window._isFullscreen = false;

    document.addEventListener('fullscreenchange', () => {
        const fullscreen = !!document.fullscreenElement;
        if (window._isFullscreen === fullscreen) return;
        window._isFullscreen = fullscreen;
        console.log('[Media] Fullscreen changed:', fullscreen);
        // Notify player so UI updates (jellyfin-web listens for this)
        const player = window._mpvVideoPlayerInstance;
        if (player && player.events) {
            player.events.trigger(player, 'fullscreenchange');
        }
    });

    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape' && window._isFullscreen) {
            document.exitFullscreen().catch(() => {});
        }
    });

    // Buffered ranges storage (updated by native code)
    window._bufferedRanges = [];
    window._nativeUpdateBufferedRanges = function(ranges) {
        window._bufferedRanges = ranges || [];
    };

    // Signal emulation (Qt-style connect/disconnect)
    function createSignal(name) {
        const callbacks = [];
        const signal = function(...args) {
            for (const cb of callbacks) {
                try { cb(...args); } catch(e) { console.error('[Media] [Signal] ' + name + ' error:', e); }
            }
        };
        signal.connect = (cb) => {
            callbacks.push(cb);
            console.log('[Media] [Signal] ' + name + ' connected, now has', callbacks.length, 'listeners');
        };
        signal.disconnect = (cb) => {
            const idx = callbacks.indexOf(cb);
            if (idx >= 0) callbacks.splice(idx, 1);
            console.log('[Media] [Signal] ' + name + ' disconnected, now has', callbacks.length, 'listeners');
        };
        return signal;
    }

    // Saved settings from native (injected as placeholder, replaced at load time)
    const _savedSettings = JSON.parse('__SETTINGS_JSON__');

    // window.jmpInfo - settings and device info
    window.jmpInfo = {
        version: '1.0.0',
        deviceName: 'Jellyfin Desktop',
        mode: 'desktop',
        userAgent: navigator.userAgent,
        scriptPath: '',
        sections: [
            { key: 'playback', order: 0 },
            { key: 'audio', order: 1 },
            { key: 'advanced', order: 2 }
        ],
        settings: {
            main: { enableMPV: true, fullscreen: false, userWebClient: '__SERVER_URL__' },
            playback: {
                hwdec: _savedSettings.hwdec || 'auto-safe'
            },
            audio: {
                audioPassthrough: _savedSettings.audioPassthrough || '',
                audioExclusive: _savedSettings.audioExclusive || false,
                audioChannels: _savedSettings.audioChannels || ''
            },
            advanced: {
                transparentTitlebar: _savedSettings.transparentTitlebar !== false,
                logLevel: _savedSettings.logLevel || ''
            }
        },
        settingsDescriptions: {
            playback: [
                { key: 'hwdec', displayName: 'Hardware Decoding', help: 'Hardware video decoding mode. Use "auto-safe" for safe auto-detection, "auto" for aggressive auto-detection, or "no" to disable.', options: [
                    { value: 'auto-safe', title: 'Auto (Safe)' },
                    { value: 'auto', title: 'Auto' },
                    { value: 'no', title: 'Disabled' },
                    { value: 'vaapi', title: 'VA-API (Linux)' },
                    { value: 'nvdec', title: 'NVDEC (NVIDIA)' },
                    { value: 'vulkan', title: 'Vulkan' },
                    { value: 'd3d11va', title: 'D3D11VA (Windows)' },
                    { value: 'videotoolbox', title: 'VideoToolbox (macOS)' }
                ]}
            ],
            audio: [
                { key: 'audioPassthrough', displayName: 'Audio Passthrough', help: 'Comma-separated list of codecs to pass through to the audio device (e.g. ac3,eac3,dts-hd,truehd). Leave empty to disable.', inputType: 'textarea' },
                { key: 'audioExclusive', displayName: 'Exclusive Audio Output', help: 'Take exclusive control of the audio device during playback. May reduce latency but prevents other apps from playing audio.' },
                { key: 'audioChannels', displayName: 'Audio Channel Layout', help: 'Force a specific channel layout. Leave empty for auto-detection.', options: [
                    { value: '', title: 'Auto' },
                    { value: 'stereo', title: 'Stereo' },
                    { value: '5.1', title: '5.1 Surround' },
                    { value: '7.1', title: '7.1 Surround' }
                ]}
            ],
            advanced: [
                { key: 'logLevel', displayName: 'Log Level', help: 'Set the application log verbosity level.', options: [
                    { value: '', title: 'Default (Info)' },
                    { value: 'verbose', title: 'Verbose' },
                    { value: 'debug', title: 'Debug' },
                    { value: 'warn', title: 'Warning' },
                    { value: 'error', title: 'Error' }
                ]}
            ]
        },
        settingsUpdate: [],
        settingsDescriptionsUpdate: []
    };

    // macOS-only: transparent titlebar toggle (shown first in Advanced section)
    if (navigator.platform.startsWith('Mac')) {
        jmpInfo.settingsDescriptions.advanced.unshift({
            key: 'transparentTitlebar',
            displayName: 'Transparent Titlebar',
            help: 'Overlay traffic light buttons on the window content instead of a separate titlebar. Requires restart.'
        });
    }

    // Player state
    const playerState = {
        position: 0,
        duration: 0,
        volume: 100,
        muted: false,
        paused: false
    };

    // window.api.player - MPV control API
    window.api = {
        player: {
            // Signals (Qt-style)
            playing: createSignal('playing'),
            paused: createSignal('paused'),
            finished: createSignal('finished'),
            stopped: createSignal('stopped'),
            canceled: createSignal('canceled'),
            error: createSignal('error'),
            buffering: createSignal('buffering'),
            seeking: createSignal('seeking'),
            positionUpdate: createSignal('positionUpdate'),
            updateDuration: createSignal('updateDuration'),
            stateChanged: createSignal('stateChanged'),
            videoPlaybackActive: createSignal('videoPlaybackActive'),
            windowVisible: createSignal('windowVisible'),
            onVideoRecangleChanged: createSignal('onVideoRecangleChanged'),
            onMetaData: createSignal('onMetaData'),

            // Methods
            load(url, options, streamdata, audioStream, subtitleStream, callback) {
                console.log('[Media] player.load:', url);
                if (callback) {
                    // Wait for playing signal before calling callback
                    const onPlaying = () => {
                        this.playing.disconnect(onPlaying);
                        this.error.disconnect(onError);
                        callback();
                    };
                    const onError = () => {
                        this.playing.disconnect(onPlaying);
                        this.error.disconnect(onError);
                        callback();
                    };
                    this.playing.connect(onPlaying);
                    this.error.connect(onError);
                }
                if (window.jmpNative && window.jmpNative.playerLoad) {
                    const metadataJson = streamdata?.metadata ? JSON.stringify(streamdata.metadata) : '{}';
                    window.jmpNative.playerLoad(url, options?.startMilliseconds || 0, audioStream || -1, subtitleStream || -1, metadataJson);
                }
            },
            stop() {
                console.log('[Media] player.stop');
                if (window.jmpNative) window.jmpNative.playerStop();
            },
            pause() {
                console.log('[Media] player.pause');
                if (window.jmpNative) window.jmpNative.playerPause();
                playerState.paused = true;
            },
            play() {
                console.log('[Media] player.play');
                if (window.jmpNative) window.jmpNative.playerPlay();
                playerState.paused = false;
            },
            seekTo(ms) {
                console.log('[Media] player.seekTo:', ms);
                if (window.jmpNative) window.jmpNative.playerSeek(ms);
            },
            setVolume(vol) {
                console.log('[Media] player.setVolume:', vol);
                playerState.volume = vol;
                if (window.jmpNative) window.jmpNative.playerSetVolume(vol);
            },
            setMuted(muted) {
                console.log('[Media] player.setMuted:', muted);
                playerState.muted = muted;
                if (window.jmpNative) window.jmpNative.playerSetMuted(muted);
            },
            setPlaybackRate(rate) {
                console.log('[Media] player.setPlaybackRate:', rate);
                if (window.jmpNative) window.jmpNative.playerSetSpeed(rate);
            },
            setSubtitleStream(index) {
                console.log('[Media] player.setSubtitleStream:', index);
                if (window.jmpNative) window.jmpNative.playerSetSubtitle(index != null ? index : -1);
            },
            setAudioStream(index) {
                console.log('[Media] player.setAudioStream:', index);
                if (window.jmpNative) window.jmpNative.playerSetAudio(index != null ? index : -1);
            },
            setSubtitleDelay(ms) {
                console.log('[Media] player.setSubtitleDelay:', ms);
            },
            setAudioDelay(ms) {
                console.log('[Media] player.setAudioDelay:', ms);
                if (window.jmpNative) window.jmpNative.playerSetAudioDelay(ms / 1000.0);
            },
            setVideoRectangle(x, y, w, h) {
                // No-op for now, we always render fullscreen
            },
            getPosition(callback) {
                if (callback) callback(playerState.position);
                return playerState.position;
            },
            getDuration(callback) {
                if (callback) callback(playerState.duration);
                return playerState.duration;
            },
        },
        system: {
            openExternalUrl(url) {
                window.open(url, '_blank');
            },
            exit() {
                if (window.jmpNative) window.jmpNative.appExit();
            },
            cancelServerConnectivity() {
                if (window.jmpCheckServerConnectivity && window.jmpCheckServerConnectivity.abort) {
                    window.jmpCheckServerConnectivity.abort();
                }
            }
        },
        settings: {
            setValue(section, key, value, callback) {
                if (window.jmpNative && window.jmpNative.setSettingValue) {
                    window.jmpNative.setSettingValue(section, key, typeof value === 'boolean' ? (value ? 'true' : 'false') : String(value));
                }
                if (callback) callback();
            },
            sectionValueUpdate: createSignal('sectionValueUpdate'),
            groupUpdate: createSignal('groupUpdate')
        },
        input: {
            // Signals for media session control commands
            hostInput: createSignal('hostInput'),
            positionSeek: createSignal('positionSeek'),
            rateChanged: createSignal('rateChanged'),
            volumeChanged: createSignal('volumeChanged'),

            executeActions() {}
        },
        window: {
            setCursorVisibility(visible) {}
        }
    };

    // Expose signal emitter for native code
    window._nativeEmit = function(signal, ...args) {
        console.log('[Media] _nativeEmit called with signal:', signal, 'args:', args);
        if (window.api && window.api.player && window.api.player[signal]) {
            console.log('[Media] Firing signal:', signal);
            window.api.player[signal](...args);
        } else {
            console.error('[Media] Signal not found:', signal, 'api exists:', !!window.api);
        }
    };
    window._nativeUpdatePosition = function(ms) {
        playerState.position = ms;
        window.api.player.positionUpdate(ms);
    };
    window._nativeUpdateDuration = function(ms) {
        playerState.duration = ms;
        window.api.player.updateDuration(ms);
    };
    // Native emitters for media session control commands
    window._nativeHostInput = function(actions) {
        console.log('[Media] _nativeHostInput:', actions);
        window.api.input.hostInput(actions);
    };
    window._nativeSetRate = function(rate) {
        console.log('[Media] _nativeSetRate:', rate);
        window.api.input.rateChanged(rate);
    };
    window._nativeSeek = function(positionMs) {
        console.log('[Media] _nativeSeek:', positionMs);
        window.api.input.positionSeek(positionMs);
    };

    // window.NativeShell - app info and plugins
    const plugins = ['mpvVideoPlayer', 'mpvAudioPlayer', 'inputPlugin'];
    for (const plugin of plugins) {
        window[plugin] = () => window['_' + plugin];
    }

    window.NativeShell = {
        openUrl(url, target) {
            window.api.system.openExternalUrl(url);
        },
        downloadFile(info) {
            window.api.system.openExternalUrl(info.url);
        },
        openClientSettings() {
            showSettingsModal();
        },
        getPlugins() {
            return plugins;
        }
    };

    // Device profile for direct play
    function getDeviceProfile() {
        return {
            Name: 'Jellyfin Desktop',
            MaxStaticBitrate: 1000000000,
            MusicStreamingTranscodingBitrate: 1280000,
            TimelineOffsetSeconds: 5,
            TranscodingProfiles: [
                { Type: 'Audio' },
                {
                    Container: 'ts',
                    Type: 'Video',
                    Protocol: 'hls',
                    AudioCodec: 'aac,mp3,ac3,opus,vorbis',
                    VideoCodec: 'h264,h265,hevc,mpeg4,mpeg2video',
                    MaxAudioChannels: '6'
                },
                { Container: 'jpeg', Type: 'Photo' }
            ],
            DirectPlayProfiles: [
                { Type: 'Video' },
                { Type: 'Audio' },
                { Type: 'Photo' }
            ],
            ResponseProfiles: [],
            ContainerProfiles: [],
            CodecProfiles: [],
            SubtitleProfiles: [
                { Format: 'srt', Method: 'External' },
                { Format: 'srt', Method: 'Embed' },
                { Format: 'ass', Method: 'External' },
                { Format: 'ass', Method: 'Embed' },
                { Format: 'sub', Method: 'Embed' },
                { Format: 'ssa', Method: 'Embed' },
                { Format: 'pgssub', Method: 'Embed' },
                { Format: 'dvdsub', Method: 'Embed' }
            ]
        };
    }

    window.NativeShell.AppHost = {
        init() {
            return Promise.resolve({
                deviceName: jmpInfo.deviceName,
                appName: 'Jellyfin Desktop',
                appVersion: jmpInfo.version
            });
        },
        getDefaultLayout() {
            return jmpInfo.mode;
        },
        supports(command) {
            const features = [
                'filedownload', 'displaylanguage', 'htmlaudioautoplay',
                'htmlvideoautoplay', 'externallinks', 'multiserver',
                'fullscreenchange', 'remotevideo', 'displaymode',
                'exitmenu', 'clientsettings'
            ];
            return features.includes(command.toLowerCase());
        },
        getDeviceProfile,
        getSyncProfile: getDeviceProfile,
        appName() { return 'Jellyfin Desktop'; },
        appVersion() { return jmpInfo.version; },
        deviceName() { return jmpInfo.deviceName; },
        exit() { window.api.system.exit(); }
    };

    window.initCompleted = Promise.resolve();
    window.apiPromise = Promise.resolve(window.api);

    // Observe <meta name="theme-color"> for titlebar color sync.
    // jellyfin-web's themeManager.js updates this tag when the user switches themes.
    function sendThemeColor(color) {
        if (color && window.jmpNative && window.jmpNative.themeColor) {
            window.jmpNative.themeColor(color);
        }
    }

    function observeThemeColorMeta(meta) {
        sendThemeColor(meta.content);
        new MutationObserver(() => sendThemeColor(meta.content))
            .observe(meta, { attributes: true, attributeFilter: ['content'] });
    }

    document.addEventListener('DOMContentLoaded', () => {
        // Inject CSS to hide cursor when jellyfin-web signals mouse idle.
        // jellyfin-web adds 'mouseIdle' to body after inactivity during video playback.
        // This CSS makes CEF report CT_NONE so the native side can hide the OS cursor.
        const style = document.createElement('style');
        let css = 'body.mouseIdle, body.mouseIdle * { cursor: none !important; }';

        // macOS: offset UI elements so traffic lights don't overlap content
        if (navigator.platform.startsWith('Mac') && jmpInfo.settings.advanced.transparentTitlebar) {
            css += '\n:root { --mac-titlebar-height: 28px; }';
            css += '\n.skinHeader { padding-top: var(--mac-titlebar-height) !important; }';
            css += '\n.mainAnimatedPage { top: var(--mac-titlebar-height) !important; }';
            css += '\n.touch-menu-la { padding-top: var(--mac-titlebar-height); }';
            // Dashboard uses MUI AppBar + Drawer instead of .skinHeader
            css += '\n.MuiAppBar-positionFixed { padding-top: var(--mac-titlebar-height) !important; }';
            css += '\n.MuiDrawer-paper { padding-top: var(--mac-titlebar-height) !important; }';
            // Dialog headers (e.g. client settings modal)
            css += '\n.formDialogHeader { padding-top: var(--mac-titlebar-height) !important; }';

            // Hide/show traffic lights with the video OSD.
            // jellyfin-web uses an internal Events.trigger() system (obj._callbacks),
            // not DOM events. Register directly on that callback structure.
            document._callbacks = document._callbacks || {};
            document._callbacks['SHOW_VIDEO_OSD'] = document._callbacks['SHOW_VIDEO_OSD'] || [];
            document._callbacks['SHOW_VIDEO_OSD'].push((_e, visible) => {
                if (window.jmpNative && window.jmpNative.setOsdVisible) {
                    window.jmpNative.setOsdVisible(!!visible);
                }
            });
        }

        style.textContent = css;
        document.head.appendChild(style);

        // Sync titlebar color with theme-color meta tag
        const meta = document.querySelector('meta[name="theme-color"]');
        if (meta) {
            observeThemeColorMeta(meta);
        } else {
            // Tag may be added dynamically — watch for it
            new MutationObserver((mutations, obs) => {
                for (const m of mutations) {
                    for (const node of m.addedNodes) {
                        if (node.nodeName === 'META' && node.name === 'theme-color') {
                            obs.disconnect();
                            observeThemeColorMeta(node);
                            return;
                        }
                    }
                }
            }).observe(document.head, { childList: true });
        }
    });

    // Settings modal (adapted from jellyfin-desktop's nativeshell.js)
    function showSettingsModal() {
        const modalContainer = document.createElement('div');
        modalContainer.className = 'dialogContainer';
        modalContainer.style.backgroundColor = 'rgba(0,0,0,0.5)';
        modalContainer.addEventListener('click', e => {
            if (e.target === modalContainer) modalContainer.remove();
        });
        document.body.appendChild(modalContainer);

        const dialog = document.createElement('div');
        dialog.className = 'focuscontainer dialog dialog-fixedSize dialog-small formDialog opened';
        modalContainer.appendChild(dialog);

        const header = document.createElement('div');
        header.className = 'formDialogHeader';
        dialog.appendChild(header);

        const title = document.createElement('h3');
        title.className = 'formDialogHeaderTitle';
        title.textContent = 'Client Settings';
        header.appendChild(title);

        const contents = document.createElement('div');
        contents.className = 'formDialogContent smoothScrollY';
        contents.style.paddingTop = '2em';
        contents.style.marginBottom = '6.2em';
        dialog.appendChild(contents);

        // Restart notice
        const notice = document.createElement('div');
        notice.style.cssText = 'padding: 0.5em 1em; margin: 0 1em 1em; background: #332b00; border-radius: 4px; color: #ffcc00; font-size: 0.9em;';
        notice.textContent = 'Changes take effect after restarting the application.';
        contents.appendChild(notice);

        for (const sectionOrder of jmpInfo.sections.sort((a, b) => a.order - b.order)) {
            const section = sectionOrder.key;
            const values = jmpInfo.settings[section];
            const descriptions = jmpInfo.settingsDescriptions[section];
            if (!descriptions || !descriptions.length) continue;

            const group = document.createElement('fieldset');
            group.className = 'editItemMetadataForm editMetadataForm dialog-content-centered';
            group.style.border = '0';
            group.style.outline = '0';
            contents.appendChild(group);

            const legend = document.createElement('legend');
            const legendHeader = document.createElement('h2');
            legendHeader.textContent = section.charAt(0).toUpperCase() + section.slice(1);
            legend.appendChild(legendHeader);
            group.appendChild(legend);

            for (const setting of descriptions) {
                const label = document.createElement('label');
                label.className = 'inputContainer';
                label.style.marginBottom = '1.8em';
                label.style.display = 'block';

                if (setting.options) {
                    const control = document.createElement('select');
                    control.className = 'emby-select-withcolor emby-select';
                    for (const option of setting.options) {
                        const opt = document.createElement('option');
                        opt.value = option.value;
                        opt.selected = String(option.value) === String(values[setting.key]);
                        opt.textContent = option.title;
                        control.appendChild(opt);
                    }
                    control.addEventListener('change', () => {
                        jmpInfo.settings[section][setting.key] = control.value;
                        window.api.settings.setValue(section, setting.key, control.value);
                    });
                    const labelText = document.createElement('label');
                    labelText.className = 'inputLabel';
                    labelText.textContent = setting.displayName + ':';
                    label.appendChild(labelText);
                    if (setting.help) {
                        const helpText = document.createElement('div');
                        helpText.style.cssText = 'font-size: 0.8em; color: #999; margin-bottom: 0.5em;';
                        helpText.textContent = setting.help;
                        label.appendChild(helpText);
                    }
                    label.appendChild(control);
                } else if (setting.inputType === 'textarea') {
                    const control = document.createElement('textarea');
                    control.className = 'emby-select-withcolor emby-select';
                    control.style.resize = 'none';
                    control.value = values[setting.key] || '';
                    control.rows = 2;
                    control.addEventListener('change', () => {
                        jmpInfo.settings[section][setting.key] = control.value;
                        window.api.settings.setValue(section, setting.key, control.value);
                    });
                    const labelText = document.createElement('label');
                    labelText.className = 'inputLabel';
                    labelText.textContent = setting.displayName + ':';
                    label.appendChild(labelText);
                    if (setting.help) {
                        const helpText = document.createElement('div');
                        helpText.style.cssText = 'font-size: 0.8em; color: #999; margin-bottom: 0.5em;';
                        helpText.textContent = setting.help;
                        label.appendChild(helpText);
                    }
                    label.appendChild(control);
                } else {
                    const control = document.createElement('input');
                    control.type = 'checkbox';
                    control.checked = !!values[setting.key];
                    control.addEventListener('change', () => {
                        jmpInfo.settings[section][setting.key] = control.checked;
                        window.api.settings.setValue(section, setting.key, control.checked);
                    });
                    label.appendChild(control);
                    label.appendChild(document.createTextNode(' ' + setting.displayName));
                    if (setting.help) {
                        const helpText = document.createElement('div');
                        helpText.style.cssText = 'font-size: 0.8em; color: #999; margin-top: 0.3em;';
                        helpText.textContent = setting.help;
                        label.appendChild(helpText);
                    }
                }

                group.appendChild(label);
            }
        }

        // Reset server button
        if (jmpInfo.settings.main && jmpInfo.settings.main.userWebClient) {
            const group = document.createElement('fieldset');
            group.className = 'editItemMetadataForm editMetadataForm dialog-content-centered';
            group.style.border = '0';
            group.style.outline = '0';
            contents.appendChild(group);

            const legend = document.createElement('legend');
            const legendHeader = document.createElement('h2');
            legendHeader.textContent = 'Server';
            legend.appendChild(legendHeader);
            group.appendChild(legend);

            const btn = document.createElement('button');
            btn.className = 'raised button-cancel block btnCancel emby-button';
            btn.textContent = 'Reset Saved Server';
            btn.style.maxWidth = '50%';
            btn.style.margin = '0 auto';
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

    console.log('[Media] Native shim installed');
})();
