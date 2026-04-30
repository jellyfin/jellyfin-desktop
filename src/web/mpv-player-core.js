(function() {
    class MpvPlayerCore {
        constructor(events, appSettings) {
            this.events = events;
            this.appSettings = appSettings;
            this._duration = undefined;
            this._currentTime = null;
            this._paused = false;
            this._volume = 100;
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

            this.setVolume(this.getSavedVolume() * 100, false);
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

        // Signal management — wires player.* notifications into the local
        // handlers object. api.player is the in-process signal bus that
        // native-shim hooks up to bus subscriptions.
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

        defaultOnPause() {
            this._paused = true;
            this.stopTimeUpdateTimer();
            this.events.trigger(this.player, 'pause');
        }

        defaultOnDuration(duration) {
            this._duration = duration;
        }

        // Playback control — direct bus dispatch
        pause()   { window.jmp.send('player.pause'); }
        resume()  { this._paused = false; window.jmp.send('player.play'); }
        unpause() { window.jmp.send('player.play'); }
        paused()  { return this._paused; }

        currentTime(val) {
            if (val != null) {
                this._currentTime = val;
                this._lastTimerTick = Date.now();
                window.jmp.send('player.seek', { positionMs: val });
                return;
            }
            return this._currentTime;
        }

        currentTimeAsync() {
            return new Promise(resolve => window.api.player.getPositionAsync(resolve));
        }

        duration() { return this._duration || null; }
        seekable() { return Boolean(this._duration); }
        getBufferedRanges() { return window.api.player.getBufferedRanges(); }

        // Playback rate — plain double; mpv side no longer expects scaling.
        setPlaybackRate(value) {
            this._playRate = value;
            window.jmp.send('player.setRate', { rate: value });
        }

        getPlaybackRate() { return this._playRate || 1; }

        getSupportedPlaybackRates() {
            return [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 3.5, 4.0]
                .map(id => ({ name: id + 'x', id }));
        }

        saveVolume(value) {
            if (value) this.appSettings.set('volume', value);
        }

        getSavedVolume() {
            return this.appSettings.get('volume') || 1;
        }

        setVolume(val, save = true) {
            val = Number(val);
            if (!isNaN(val)) {
                this._volume = val;
                if (save) {
                    this.appSettings.set('volume', (val || 100) / 100);
                    this.events.trigger(this.player, 'volumechange');
                }
                window.jmp.send('player.setVolume', { volume: val });
            }
        }

        getVolume() { return this._volume; }
        volumeUp()   { this.setVolume(Math.min(this._volume + 2, 100)); }
        volumeDown() { this.setVolume(Math.max(this._volume - 2, 0)); }

        setMute(mute, triggerEvent = true) {
            this._muted = mute;
            window.jmp.send('player.setMuted', { muted: !!mute });
            if (triggerEvent) this.events.trigger(this.player, 'volumechange');
        }

        isMuted() { return this._muted; }
    }

    window.MpvPlayerCore = MpvPlayerCore;
})();
