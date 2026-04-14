(function() {
    function getMediaStreamAudioTracks(mediaSource) {
        return mediaSource.MediaStreams.filter(s => s.Type === 'Audio');
    }

    // Convert Jellyfin global MediaStream.Index to 1-based type-relative index
    function getRelativeIndexByType(mediaStreams, jellyIndex, streamType) {
        let relIndex = 1;
        for (const source of mediaStreams) {
            if (source.Type !== streamType || source.IsExternal) continue;
            if (source.Index === jellyIndex) return relIndex;
            relIndex += 1;
        }
        return null;
    }

    function getStreamByIndex(mediaStreams, index) {
        return mediaStreams.find(s => s.Index === index) || null;
    }

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

            // Register for fullscreen notifications
            window._mpvVideoPlayerInstance = this;

            // Use defineProperty to avoid circular reference in JSON.stringify
            Object.defineProperty(this, '_core', {
                value: new window.MpvPlayerCore(events),
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

            // Set up video-specific event handlers
            this._core.handlers.onPlaying = () => {
                if (!this._started) {
                    this._started = true;
                    this.loading.hide();
                    const dlg = this._videoDialog;
                    // Remove poster so video shows through from subsurface
                    if (dlg) {
                        const poster = dlg.querySelector('.mpvPoster');
                        if (poster) poster.remove();
                    }
                    // "fullscreen" = fills entire web content area, not the actual screen
                    if (this._currentPlayOptions?.fullscreen) {
                        this.appRouter.showVideoOsd();
                        if (dlg) dlg.style.zIndex = 'unset';
                    }
                    window.api.player.setVideoRectangle(0, 0, 0, 0);
                }
                if (this._core._paused) {
                    this._core._paused = false;
                    this.events.trigger(this, 'unpause');
                }
                this._core.startTimeUpdateTimer();
                this.events.trigger(this, 'playing');
                console.log('[Media] [MPV] playing event triggered');
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
            console.log('[Media] [MPV] play() called with options:', options);
            this._started = false;
            this._timeUpdated = false;
            this._core._currentTime = null;
            this._endedPending = false;
            if (options.fullscreen) this.loading.show();  // fills entire web content area, not the actual screen
            await this.createMediaElement(options);
            console.log('[Media] [MPV] createMediaElement done, calling setCurrentSrc');
            return await this.setCurrentSrc(options);
        }

        getSavedVolume() {
            return this.appSettings.get('volume') || 1;
        }

        setCurrentSrc(options) {
            return new Promise((resolve) => {
                const val = options.url;
                this._currentSrc = val;
                console.log('[Media] [MPV] Playing:', val);

                const ms = Math.round((options.playerStartPositionTicks || 0) / 10000);
                this._currentPlayOptions = options;
                this._core._currentTime = ms;

                const streams = options.mediaSource?.MediaStreams || [];
                const defaultAudioIdx = options.mediaSource.DefaultAudioStreamIndex ?? -1;
                const defaultSubIdx = options.mediaSource.DefaultSubtitleStreamIndex ?? -1;

                // Convert audio index from Jellyfin global stream index to mpv 1-based audio track index
                let audioParam = MpvPlayerCore.TRACK_DISABLE;
                if (defaultAudioIdx >= 0) {
                    const relIdx = getRelativeIndexByType(streams, defaultAudioIdx, 'Audio');
                    audioParam = relIdx != null ? relIdx : MpvPlayerCore.TRACK_AUTO;
                }

                // Convert subtitle index to relative
                let subParam = MpvPlayerCore.TRACK_DISABLE;
                let externalSubUrl = null;
                if (defaultSubIdx >= 0) {
                    const subStream = getStreamByIndex(streams, defaultSubIdx);
                    if (subStream && subStream.DeliveryMethod === 'External' && subStream.DeliveryUrl) {
                        externalSubUrl = subStream.DeliveryUrl;
                    } else {
                        const relIdx = getRelativeIndexByType(streams, defaultSubIdx, 'Subtitle');
                        subParam = relIdx != null ? relIdx : MpvPlayerCore.TRACK_AUTO;
                    }
                }

                window.api.player.load(val,
                    { startMilliseconds: ms, autoplay: true },
                    { type: 'video', metadata: options.item },
                    audioParam,
                    subParam,
                    resolve);

                if (externalSubUrl) {
                    window.api.player.addSubtitleStream(externalSubUrl);
                }
            });
        }

        setSubtitleStreamIndex(index) {
            if (index == null || index < 0) {
                window.api.player.setSubtitleStream(MpvPlayerCore.TRACK_DISABLE);
                return;
            }
            const streams = this._currentPlayOptions?.mediaSource?.MediaStreams || [];
            const stream = getStreamByIndex(streams, index);
            if (stream && stream.DeliveryMethod === 'External' && stream.DeliveryUrl) {
                window.api.player.addSubtitleStream(stream.DeliveryUrl);
                return;
            }
            const relIdx = getRelativeIndexByType(streams, index, 'Subtitle');
            window.api.player.setSubtitleStream(relIdx != null ? relIdx : MpvPlayerCore.TRACK_DISABLE);
        }

        setSecondarySubtitleStreamIndex(index) {}

        resetSubtitleOffset() {
            window.api.player.setSubtitleDelay(0);
        }

        enableShowingSubtitleOffset() {}
        disableShowingSubtitleOffset() {}
        isShowingSubtitleOffsetEnabled() { return false; }
        setSubtitleOffset(offset) { window.api.player.setSubtitleDelay(Math.round(offset * 1000)); }
        getSubtitleOffset() { return 0; }

        setAudioStreamIndex(index) {
            if (index == null || index < 0) {
                window.api.player.setAudioStream(MpvPlayerCore.TRACK_AUTO);
                return;
            }
            const streams = this._currentPlayOptions?.mediaSource?.MediaStreams || [];
            const relIdx = getRelativeIndexByType(streams, index, 'Audio');
            window.api.player.setAudioStream(relIdx != null ? relIdx : MpvPlayerCore.TRACK_AUTO);
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
            window.api.player.stop();
            this._core.handlers.onEnded();
            if (destroyPlayer) this.destroy();
            return Promise.resolve();
        }

        removeMediaDialog() {
            window.api.player.stop();
            if (window.jmpNative) window.jmpNative.playerOsdActive(false);
            window.api.player.setVideoRectangle(-1, 0, 0, 0);
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
        }

        createMediaElement(options) {
            let dlg = document.querySelector('.videoPlayerContainer');
            if (!dlg) {
                if (window.jmpNative) window.jmpNative.playerOsdActive(true);
                dlg = document.createElement('div');
                dlg.classList.add('videoPlayerContainer');
                dlg.style.cssText = 'position:fixed;top:0;bottom:0;left:0;right:0;display:flex;align-items:center;background:transparent;';
                if (options.fullscreen) dlg.style.zIndex = 1000;  // fills entire web content area, not the actual screen
                document.body.insertBefore(dlg, document.body.firstChild);
                this.setTransparency(2);
                this._videoDialog = dlg;

                this._core.connectSignals();
                if (window.jmpNative) {
                    window.jmpNative.notifyRateChange(this._core._playRate);
                }
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
            if (options.fullscreen) document.body.classList.add('hide-scroll');  // fills entire web content area, not the actual screen
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
            if (window.jmpNative) window.jmpNative.toggleFullscreen();
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
            if (window.jmpNative) window.jmpNative.notifyRateChange(value);
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

        saveVolume(value) { if (value) this.appSettings.set('volume', value); }
        setVolume(val, save = true) {
            val = Number(val);
            if (!isNaN(val)) {
                this._core._volume = val;
                if (save) { this.saveVolume(val / 100); this.events.trigger(this, 'volumechange'); }
                window.api.player.setVolume(val);
            }
        }
        getVolume() { return this._core.getVolume(); }
        volumeUp() { this.setVolume(Math.min(this.getVolume() + 2, 100)); }
        volumeDown() { this.setVolume(Math.max(this.getVolume() - 2, 0)); }

        setMute(mute, triggerEvent = true) {
            this._core._muted = mute;
            window.api.player.setMuted(mute);
            if (triggerEvent) this.events.trigger(this, 'volumechange');
        }
        isMuted() { return this._core.isMuted(); }

        togglePictureInPicture() {}
        toggleAirPlay() {}
        getStats() { return Promise.resolve({ categories: [] }); }
        getSupportedAspectRatios() { return []; }
        getAspectRatio() { return 'normal'; }
        setAspectRatio(value) {}
    }

    window._mpvVideoPlayer = mpvVideoPlayer;
    console.log('[Media] mpvVideoPlayer class installed');
})();
