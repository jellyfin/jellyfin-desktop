(function() {
let fadeTimeout;
function fade(instance, elem, startingVolume) {
    instance._isFadingOut = true;

    // Need to record the starting volume on each pass rather than querying elem.volume
    // This is due to iOS safari not allowing volume changes and always returning the system volume value
    const newVolume = Math.max(0, startingVolume - 15);
    console.debug('fading volume to ' + newVolume);
    api.player.setVolume(newVolume);

    if (newVolume <= 0) {
        instance._isFadingOut = false;
        return Promise.resolve();
    }

    return new Promise(function (resolve, reject) {
        cancelFadeTimeout();
        fadeTimeout = setTimeout(function () {
            fade(instance, null, newVolume).then(resolve, reject);
        }, 100);
    });
}

function cancelFadeTimeout() {
    const timeout = fadeTimeout;
    if (timeout) {
        clearTimeout(timeout);
        fadeTimeout = null;
    }
}

class mpvAudioPlayer {
    constructor({ events, appHost, appSettings, toast }) {
        const self = this;

        self.events = events;
        self.appHost = appHost;
        self.appSettings = appSettings;

        self.name = 'MPV Audio Player';
        self.type = 'mediaplayer';
        self.id = 'mpvaudioplayer';
        self.syncPlayWrapAs = 'htmlaudioplayer';
        self.useServerPlaybackInfoForAudio = true;

        self._duration = undefined;
        self._currentTime = undefined;
        self._paused = false;
        self._volume = self.getSavedVolume() * 100;
        self._playRate = 1;
        self._hasConnection = false;
        self._bufferedRanges = [];

        self.play = (options) => {
            self._started = false;
            self._timeUpdated = false;
            self._currentTime = null;
            self._duration = undefined;
            self._bufferedRanges = [];

            const player = window.api.player;
            if (!self._hasConnection) {
                self._hasConnection = true;
                player.playing.connect(onPlaying);
                player.positionUpdate.connect(onTimeUpdate);
                player.finished.connect(onEnded);
                player.updateDuration.connect(onDuration);
                player.error.connect(onError);
                player.paused.connect(onPause);
                player.bufferedRangesUpdated.connect(onBufferedRangesUpdated);
            }

            return setCurrentSrc(options);
        };

        function setCurrentSrc(options) {
            return new Promise((resolve) => {
                const val = options.url;
                self._currentSrc = val;
                console.debug('playing url: ' + val);

                // Convert to seconds
                const ms = (options.playerStartPositionTicks || 0) / 10000;
                self._currentPlayOptions = options;

                window.api.player.load(val,
                    { startMilliseconds: ms, autoplay: true },
                    {type: 'music', headers: {'User-Agent': jmpInfo.userAgent}, metadata: options.item, media: {}},
                    '#1',
                    '',
                    resolve);
            });
        }

        self.onEndedInternal = () => {
            const stopInfo = {
                src: self._currentSrc
            };

            self.events.trigger(self, 'stopped', [stopInfo]);

            self._currentTime = null;
            self._currentSrc = null;
            self._currentPlayOptions = null;
        };

        self.stop = (destroyPlayer) => {
            cancelFadeTimeout();

            const src = self._currentSrc;

            if (src) {
                if (!destroyPlayer) {
                    self.pause();
                    self.onEndedInternal();
                    return Promise.resolve();
                }

                const originalVolume = self._volume;

                return fade(self, null, self._volume).then(function () {
                    self.pause();
                    self.setVolume(originalVolume, false);

                    self.onEndedInternal();
                    self.destroy();
                });
            }
            return Promise.resolve();
        };

        self.destroy = () => {
            window.api.player.stop();

            const player = window.api.player;
            self._hasConnection = false;
            player.playing.disconnect(onPlaying);
            player.positionUpdate.disconnect(onTimeUpdate);
            player.finished.disconnect(onEnded);
            self._duration = undefined;
            player.updateDuration.disconnect(onDuration);
            player.error.disconnect(onError);
            player.paused.disconnect(onPause);
            player.bufferedRangesUpdated.disconnect(onBufferedRangesUpdated);
            self._bufferedRanges = [];
        };

        function onDuration(duration) {
            self._duration = duration;
        }

        function onEnded() {
            self.onEndedInternal();
        }

        function onTimeUpdate(time) {
            // Don't trigger events after user stop
            if (!self._isFadingOut) {
                self._currentTime = time;
                self.events.trigger(self, 'timeupdate');
            }
        }

        function onPlaying() {
            if (!self._started) {
                self._started = true;

                const volume = self.getSavedVolume() * 100;
                self.setVolume(volume, volume != self._volume);
            }

            self.setPlaybackRate(self.getPlaybackRate());

            if (self._paused) {
                self._paused = false;
                self.events.trigger(self, 'unpause');
            }

            self.events.trigger(self, 'playing');
        }

        function onPause() {
            self._paused = true;
            self.events.trigger(self, 'pause');
        }

        function onBufferedRangesUpdated(ranges) {
            self._bufferedRanges = ranges;
        }

        function onError(error) {
            console.error(`media error: ${error}`);
            toast(`media error: ${error}`);

            self.events.trigger(self, 'error', [
                {
                    type: 'mediadecodeerror'
                }
            ]);
        }
    }

    getSavedVolume() {
        return this.appSettings.get('volume') || 1;
    }

    currentSrc() {
        return this._currentSrc;
    }

    canPlayMediaType(mediaType) {
        return (mediaType || '').toLowerCase() === 'audio';
    }

    getDeviceProfile(item, options) {
        if (this.appHost.getDeviceProfile) {
            return this.appHost.getDeviceProfile(item, options);
        }

        return {};
    }

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

    seekable() {
        return Boolean(this._duration);
    }

    getBufferedRanges() {
        return this._bufferedRanges;
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
        this._playRate = value;
        window.api.player.setPlaybackRate(value * 1000);

        if (window.api && window.api.player) {
            window.api.player.notifyRateChange(value);
        }
    }

    getPlaybackRate() {
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
        }];
    }

    saveVolume(value) {
        if (value) {
            this.appSettings.set('volume', value);
        }
    }

    setVolume(val, save = true) {
        this._volume = val;
        if (save) {
            this.saveVolume((val || 100) / 100);
            this.events.trigger(this, 'volumechange');
        }
        window.api.player.setVolume(val);
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

    supports(feature) {
        if (!supportedFeatures) {
            supportedFeatures = getSupportedFeatures();
        }

        return supportedFeatures.indexOf(feature) !== -1;
    }
}

let supportedFeatures;

function getSupportedFeatures() {
    return ['PlaybackRate'];
}

window._mpvAudioPlayer = mpvAudioPlayer;
})();
