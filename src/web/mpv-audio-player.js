(function() {
    let fadeTimeout;

    function fade(instance, startingVolume) {
        instance._isFadingOut = true;
        const newVolume = Math.max(0, startingVolume - 15);
        instance._core.setVolume(newVolume, false);

        if (newVolume <= 0) {
            instance._isFadingOut = false;
            return Promise.resolve();
        }

        return new Promise((resolve, reject) => {
            cancelFadeTimeout();
            fadeTimeout = setTimeout(() => {
                fade(instance, newVolume).then(resolve, reject);
            }, 100);
        });
    }

    function cancelFadeTimeout() {
        if (fadeTimeout) {
            clearTimeout(fadeTimeout);
            fadeTimeout = null;
        }
    }

    class mpvAudioPlayer {
        constructor({ events, appHost, appSettings }) {
            this.events = events;
            this.appHost = appHost;
            this.appSettings = appSettings;

            this.name = 'MPV Audio Player';
            this.type = 'mediaplayer';
            this.id = 'mpvaudioplayer';
            this.syncPlayWrapAs = 'htmlaudioplayer';
            this.useServerPlaybackInfoForAudio = true;

            // Use defineProperty to avoid circular reference in JSON.stringify
            Object.defineProperty(this, '_core', {
                value: new window.MpvPlayerCore(events, appSettings),
                writable: true,
                enumerable: false
            });
            this._core.player = this;

            this._currentSrc = null;
            this._currentPlayOptions = null;
            this._started = false;
            this._isFadingOut = false;

            // Set up event handlers
            this._core.handlers.onPlaying = () => {
                if (!this._started) {
                    this._started = true;
                    const volume = this.getSavedVolume() * 100;
                    this.setVolume(volume, volume !== this._core._volume);
                }
                this.setPlaybackRate(this.getPlaybackRate());
                if (this._core._paused) {
                    this._core._paused = false;
                    this.events.trigger(this, 'unpause');
                }
                this._core.startTimeUpdateTimer();
                this.events.trigger(this, 'playing');
            };

            this._core.handlers.onTimeUpdate = (time) => {
                if (!this._isFadingOut) {
                    this._core._seeking = false;
                    this._core._currentTime = time;
                    this._core._lastTimerTick = Date.now();
                    this.events.trigger(this, 'timeupdate');
                }
            };

            this._core.handlers.onSeeking = () => {
                this._core._seeking = true;
            };

            this._core.handlers.onEnded = () => {
                this.onEndedInternal();
            };

            this._core.handlers.onPause = () => {
                this._core._paused = true;
                this._core.stopTimeUpdateTimer();
                this.events.trigger(this, 'pause');
            };

            this._core.handlers.onDuration = (duration) => {
                this._core._duration = duration;
            };

            this._core.handlers.onError = (error) => {
                console.error('[Media] [Audio] media error:', error);
                this.events.trigger(this, 'error', [{ type: 'mediadecodeerror' }]);
            };
        }

        play(options) {
            this._started = false;
            this._core._currentTime = null;
            this._core._duration = undefined;
            this._core.connectSignals();
            return this.setCurrentSrc(options);
        }

        setCurrentSrc(options) {
            return new Promise((resolve) => {
                const val = options.url;
                this._currentSrc = val;
                console.log('[Media] [Audio] Playing:', val);

                const ms = Math.round((options.playerStartPositionTicks || 0) / 10000);
                this._currentPlayOptions = options;
                this._core._currentTime = ms;

                window.api.player.load(val,
                    { startMilliseconds: ms, autoplay: true },
                    { type: 'music', metadata: options.item },
                    MpvPlayerCore.TRACK_AUTO,
                    MpvPlayerCore.TRACK_AUTO,
                    resolve);
            });
        }

        onEndedInternal() {
            this._core.stopTimeUpdateTimer();
            this.events.trigger(this, 'stopped', [{ src: this._currentSrc }]);
            this._core._currentTime = null;
            this._currentSrc = null;
            this._currentPlayOptions = null;
        }

        stop(destroyPlayer) {
            cancelFadeTimeout();
            const src = this._currentSrc;

            if (src) {
                if (!destroyPlayer) {
                    this.pause();
                    this.onEndedInternal();
                    return Promise.resolve();
                }

                const originalVolume = this._core._volume;
                return fade(this, this._core._volume).then(() => {
                    this.pause();
                    this.setVolume(originalVolume, false);
                    this.onEndedInternal();
                    this.destroy();
                });
            }
            return Promise.resolve();
        }

        destroy() {
            this._core.stopTimeUpdateTimer();
            window.api.player.stop();
            this._core.disconnectSignals();
            this._core._duration = undefined;
        }

        currentSrc() { return this._currentSrc; }

        canPlayMediaType(mediaType) {
            return (mediaType || '').toLowerCase() === 'audio';
        }

        getDeviceProfile(item, options) {
            if (this.appHost && this.appHost.getDeviceProfile) {
                return this.appHost.getDeviceProfile(item, options);
            }
            return Promise.resolve({});
        }

        // Delegate to core
        currentTime(val) { return this._core.currentTime(val); }
        currentTimeAsync() { return this._core.currentTimeAsync(); }
        duration() { return this._core.duration(); }
        seekable() { return this._core.seekable(); }
        getBufferedRanges() { return this._core.getBufferedRanges(); }
        pause() { this._core.pause(); }
        resume() { this._core.resume(); }
        unpause() { this._core.unpause(); }
        paused() { return this._core.paused(); }

        setPlaybackRate(value) { this._core.setPlaybackRate(value); }
        getPlaybackRate() { return this._core.getPlaybackRate(); }
        getSupportedPlaybackRates() { return this._core.getSupportedPlaybackRates(); }

        saveVolume(value) { this._core.saveVolume(value); }
        getSavedVolume() { return this._core.getSavedVolume(); }
        setVolume(val, save = true) { this._core.setVolume(val, save); }
        getVolume() { return this._core.getVolume(); }
        volumeUp() { this._core.volumeUp(); }
        volumeDown() { this._core.volumeDown(); }

        setMute(mute, triggerEvent = true) { this._core.setMute(mute, triggerEvent); }
        isMuted() { return this._core.isMuted(); }

        supports(feature) {
            return ['PlaybackRate'].includes(feature);
        }
    }

    window._mpvAudioPlayer = mpvAudioPlayer;
    console.log('[Media] mpvAudioPlayer plugin installed');
})();
