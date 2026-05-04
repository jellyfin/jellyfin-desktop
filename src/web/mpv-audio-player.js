(function() {
    let fadeTimeout;

    function fade(instance, startingVolume) {
        instance._isFadingOut = true;
        const newVolume = Math.max(0, startingVolume - 15);
        instance.setVolume(newVolume, false);

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

    class mpvAudioPlayer extends window.MpvPlayerBase {
        constructor(args) {
            super(args);

            this.id = 'mpvaudioplayer';
            this.logTag = 'Audio';
            this.name = 'MPV Audio Player';
            this.syncPlayWrapAs = 'htmlaudioplayer';
            this.useServerPlaybackInfoForAudio = true;

            this._isFadingOut = false;

            this.handlers.onPlaying = () => {
                if (!this._started) {
                    this._started = true;
                    const volume = this.getSavedVolume() * 100;
                    this.setVolume(volume, volume !== this._volume);
                }
                this.setPlaybackRate(this.getPlaybackRate());
                this._emitPlaying();
            };

            this.handlers.onTimeUpdate = (time) => {
                if (!this._isFadingOut) {
                    this._seeking = false;
                    this._currentTime = time;
                    this.events.trigger(this, 'timeupdate');
                }
            };

        }

        play(options) {
            this._started = false;
            this._currentTime = null;
            this._duration = undefined;
            this.connectSignals();
            return this.setCurrentSrc(options);
        }

        get mediaType() { return 'music'; }

        stop(destroyPlayer) {
            cancelFadeTimeout();
            const src = this._currentSrc;

            if (src) {
                if (!destroyPlayer) {
                    this.pause();
                    this.onEndedInternal();
                    return Promise.resolve();
                }

                const originalVolume = this._volume;
                return fade(this, this._volume).then(() => {
                    this.pause();
                    this.setVolume(originalVolume, false);
                    this.onEndedInternal();
                    this.destroy();
                });
            }
            return Promise.resolve();
        }

        destroy() {
            window.api.player.stop();
            this.disconnectSignals();
            this._duration = undefined;
        }

        canPlayMediaType(mediaType) {
            return (mediaType || '').toLowerCase() === 'audio';
        }

        supports(feature) {
            return ['PlaybackRate'].includes(feature);
        }
    }

    window._mpvAudioPlayer = mpvAudioPlayer;
    console.log('[Media] mpvAudioPlayer plugin installed');
})();
