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
            console.log('[Media] [Signal] ' + name + ' firing with', callbacks.length, 'listeners');
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

    // window.jmpInfo - settings and device info
    window.jmpInfo = {
        version: '1.0.0',
        deviceName: 'Jellyfin Desktop CEF',
        mode: 'desktop',
        userAgent: navigator.userAgent,
        scriptPath: '',
        sections: [
            { key: 'main', order: 0 },
            { key: 'audio', order: 1 },
            { key: 'video', order: 2 }
        ],
        settings: {
            main: { enableMPV: true, fullscreen: false, userWebClient: '__SERVER_URL__' },
            audio: { channels: '2.0' },
            video: {
                force_transcode_dovi: false,
                force_transcode_hdr: false,
                force_transcode_hi10p: false,
                force_transcode_hevc: false,
                force_transcode_av1: false,
                force_transcode_4k: false,
                always_force_transcode: false,
                allow_transcode_to_hevc: true,
                prefer_transcode_to_h265: false,
                aspect: 'normal',
                default_playback_speed: 1
            }
        },
        settingsDescriptions: {
            video: [{ key: 'aspect', options: [
                { value: 'normal', title: 'video.aspect.normal' },
                { value: 'zoom', title: 'video.aspect.zoom' },
                { value: 'stretch', title: 'video.aspect.stretch' }
            ]}]
        },
        settingsUpdate: [],
        settingsDescriptionsUpdate: []
    };

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
        // Update position immediately and set rate to 0 during buffering
        if (window.jmpNative) {
            window.jmpNative.notifyPosition(Math.floor(positionMs));
            window.jmpNative.notifyRateChange(0.0);
        }
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
        openClientSettings() {},
        getPlugins() {
            return plugins;
        }
    };

    // Device profile for direct play
    function getDeviceProfile() {
        return {
            Name: 'Jellyfin Desktop CEF',
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
                appName: 'Jellyfin Desktop CEF',
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
                'exitmenu'
            ];
            return features.includes(command.toLowerCase());
        },
        getDeviceProfile,
        getSyncProfile: getDeviceProfile,
        appName() { return 'Jellyfin Desktop CEF'; },
        appVersion() { return jmpInfo.version; },
        deviceName() { return jmpInfo.deviceName; },
        exit() { window.api.system.exit(); }
    };

    window.initCompleted = Promise.resolve();
    window.apiPromise = Promise.resolve(window.api);

    // Inject CSS to hide cursor when jellyfin-web signals mouse idle.
    // jellyfin-web adds 'mouseIdle' to body after inactivity during video playback.
    // This CSS makes CEF report CT_NONE so the native side can hide the OS cursor.
    document.addEventListener('DOMContentLoaded', () => {
        const style = document.createElement('style');
        style.textContent = 'body.mouseIdle, body.mouseIdle * { cursor: none !important; }';
        document.head.appendChild(style);
    });

    console.log('[Media] Native shim installed');
})();
