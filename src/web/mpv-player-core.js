(function() {
    class MpvPlayerCore {
        constructor(events, appSettings = null) {
            this.events = events;
            this.appSettings = appSettings;
            this._duration = undefined;
            this._currentTime = null;
            this._paused = false;
            this._volume = this.getSavedVolume() * 100;
            this._playRate = 1;
            this._muted = false;
            this._timeUpdateTimer = null;
            this._lastTimerTick = null;
            this._hasConnection = false;
            this._seeking = false;

            this.handlers = {
                onPlaying: null,
                onTimeUpdate: null,
                onSeeking: null,
                onEnded: null,
                onPause: null,
                onDuration: null,
                onError: null
            };
        }

        // Timer management
        startTimeUpdateTimer() {
            if (this._timeUpdateTimer) return;
            this._lastTimerTick = Date.now();
            this._timeUpdateTimer = setInterval(() => {
                if (this._paused || this._seeking || this._currentTime === null) return;
                const now = Date.now();
                const elapsed = now - this._lastTimerTick;
                this._lastTimerTick = now;
                const rate = this._playRate || 1.0;
                this._currentTime += elapsed * rate;
                this.events.trigger(this.player, 'timeupdate');
            }, 250);
        }

        stopTimeUpdateTimer() {
            if (this._timeUpdateTimer) {
                clearInterval(this._timeUpdateTimer);
                this._timeUpdateTimer = null;
            }
        }

        // Signal management
        connectSignals() {
            if (this._hasConnection) return;
            this._hasConnection = true;
            const p = window.api.player;
            p.playing.connect(this.handlers.onPlaying);
            p.positionUpdate.connect(this.handlers.onTimeUpdate);
            p.seeking.connect(this.handlers.onSeeking);
            p.finished.connect(this.handlers.onEnded);
            p.updateDuration.connect(this.handlers.onDuration);
            p.error.connect(this.handlers.onError);
            p.paused.connect(this.handlers.onPause);
        }

        disconnectSignals() {
            if (!this._hasConnection) return;
            this._hasConnection = false;
            const p = window.api.player;
            p.playing.disconnect(this.handlers.onPlaying);
            p.positionUpdate.disconnect(this.handlers.onTimeUpdate);
            p.seeking.disconnect(this.handlers.onSeeking);
            p.finished.disconnect(this.handlers.onEnded);
            p.updateDuration.disconnect(this.handlers.onDuration);
            p.error.disconnect(this.handlers.onError);
            p.paused.disconnect(this.handlers.onPause);
        }

        // Default event handlers (can be overridden via handlers object)
        defaultOnPause() {
            this._paused = true;
            this.stopTimeUpdateTimer();
            this.events.trigger(this.player, 'pause');
        }

        defaultOnDuration(duration) {
            this._duration = duration;
        }

        // Playback control
        pause() { window.api.player.pause(); }
        resume() { this._paused = false; window.api.player.play(); }
        unpause() { window.api.player.play(); }
        paused() { return this._paused; }

        // Time
        currentTime(val) {
            if (val != null) {
                this._currentTime = val;
                this._lastTimerTick = Date.now();
                window.api.player.seekTo(val);
                return;
            }
            return this._currentTime;
        }

        currentTimeAsync() {
            return new Promise(resolve => window.api.player.getPosition(resolve));
        }

        duration() { return this._duration || null; }
        seekable() { return Boolean(this._duration); }
        getBufferedRanges() { return window._bufferedRanges || []; }

        // Playback rate
        setPlaybackRate(value) {
            this._playRate = value;
            window.api.player.setPlaybackRate(value * 1000);
        }

        getPlaybackRate() { return this._playRate || 1; }

        getSupportedPlaybackRates() {
            return [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 3.5, 4.0].map(id => ({ name: id + 'x', id }));
        }

        // Volume (0-100 at the mpv/runtime layer; appSettings stores 0-1 to match jellyfin-web)
        getSavedVolume() {
            const raw = this.appSettings ? Number(this.appSettings.get('volume')) : NaN;
            return isNaN(raw) || raw <= 0 ? 1 : raw;
        }

        saveVolume(valFractional) {
            // Skip persisting 0: getSavedVolume falls back to 1 for 0/negative/NaN,
            // so writing 0 would be lost on next load. Use the mute state for silence.
            if (valFractional > 0 && this.appSettings) {
                this.appSettings.set('volume', valFractional);
            }
        }

        setVolume(val, save = true) {
            val = Number(val);
            if (isNaN(val)) return;
            this._volume = val;
            if (save) {
                this.saveVolume(val / 100);
                this.events.trigger(this.player, 'volumechange');
            }
            window.api.player.setVolume(val);
        }

        // Push tracked volume to mpv without save/event. mpv defaults to 100 on each
        // load, so call this on first playback to restore the saved level.
        pushVolume() { window.api.player.setVolume(this._volume); }

        getVolume() { return this._volume; }
        volumeUp() { this.setVolume(Math.min(this._volume + 2, 100)); }
        volumeDown() { this.setVolume(Math.max(this._volume - 2, 0)); }

        setMute(mute, triggerEvent = true) {
            this._muted = mute;
            window.api.player.setMuted(mute);
            if (triggerEvent) this.events.trigger(this.player, 'volumechange');
        }

        isMuted() { return this._muted; }
    }

    // mpv track selection (1-based track indices)
    MpvPlayerCore.TRACK_AUTO    = -1;  // mpv auto-selects
    MpvPlayerCore.TRACK_DISABLE =  0;  // disable track (sid=0, aid=0)

    window.MpvPlayerCore = MpvPlayerCore;
})();
