(function() {
    class mpvVideoPlayer {
        constructor({ events, loading, appRouter, globalize, appHost, appSettings, confirm, dashboard }) {
            this.events = events;
            this.loading = loading;
            this.appRouter = appRouter;
            this.globalize = globalize;
            this.appHost = appHost;
            this.appSettings = appSettings;
            if (dashboard && dashboard.default) {
                this.setTransparency = dashboard.default.setBackdropTransparency.bind(dashboard);
            } else {
                this.setTransparency = () => {};
            }

            this.name = 'MPV Video Player';
            this.type = 'mediaplayer';
            this.id = 'mpvvideoplayer';
            this.syncPlayWrapAs = 'htmlvideoplayer';
            this.priority = -1;
            this.useFullSubtitleUrls = true;
            this.isLocalPlayer = true;
            this.isFetching = false;

            window._mpvVideoPlayerInstance = this;

            // Use defineProperty to avoid circular reference in JSON.stringify
            Object.defineProperty(this, '_core', {
                value: new window.MpvPlayerCore(events, appSettings),
                writable: true,
                enumerable: false
            });
            this._core.player = this;

            this._videoDialog = undefined;
            this._currentSrc = undefined;
            this._started = false;
            this._timeUpdated = false;
            this._currentPlayOptions = undefined;
            this._endedPending = false;

            // Support jellyfin-web v10.10.7
            this._currentAspectRatio = undefined;

            // Set up video-specific event handlers
            this._core.handlers.onPlaying = () => {
                if (!this._started) {
                    this._started = true;
                    this.loading.hide();
                    const dlg = this._videoDialog;
                    if (dlg) {
                        const poster = dlg.querySelector('.mpvPoster');
                        if (poster) poster.remove();
                    }
                    if (this._currentPlayOptions?.fullscreen) {
                        this.appRouter.showVideoOsd();
                        if (dlg) dlg.style.zIndex = 'unset';
                    }
                    window.jmp.send('player.setVideoRectangle', { x: 0, y: 0, w: 0, h: 0 });
                }
                if (this._core._paused) {
                    this._core._paused = false;
                    this.events.trigger(this, 'unpause');
                }
                this._core.startTimeUpdateTimer();
                this.events.trigger(this, 'playing');
            };

            this._core.handlers.onTimeUpdate = (time) => {
                if (time && !this._timeUpdated) this._timeUpdated = true;
                this._core._seeking = false;
                this._core._currentTime = time;
                this._core._lastTimerTick = Date.now();
                this.events.trigger(this, 'timeupdate');
            };

            this._core.handlers.onSeeking = () => {
                this._core._seeking = true;
            };

            this._core.handlers.onEnded = () => {
                if (!this._endedPending) {
                    this._endedPending = true;
                    this.onEndedInternal();
                }
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
                this.removeMediaDialog();
                console.error('[Media] media error:', error);
                this.events.trigger(this, 'error', [{ type: 'mediadecodeerror' }]);
            };
        }

        currentSrc() { return this._currentSrc; }

        async play(options) {
            this._started = false;
            this._timeUpdated = false;
            this._core._currentTime = null;
            this._endedPending = false;
            if (options.fullscreen) this.loading.show();
            await this.createMediaElement(options);
            return await this.setCurrentSrc(options);
        }

        setCurrentSrc(options) {
            return new Promise((resolve) => {
                const val = options.url;
                this._currentSrc = val;

                const ms = Math.round((options.playerStartPositionTicks || 0) / 10000);
                this._currentPlayOptions = options;
                this._core._currentTime = ms;

                window._jmpVideoActive = true;

                // Wait for first playing/error to resolve play().
                const onPlaying = () => {
                    window.api.player.playing.disconnect(onPlaying);
                    window.api.player.error.disconnect(onError);
                    resolve();
                };
                const onError = () => {
                    window.api.player.playing.disconnect(onPlaying);
                    window.api.player.error.disconnect(onError);
                    resolve();
                };
                window.api.player.playing.connect(onPlaying);
                window.api.player.error.connect(onError);

                window.jmp.send('player.setAspectRatio', { mode: this.getAspectRatio() });
                window.jmp.send('player.load', {
                    url: val,
                    startMs: ms,
                    item: options.item,
                    mediaSource: options.mediaSource,
                    defaultAudioIdx: options.mediaSource?.DefaultAudioStreamIndex ?? null,
                    defaultSubIdx:   options.mediaSource?.DefaultSubtitleStreamIndex ?? null,
                });
            });
        }

        setSubtitleStreamIndex(index) {
            window.jmp.send('player.selectSubtitle', {
                jellyfinIndex: (index == null || index < 0) ? null : index,
            });
        }

        setSecondarySubtitleStreamIndex() {}

        resetSubtitleOffset() {
            window.jmp.send('player.setSubtitleOffset', { seconds: 0 });
        }

        enableShowingSubtitleOffset() {}
        disableShowingSubtitleOffset() {}
        isShowingSubtitleOffsetEnabled() { return false; }
        setSubtitleOffset(offset) {
            window.jmp.send('player.setSubtitleOffset', { seconds: offset });
        }
        getSubtitleOffset() { return 0; }

        setAudioStreamIndex(index) {
            window.jmp.send('player.selectAudio', {
                jellyfinIndex: (index == null || index < 0) ? null : index,
            });
        }

        onEndedInternal() {
            this.events.trigger(this, 'stopped', [{ src: this._currentSrc }]);
            this._core._currentTime = null;
            this._currentSrc = null;
            this._currentPlayOptions = null;
        }

        stop(destroyPlayer) {
            if (!destroyPlayer && this._videoDialog && this._currentPlayOptions?.backdropUrl) {
                const dlg = this._videoDialog;
                const url = this._currentPlayOptions.backdropUrl;
                if (!dlg.querySelector('.mpvPoster')) {
                    const poster = document.createElement('div');
                    poster.classList.add('mpvPoster');
                    poster.style.cssText = `position:absolute;top:0;left:0;right:0;bottom:0;background:#000 url('${url}') center/cover no-repeat;`;
                    dlg.appendChild(poster);
                }
            }
            window.jmp.send('player.stop');
            this._core.handlers.onEnded();
            if (destroyPlayer) this.destroy();
            return Promise.resolve();
        }

        removeMediaDialog() {
            window.jmp.send('player.stop');
            window.jmp.send('osd.active', { active: false });
            document.body.classList.remove('hide-scroll');
            const dlg = this._videoDialog;
            if (dlg) {
                this.setTransparency(0);
                this._videoDialog = null;
                dlg.parentNode.removeChild(dlg);
            }
        }

        destroy() {
            this._core.stopTimeUpdateTimer();
            this.removeMediaDialog();
            this._core.disconnectSignals();
            this._currentAspectRatio = undefined;
        }

        createMediaElement(options) {
            let dlg = document.querySelector('.videoPlayerContainer');
            if (!dlg) {
                window.jmp.send('osd.active', { active: true });
                dlg = document.createElement('div');
                dlg.classList.add('videoPlayerContainer');
                dlg.style.cssText = 'position:fixed;top:0;bottom:0;left:0;right:0;display:flex;align-items:center;background:transparent;';
                if (options.fullscreen) dlg.style.zIndex = 1000;
                document.body.insertBefore(dlg, document.body.firstChild);
                this.setTransparency(2);
                this._videoDialog = dlg;

                this._core.connectSignals();
                window.jmp.send('input.rateChanged', { rate: this._core._playRate });
            } else {
                this._videoDialog = dlg;
            }
            if (options.backdropUrl) {
                const existing = dlg.querySelector('.mpvPoster');
                if (existing) existing.remove();
                const poster = document.createElement('div');
                poster.classList.add('mpvPoster');
                poster.style.cssText = `position:absolute;top:0;left:0;right:0;bottom:0;background:#000 url('${options.backdropUrl}') center/cover no-repeat;`;
                dlg.appendChild(poster);
            }
            if (options.fullscreen) document.body.classList.add('hide-scroll');
            return Promise.resolve();
        }

        canPlayMediaType(mediaType) {
            return (mediaType || '').toLowerCase() === 'video';
        }
        canPlayItem(item) { return this.canPlayMediaType(item.MediaType); }
        supportsPlayMethod() { return true; }
        getDeviceProfile(item, options) {
            return this.appHost.getDeviceProfile ? this.appHost.getDeviceProfile(item, options) : Promise.resolve({});
        }
        static getSupportedFeatures() { return ['PlaybackRate', 'SetAspectRatio']; }
        supports(feature) { return mpvVideoPlayer.getSupportedFeatures().includes(feature); }
        isFullscreen() { return window._isFullscreen === true; }
        toggleFullscreen() {
            window.jmp.send('fullscreen.toggle');
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

        setPlaybackRate(value) {
            this._core.setPlaybackRate(value);
            window.jmp.send('input.rateChanged', { rate: value });
        }
        getPlaybackRate() { return this._core.getPlaybackRate(); }
        getSupportedPlaybackRates() { return this._core.getSupportedPlaybackRates(); }

        canSetAudioStreamIndex() { return true; }
        setPictureInPictureEnabled() {}
        isPictureInPictureEnabled() { return false; }
        isAirPlayEnabled() { return false; }
        setAirPlayEnabled() {}
        setBrightness() {}
        getBrightness() { return 100; }

        saveVolume(value) { this._core.saveVolume(value); }
        getSavedVolume() { return this._core.getSavedVolume(); }
        setVolume(val, save = true) { this._core.setVolume(val, save); }
        getVolume() { return this._core.getVolume(); }
        volumeUp() { this._core.volumeUp(); }
        volumeDown() { this._core.volumeDown(); }

        setMute(mute, triggerEvent = true) { this._core.setMute(mute, triggerEvent); }
        isMuted() { return this._core.isMuted(); }

        togglePictureInPicture() {}
        toggleAirPlay() {}
        getStats() { return Promise.resolve({ categories: [] }); }
        getSupportedAspectRatios() {
            return [
                { id: 'auto',  name: this.globalize.translate('Auto') },
                { id: 'cover', name: this.globalize.translate('AspectRatioCover') },
                { id: 'fill',  name: this.globalize.translate('AspectRatioFill') }
            ];
        }
        getAspectRatio() {
            const aspectRatio = typeof this.appSettings.aspectRatio === 'function'
                ? this.appSettings.aspectRatio()
                : this._currentAspectRatio;
            return aspectRatio || 'auto';
        }
        setAspectRatio(value) {
            if (typeof this.appSettings.aspectRatio === 'function') {
                this.appSettings.aspectRatio(value);
            } else {
                this._currentAspectRatio = value;
            }
            window.jmp.send('player.setAspectRatio', { mode: value });
        }
    }

    window._mpvVideoPlayer = mpvVideoPlayer;
})();
