/* eslint-disable indent */
(function() {
    function getMediaStreamAudioTracks(mediaSource) {
        return mediaSource.MediaStreams.filter(function (s) {
            return s.Type === 'Audio';
        });
    }

    class mpvVideoPlayer {
        constructor({ events, loading, appRouter, globalize, appHost, appSettings, confirm, dashboard }) {
            this.events = events;
            this.loading = loading;
            this.appRouter = appRouter;
            this.globalize = globalize;
            this.appHost = appHost;
            this.appSettings = appSettings;

            this.setTransparency = dashboard.default.setBackdropTransparency.bind(dashboard);

            /**
             * @type {string}
             */
            this.name = 'MPV Video Player';
            /**
             * @type {string}
             */
            this.type = 'mediaplayer';
            /**
             * @type {string}
             */
            this.id = 'mpvvideoplayer';
            this.syncPlayWrapAs = 'htmlvideoplayer';
            this.priority = -1;
            this.useFullSubtitleUrls = true;
            /**
             * @type {boolean}
             */
            this.isLocalPlayer = true;
            /**
             * @type {boolean}
             */
            this.isFetching = false;

            /**
             * @type {HTMLDivElement | null | undefined}
             */
            this._videoDialog = undefined;
            /**
             * @type {number | undefined}
             */
            this._subtitleTrackIndexToSetOnPlaying = undefined;
            /**
             * @type {number | null}
             */
            this._audioTrackIndexToSetOnPlaying = undefined;
            /**
             * @type {boolean | undefined}
             */
            this._showTrackOffset = undefined;
            /**
             * @type {number | undefined}
             */
            this._currentTrackOffset = undefined;
            /**
             * @type {string[] | undefined}
             */
            this._supportedFeatures = undefined;
            /**
             * @type {string | undefined}
             */
            this._currentSrc = undefined;
            /**
             * @type {boolean | undefined}
             */
            this._started = undefined;
            /**
             * @type {boolean | undefined}
             */
            this._timeUpdated = undefined;
            /**
             * @type {number | null | undefined}
             */
            this._currentTime = undefined;
            /**
             * @private (used in other files)
             * @type {any | undefined}
             */
            this._currentPlayOptions = undefined;
            /**
             * @type {any | undefined}
             */
            this._lastProfile = undefined;
            /**
             * @type {number | undefined}
             */
            this._duration = undefined;
            /**
             * @type {boolean}
             */
            this._paused = false;
            /**
             * @type {int}
             */
            this._volume = 100;
            /**
             * @type {boolean}
             */
            this._muted = false;
            /**
             * @type {float}
             */
            this._playRate;
            /**
             * @type {boolean}
             */
            this._hasConnection = false;
            /**
             * @type {Array<{start: number, end: number}>}
             */
            this._bufferedRanges = [];

            /**
             * @private
             */
            this.onBufferedRangesUpdated = (ranges) => {
                this._bufferedRanges = ranges;
            };

            /**
             * @private
             */
            this.onEnded = () => {
                this.onEndedInternal();
            };

            /**
             * @private
             */
            this.onTimeUpdate = (time) => {
                if (time && !this._timeUpdated) {
                    this._timeUpdated = true;
                }

                this._currentTime = time;
                this.events.trigger(this, 'timeupdate');
            };

            /**
             * @private
             */
            this.onPlaying = () => {
                if (!this._started) {
                    this._started = true;

                    this.loading.hide();

                    const volume = this.getSavedVolume() * 100;
                    this.setVolume(volume, false);

                    this.setPlaybackRate(this.getPlaybackRate());

                    // Hide backdrop when playback starts
                    const dlg = this._videoDialog;
                    if (dlg) {
                        dlg.style.backgroundImage = '';
                    }

                    // Navigate to OSD view to show playback screen
                    if (this._currentPlayOptions.fullscreen) {
                        this.appRouter.showVideoOsd();
                        // Lower video dialog z-index so OSD can receive input
                        if (dlg) {
                            dlg.style.zIndex = 'unset';
                        }
                    }

                    // Keep video fullscreen - native OSD is above it
                    window.api.player.setVideoRectangle(0, 0, 0, 0);
                }

                if (this._paused) {
                    this._paused = false;
                    this.events.trigger(this, 'unpause');
                }

                this.events.trigger(this, 'playing');
            };

            /**
             * @private
             */
            this.onPause = () => {
                this._paused = true;
                // For Syncplay ready notification
                this.events.trigger(this, 'pause');
            };

            this.onWaiting = () => {
                this.events.trigger(this, 'waiting');
            };

            /**
             * @private
             * @param e {Event} The event received from the `<video>` element
             */
            this.onError = async (error) => {
                this.removeMediaDialog();
                console.error(`media error: ${error}`);

                const errorData = {
                    type: 'mediadecodeerror'
                };

                try {
                    await confirm({
                        title: "Playback Failed",
                        text: `Playback failed with error "${error}". Retry with transcode? (Note this may hang the player.)`,
                        cancelText: "Cancel",
                        confirmText: "Retry"
                    });
                } catch (ex) {
                    // User declined retry
                    errorData.streamInfo = {
                        // Prevent jellyfin-web retrying with transcode
                        // which crashes the player
                        mediaSource: {
                            SupportsTranscoding: false
                        }
                    };
                }

                this.events.trigger(this, 'error', [errorData]);
            };

            this.onDuration = (duration) => {
                this._duration = duration;
            };
        }

        currentSrc() {
            return this._currentSrc;
        }

        async play(options) {
            this._started = false;
            this._timeUpdated = false;
            this._currentTime = null;
            this._bufferedRanges = [];

            this.resetSubtitleOffset();
            if (options.fullscreen) {
                this.loading.show();
            }
            const elem = await this.createMediaElement(options);
            return await this.setCurrentSrc(elem, options);
        }

        getSavedVolume() {
            return this.appSettings.get('volume') || 1;
        }


        tryGetFramerate(options) {
            if (options.mediaSource && options.mediaSource.MediaStreams) {
                for (let stream of options.mediaSource.MediaStreams) {
                    if (stream.Type == "Video") {
                        return stream.RealFrameRate || stream.AverageFrameRate || null;
                    }
                }
            }
        }

        /**
         * @private
         */
        getStreamByIndex(mediaStreams, jellyIndex) {
            for (const stream of mediaStreams) {
                if (stream.Index == jellyIndex) {
                    return stream;
                }
            }
            return null;
        }

        /**
         * @private
         */
        getRelativeIndexByType(mediaStreams, jellyIndex, streamType) {
            let relIndex = 1;
            for (const source of mediaStreams) {
                if (source.Type != streamType || source.IsExternal) {
                    continue;
                }
                if (source.Index == jellyIndex) {
                    return relIndex;
                }
                relIndex += 1;
            }
            return null;
        }

        /**
         * @private
         */
        setCurrentSrc(elem, options) {
            return new Promise((resolve) => {
                const val = options.url;
                this._currentSrc = val;
                console.debug(`playing url: ${val}`);

                // Convert to seconds
                const ms = (options.playerStartPositionTicks || 0) / 10000;
                this._currentPlayOptions = options;
                this._subtitleTrackIndexToSetOnPlaying = options.mediaSource.DefaultSubtitleStreamIndex == null ? -1 : options.mediaSource.DefaultSubtitleStreamIndex;
                this._audioTrackIndexToSetOnPlaying = options.mediaSource.DefaultAudioStreamIndex;

                console.log('[MPV] Audio track index:', this._audioTrackIndexToSetOnPlaying);
                console.log('[MPV] Subtitle track index:', this._subtitleTrackIndexToSetOnPlaying);

                const streamdata = {type: 'video', headers: {'User-Agent': jmpInfo.userAgent}, metadata: options.item, media: {}};
                const fps = this.tryGetFramerate(options);
                if (fps) {
                    streamdata.frameRate = fps;
                }

                const player = window.api.player;

                const streams = options.mediaSource?.MediaStreams || [];

                // Handle audio
                const audioRelIndex = this._audioTrackIndexToSetOnPlaying != null && this._audioTrackIndexToSetOnPlaying >= 0
                    ? this.getRelativeIndexByType(streams, this._audioTrackIndexToSetOnPlaying, 'Audio')
                    : 1;

                // Handle subtitle - check for external first
                let subtitleParam;
                if (this._subtitleTrackIndexToSetOnPlaying >= 0) {
                    const subStream = this.getStreamByIndex(streams, this._subtitleTrackIndexToSetOnPlaying);
                    if (subStream && subStream.DeliveryMethod === 'External' && subStream.DeliveryUrl) {
                        subtitleParam = '#,' + subStream.DeliveryUrl;
                        console.log('[MPV] External subtitle URL:', subStream.DeliveryUrl);
                    } else {
                        const relIndex = this.getRelativeIndexByType(streams, this._subtitleTrackIndexToSetOnPlaying, 'Subtitle');
                        subtitleParam = relIndex != null ? relIndex : -1;
                        console.log('[MPV] Mapped subtitle index:', this._subtitleTrackIndexToSetOnPlaying, '->', subtitleParam);
                    }
                } else {
                    subtitleParam = -1;
                }

                console.log('[MPV] Mapped audio index:', this._audioTrackIndexToSetOnPlaying, '->', audioRelIndex);

                player.load(val,
                    { startMilliseconds: ms, autoplay: true },
                    streamdata,
                    audioRelIndex,
                    subtitleParam,
                    resolve);
            });
        }

        setSubtitleStreamIndex(index) {
            console.log('[MPV] setSubtitleStreamIndex called with index:', index);
            this._subtitleTrackIndexToSetOnPlaying = index;

            if (index < 0) {
                window.api.player.setSubtitleStream(-1);
                return;
            }

            const streams = this._currentPlayOptions?.mediaSource?.MediaStreams || [];
            const stream = this.getStreamByIndex(streams, index);

            // Handle external subtitle URL
            if (stream && stream.DeliveryMethod === 'External' && stream.DeliveryUrl) {
                console.log('[MPV] Loading external subtitle:', stream.DeliveryUrl);
                window.api.player.setSubtitleStream('#,' + stream.DeliveryUrl);
                return;
            }

            // Handle embedded subtitle via relative index
            const relIndex = this.getRelativeIndexByType(streams, index, 'Subtitle');
            console.log('[MPV] Mapped subtitle index:', index, '->', relIndex);
            window.api.player.setSubtitleStream(relIndex != null ? relIndex : -1);
        }

        setSecondarySubtitleStreamIndex(index) {
            // MPV doesn't support secondary subtitles - no-op for compatibility
            console.log('[MPV] setSecondarySubtitleStreamIndex not supported, ignoring index:', index);
        }

        resetSubtitleOffset() {
            this._currentTrackOffset = 0;
            this._showTrackOffset = false;
            window.api.player.setSubtitleDelay(0);
        }

        enableShowingSubtitleOffset() {
            this._showTrackOffset = true;
        }

        disableShowingSubtitleOffset() {
            this._showTrackOffset = false;
        }

        isShowingSubtitleOffsetEnabled() {
            return this._showTrackOffset;
        }

        setSubtitleOffset(offset) {
            const offsetValue = parseFloat(offset);
            this._currentTrackOffset = offsetValue;
            window.api.player.setSubtitleDelay(Math.round(offsetValue * 1000));
        }

        getSubtitleOffset() {
            return this._currentTrackOffset;
        }

        /**
         * @private
         */
        isAudioStreamSupported() {
            return true;
        }

        /**
         * @private
         */
        getSupportedAudioStreams() {
            const profile = this._lastProfile;

            return getMediaStreamAudioTracks(this._currentPlayOptions.mediaSource).filter((stream) => {
                return this.isAudioStreamSupported(stream, profile);
            });
        }

        setAudioStreamIndex(index) {
            console.log('[MPV] setAudioStreamIndex called with index:', index);
            this._audioTrackIndexToSetOnPlaying = index;

            const streams = this._currentPlayOptions?.mediaSource?.MediaStreams || [];
            const relIndex = index < 0 ? -1 : this.getRelativeIndexByType(streams, index, 'Audio');
            console.log('[MPV] Mapped audio index:', index, '->', relIndex);
            window.api.player.setAudioStream(relIndex != null ? relIndex : -1);
        }

        onEndedInternal() {
            const stopInfo = {
                src: this._currentSrc
            };

            this.events.trigger(this, 'stopped', [stopInfo]);

            this._currentTime = null;
            this._currentSrc = null;
            this._currentPlayOptions = null;
        }

        stop(destroyPlayer) {
            window.api.player.stop();

            this.onEndedInternal();

            if (destroyPlayer) {
                this.destroy();
            }
            return Promise.resolve();
        }

        removeMediaDialog() {
            window.api.player.stop();

            window.api.player.setVideoRectangle(-1, 0, 0, 0);

            document.body.classList.remove('hide-scroll');

            const dlg = this._videoDialog;
            if (dlg) {
                this.setTransparency(0); // TRANSPARENCY_LEVEL.None
                this._videoDialog = null;
                dlg.parentNode.removeChild(dlg);
            }

            // Only supporting QtWebEngine here
            if (document.webkitIsFullScreen && document.webkitExitFullscreen) {
                document.webkitExitFullscreen();
            }
        }

        destroy() {
            this.removeMediaDialog();

            const player = window.api.player;
            this._hasConnection = false;
            this._bufferedRanges = [];
            player.playing.disconnect(this.onPlaying);
            player.positionUpdate.disconnect(this.onTimeUpdate);
            player.finished.disconnect(this.onEnded);
            this._duration = undefined;
            player.updateDuration.disconnect(this.onDuration);
            player.error.disconnect(this.onError);
            player.paused.disconnect(this.onPause);
            player.bufferedRangesUpdated.disconnect(this.onBufferedRangesUpdated);
        }

        /**
         * @private
         */
        createMediaElement(options) {
            const dlg = document.querySelector('.videoPlayerContainer');

            if (!dlg) {
                const dlg = document.createElement('div');

                dlg.classList.add('videoPlayerContainer');
                dlg.style.position = 'fixed';
                dlg.style.top = 0;
                dlg.style.bottom = 0;
                dlg.style.left = 0;
                dlg.style.right = 0;
                dlg.style.display = 'flex';
                dlg.style.alignItems = 'center';

                if (options.fullscreen) {
                    dlg.style.zIndex = 1000;
                }

                const html = '';

                dlg.innerHTML = html;

                // Set backdrop if available
                if (options.backdropUrl) {
                    dlg.style.backgroundImage = `url('${options.backdropUrl}')`;
                    dlg.style.backgroundSize = 'cover';
                    dlg.style.backgroundPosition = 'center';
                }

                document.body.insertBefore(dlg, document.body.firstChild);
                this.setTransparency(2); // TRANSPARENCY_LEVEL.Full
                this._videoDialog = dlg;
                const player = window.api.player;
                if (!this._hasConnection) {
                    this._hasConnection = true;
                    player.playing.connect(this.onPlaying);
                    player.positionUpdate.connect(this.onTimeUpdate);
                    player.finished.connect(this.onEnded);
                    player.updateDuration.connect(this.onDuration);
                    player.error.connect(this.onError);
                    player.paused.connect(this.onPause);
                    player.bufferedRangesUpdated.connect(this.onBufferedRangesUpdated);

                    // Log all other signals
                    player.buffering.connect((percent) => {
                        console.log(`[MPV Signal] buffering: ${percent}`);
                    });
                    player.canceled.connect(() => console.log('[MPV Signal] canceled'));
                    player.stopped.connect(() => console.log('[MPV Signal] stopped'));
                    player.stateChanged.connect((newState, oldState) => console.log(`[MPV Signal] stateChanged: ${oldState} -> ${newState}`));
                    player.videoPlaybackActive.connect((active) => console.log(`[MPV Signal] videoPlaybackActive: ${active}`));
                    player.windowVisible.connect((visible) => console.log(`[MPV Signal] windowVisible: ${visible}`));
                    player.onVideoRecangleChanged.connect(() => console.log('[MPV Signal] onVideoRecangleChanged'));
                    player.onMetaData.connect((meta) => console.log(`[MPV Signal] onMetaData: ${JSON.stringify(meta)}`));
                }

                if (options.fullscreen) {
                    // At this point, we must hide the scrollbar placeholder, so it's not being displayed while the item is being loaded
                    document.body.classList.add('hide-scroll');
                }
                return Promise.resolve();
            } else {
                // we need to hide scrollbar when starting playback from page with animated background
                if (options.fullscreen) {
                    document.body.classList.add('hide-scroll');
                }

                return Promise.resolve();
            }
        }

    /**
     * @private
     */
    canPlayMediaType(mediaType) {
        return (mediaType || '').toLowerCase() === 'video';
    }

    canPlayItem(item, playOptions) {
        // Delegate to canPlayMediaType - MPV can play any video the media type check passes
        return this.canPlayMediaType(item.MediaType);
    }

    /**
     * @private
     */
    supportsPlayMethod() {
        return true;
    }

    /**
     * @private
     */
    getDeviceProfile(item, options) {
        if (this.appHost.getDeviceProfile) {
            return this.appHost.getDeviceProfile(item, options);
        }

        return Promise.resolve({});
    }

    /**
     * @private
     */
    static getSupportedFeatures() {
        return ['PlaybackRate', 'SetAspectRatio'];
    }

    supports(feature) {
        if (!this._supportedFeatures) {
            this._supportedFeatures = mpvVideoPlayer.getSupportedFeatures();
        }

        return this._supportedFeatures.includes(feature);
    }

    isFullscreen() {
        // Check native window fullscreen state
        if (window.jmpInfo && window.jmpInfo.settings && window.jmpInfo.settings.main) {
            return window.jmpInfo.settings.main.fullscreen === true;
        }
        return false;
    }

    toggleFullscreen() {
        if (window.api && window.api.input) {
            window.api.input.executeActions(['host:fullscreen']);
        }
    }

    // Save this for when playback stops, because querying the time at that point might return 0
    currentTime(val) {
        if (val != null) {
            window.api.player.seekTo(val);
            return;
        }

        return this._currentTime;
    }

    currentTimeAsync() {
        return new Promise((resolve) => {
            window.api.player.getPosition(resolve);
        });
    }

    duration() {
        if (this._duration) {
            return this._duration;
        }

        return null;
    }

    canSetAudioStreamIndex() {
        return true;
    }

    static onPictureInPictureError(err) {
        console.error(`Picture in picture error: ${err}`);
    }

    setPictureInPictureEnabled() {}

    isPictureInPictureEnabled() {
        return false;
    }

    isAirPlayEnabled() {
        return false;
    }

    setAirPlayEnabled() {}

    setBrightness() {}

    getBrightness() {
        return 100;
    }

    seekable() {
        return Boolean(this._duration);
    }

    pause() {
        window.api.player.pause();
    }

    // This is a retry after error
    resume() {
        this._paused = false;
        window.api.player.play();
    }

    unpause() {
        window.api.player.play();
    }

    paused() {
        return this._paused;
    }

    setPlaybackRate(value) {
        let playSpeed = +value; //this comes as a string from player force int for now
        this._playRate = playSpeed;
        window.api.player.setPlaybackRate(playSpeed * 1000);

        if (window.api && window.api.player) {
            window.api.player.notifyRateChange(playSpeed);
        }
    }

    getPlaybackRate() {
        if(!this._playRate) //On startup grab default
        {
            let playRate = window.jmpInfo.settings.video.default_playback_speed;

            if(!playRate) //fallback if default missing
                playRate = 1;

            this._playRate = playRate;
        }
        return this._playRate;
    }

    getSupportedPlaybackRates() {
        return [{
            name: '0.5x',
            id: 0.5
        }, {
            name: '0.75x',
            id: 0.75
        }, {
            name: '1x',
            id: 1.0
        }, {
            name: '1.25x',
            id: 1.25
        }, {
            name: '1.5x',
            id: 1.5
        }, {
            name: '1.75x',
            id: 1.75
        }, {
            name: '2x',
            id: 2.0
        }, {
            name: '2.5x',
            id: 2.5
        }, {
            name: '3x',
            id: 3.0
        }, {
            name: '3.5x',
            id: 3.5
        }, {
            name: '4.0x',
            id: 4.0
        }];
    }

    saveVolume(value) {
        if (value) {
            this.appSettings.set('volume', value);
        }
    }

    setVolume(val, save = true) {
        val = Number(val);
        if (!isNaN(val)) {
            this._volume = val;
            if (save) {
                this.saveVolume(val / 100);
                this.events.trigger(this, 'volumechange');
            }
            window.api.player.setVolume(val);
        }
    }

    getVolume() {
        return this._volume;
    }

    volumeUp() {
        this.setVolume(Math.min(this.getVolume() + 2, 100));
    }

    volumeDown() {
        this.setVolume(Math.max(this.getVolume() - 2, 0));
    }

    setMute(mute, triggerEvent = true) {
        this._muted = mute;
        window.api.player.setMuted(mute);
        if (triggerEvent) {
            this.events.trigger(this, 'volumechange');
        }
    }

    isMuted() {
        return this._muted;
    }

    togglePictureInPicture() {
    }

    toggleAirPlay() {
    }

    getBufferedRanges() {
        return this._bufferedRanges;
    }

    getStats() {
        const playOptions = this._currentPlayOptions || [];
        const categories = [];

        if (!this._currentPlayOptions) {
            return Promise.resolve({
                categories: categories
            });
        }

        const mediaCategory = {
            stats: [],
            type: 'media'
        };
        categories.push(mediaCategory);

        if (playOptions.url) {
            //  create an anchor element (note: no need to append this element to the document)
            let link = document.createElement('a');
            //  set href to any path
            link.setAttribute('href', playOptions.url);
            const protocol = (link.protocol || '').replace(':', '');

            if (protocol) {
                mediaCategory.stats.push({
                    label: this.globalize.translate('LabelProtocol'),
                    value: protocol
                });
            }

            link = null;
        }

        mediaCategory.stats.push({
            label: this.globalize.translate('LabelStreamType'),
            value: 'Video'
        });

        const videoCategory = {
            stats: [],
            type: 'video'
        };
        categories.push(videoCategory);

        const audioCategory = {
            stats: [],
            type: 'audio'
        };
        categories.push(audioCategory);

        return Promise.resolve({
            categories: categories
        });
    }

    getSupportedAspectRatios() {
        const options = window.jmpInfo.settingsDescriptions.video.find(x => x.key == 'aspect').options;
        const current = window.jmpInfo.settings.video.aspect;

        const getOptionName = (option) => {
            const canTranslate = {
                'normal': 'Auto',
                'zoom': 'AspectRatioCover',
                'stretch': 'AspectRatioFill',
            }
            const name = option.replace('video.aspect.', '');
            return canTranslate[name]
                ? this.globalize.translate(canTranslate[name])
                : name;
        }

        return options.map(x => ({
            id: x.value,
            name: getOptionName(x.title),
            selected: x.value == current
        }));
    }

    getAspectRatio() {
        return window.jmpInfo.settings.video.aspect;
    }

    setAspectRatio(value) {
        window.jmpInfo.settings.video.aspect = value;
    }
}
/* eslint-enable indent */

window._mpvVideoPlayer = mpvVideoPlayer;
})();
