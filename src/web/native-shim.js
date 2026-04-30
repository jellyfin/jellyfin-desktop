(function() {
    console.log('[Media] Installing native shim...');

    // Fullscreen state tracking via HTML5 Fullscreen API
    window._isFullscreen = false;

    document.addEventListener('fullscreenchange', () => {
        const fullscreen = !!document.fullscreenElement;
        if (window._isFullscreen === fullscreen) return;
        window._isFullscreen = fullscreen;
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

    // Double-click on video area toggles fullscreen.
    // Detected in JS because Wayland doesn't provide click count natively.
    (function() {
        let lastTime = 0, lastX = 0, lastY = 0;
        document.addEventListener('mousedown', (e) => {
            if (e.button !== 0 || !e.target.classList.contains("mainAnimatedPage")) return;
            const now = Date.now();
            const dx = e.clientX - lastX;
            const dy = e.clientY - lastY;
            if ((now - lastTime) < 500 && (dx * dx + dy * dy) < 25) {
                if (document.querySelector('.videoPlayerContainer')) {
                    jmp.send('fullscreen.toggle');
                }
                lastTime = 0;
            } else {
                lastTime = now;
                lastX = e.clientX;
                lastY = e.clientY;
            }
        }, true);
    })();

    // Signal emulation (Qt-style connect/disconnect)
    function createSignal(name) {
        const callbacks = [];
        const signal = function(...args) {
            for (const cb of callbacks) {
                try { cb(...args); } catch(e) { console.error('[Media] [Signal] ' + name + ' error:', e); }
            }
        };
        signal.connect = (cb) => { callbacks.push(cb); };
        signal.disconnect = (cb) => {
            const idx = callbacks.indexOf(cb);
            if (idx >= 0) callbacks.splice(idx, 1);
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
                hwdec: _savedSettings.hwdec || 'auto'
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
                { key: 'hwdec', displayName: 'Hardware Decoding', help: 'Hardware video decoding mode. Use "auto" for automatic detection or "no" to disable.', options: _savedSettings.hwdecOptions }
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

    // Buffered ranges cache, populated by player.bufferedRangesChanged.
    let _bufferedRanges = [];

    // Player state cache
    const playerState = {
        position: 0,
        duration: 0,
        volume: 100,
        muted: false,
        paused: false,
        pendingPositionRequests: new Map(),
        nextRequestId: 1
    };

    // window.api.player - signal-driven plugin contract used by mpv-player-core
    // and friends. Routes through jmp.send/jmp.on.
    const playerSignals = {
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
    };

    window.api = {
        player: Object.assign(playerSignals, {
            getBufferedRanges() { return _bufferedRanges; },
            getPosition(callback) {
                if (callback) callback(playerState.position);
                return playerState.position;
            },
            getDuration(callback) {
                if (callback) callback(playerState.duration);
                return playerState.duration;
            },
        }),
        system: {
            openExternalUrl(url) {
                window.open(url, '_blank');
            },
            exit() {
                jmp.send('app.exit');
            },
            cancelServerConnectivity() {
                if (window.jmpCheckServerConnectivity && window.jmpCheckServerConnectivity.abort) {
                    window.jmpCheckServerConnectivity.abort();
                }
            }
        },
        settings: {
            setValue(section, key, value, callback) {
                jmp.send('app.setSettingValue', {
                    section,
                    key,
                    value: typeof value === 'boolean' ? (value ? 'true' : 'false') : String(value),
                });
                if (callback) callback();
            },
            sectionValueUpdate: createSignal('sectionValueUpdate'),
            groupUpdate: createSignal('groupUpdate')
        },
        input: {
            // Signals for media session control commands (driven by bus inbound)
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

    // Inbound bus subscriptions — translate player.* notifications into the
    // signal surface mpv-player-core depends on.
    jmp.on('player.playing', () => { playerSignals.playing(); });
    jmp.on('player.paused', () => {
        playerState.paused = true;
        playerSignals.paused();
    });
    jmp.on('player.stopped', () => {
        playerState.paused = false;
        // mpv-player-core treats `finished` as end-of-media; preserve that mapping.
        playerSignals.finished();
        playerSignals.stopped();
    });
    jmp.on('player.error', (p) => { playerSignals.error(p && p.message); });
    jmp.on('player.tick', (p) => {
        if (!p) return;
        const ms = p.positionMs || 0;
        playerState.position = ms;
        playerSignals.positionUpdate(ms);
    });
    jmp.on('player.durationChanged', (p) => {
        if (!p) return;
        const ms = p.durationMs || 0;
        playerState.duration = ms;
        playerSignals.updateDuration(ms);
    });
    jmp.on('player.seeking', (p) => {
        if (p && p.active) playerSignals.seeking();
    });
    jmp.on('player.bufferedRangesChanged', (p) => {
        _bufferedRanges = (p && p.ranges) || [];
    });
    jmp.on('player.positionReply', (p) => {
        if (!p || p.requestId == null) return;
        const cb = playerState.pendingPositionRequests.get(p.requestId);
        if (cb) {
            playerState.pendingPositionRequests.delete(p.requestId);
            cb(p.positionMs || 0);
        }
    });

    // Async position lookup — replaces the old getPosition(callback) sync path
    // for callers that need a real-time fetch from mpv. Stored on the player
    // object so MpvPlayerCore.currentTimeAsync can use it.
    window.api.player.getPositionAsync = function(callback) {
        const id = playerState.nextRequestId++;
        playerState.pendingPositionRequests.set(id, callback);
        jmp.send('player.getPosition', { requestId: id });
    };

    // Inbound media session / fullscreen commands
    jmp.on('input.hostInput', (p) => {
        if (p && Array.isArray(p.actions)) window.api.input.hostInput(p.actions);
    });
    jmp.on('input.positionSeek', (p) => {
        if (p && p.positionMs != null) window.api.input.positionSeek(p.positionMs);
    });
    jmp.on('input.rateChanged', (p) => {
        if (p && p.rate != null) window.api.input.rateChanged(p.rate);
    });
    jmp.on('fullscreen.changed', (p) => {
        const fs = !!(p && p.fullscreen);
        window._isFullscreen = fs;
        const player = window._mpvVideoPlayerInstance;
        if (player && player.events) {
            player.events.trigger(player, 'fullscreenchange');
        }
    });

    // window.NativeShell - app info and plugins
    const plugins = ['mpvVideoPlayer', 'mpvAudioPlayer', 'inputPlugin'];
    for (const plugin of plugins) {
        window[plugin] = () => window['_' + plugin];
    }

    window.NativeShell = {
        openUrl(url) {
            window.api.system.openExternalUrl(url);
        },
        downloadFile(info) {
            window.api.system.openExternalUrl(info.url);
        },
        openClientSettings() {
            window._openClientSettings();
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
                'fileinput', 'filedownload', 'displaylanguage', 'htmlaudioautoplay',
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
    function sendThemeColor(color) {
        if (color) jmp.send('app.themeColor', { color });
    }

    function restoreThemeColor() {
        const meta = document.querySelector('meta[name="theme-color"]');
        if (meta) sendThemeColor(meta.content);
    }

    function observeThemeColorMeta(meta) {
        sendThemeColor(meta.content);
        new MutationObserver(() => sendThemeColor(meta.content))
            .observe(meta, { attributes: true, attributeFilter: ['content'] });
    }

    document.addEventListener('DOMContentLoaded', () => {
        const style = document.createElement('style');
        let css = 'body.mouseIdle, body.mouseIdle * { cursor: none !important; }';

        if (navigator.platform.startsWith('Mac') && jmpInfo.settings.advanced.transparentTitlebar) {
            css += '\n:root { --mac-titlebar-height: 28px; }';
            css += '\n.skinHeader { padding-top: var(--mac-titlebar-height) !important; }';
            css += '\n.mainAnimatedPage { top: var(--mac-titlebar-height) !important; }';
            css += '\n.touch-menu-la { padding-top: var(--mac-titlebar-height); }';
            css += '\n.MuiAppBar-positionFixed { padding-top: var(--mac-titlebar-height) !important; }';
            css += '\n.MuiDrawer-paper { padding-top: var(--mac-titlebar-height) !important; }';
            css += '\n.formDialogHeader { padding-top: var(--mac-titlebar-height) !important; }';

            document._callbacks = document._callbacks || {};
            document._callbacks['SHOW_VIDEO_OSD'] = document._callbacks['SHOW_VIDEO_OSD'] || [];
            document._callbacks['SHOW_VIDEO_OSD'].push((_e, visible) => {
                jmp.send('osd.active', { active: !!visible });
            });
        }

        style.textContent = css;
        document.head.appendChild(style);

        // Titlebar black during video playback, restore theme color when done
        window.api.player.playing.connect(() => {
            if (window._jmpVideoActive) sendThemeColor('#000000');
        });
        window.api.player.finished.connect(() => { window._jmpVideoActive = false; restoreThemeColor(); });
        window.api.player.stopped.connect(() => { window._jmpVideoActive = false; restoreThemeColor(); });
        window.api.player.canceled.connect(() => { window._jmpVideoActive = false; restoreThemeColor(); });
        window.api.player.error.connect(() => { window._jmpVideoActive = false; restoreThemeColor(); });

        // Watch for mouseIdle class on body and tell native to hide/show cursor.
        new MutationObserver(() => {
            const idle = document.body.classList.contains('mouseIdle');
            jmp.send('osd.cursorVisible', { visible: !idle });
        }).observe(document.body, { attributes: true, attributeFilter: ['class'] });

        // Sync titlebar color with theme-color meta tag
        const meta = document.querySelector('meta[name="theme-color"]');
        if (meta) {
            observeThemeColorMeta(meta);
        } else {
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

    console.log('[Media] Native shim installed');
})();
