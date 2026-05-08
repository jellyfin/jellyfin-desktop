(function() {
    class inputPlugin {
        constructor({ playbackManager, inputManager }) {
            this.name = 'Input Plugin';
            this.type = 'input';
            this.id = 'inputPlugin';
            this.playbackManager = playbackManager;
            this.inputManager = inputManager;
            this.positionInterval = null;
            this.artworkAbortController = null;
            this.pendingArtworkUrl = null;
            this.attachedPlayer = null;

            console.debug('[Media] inputPlugin constructed with playbackManager:', !!playbackManager);

            if (playbackManager && window.Events) {
                this.setupEvents(playbackManager);
            }
        }

        notifyMetadata(item) {
            if (!item || !window.jmpNative) return;
            const meta = {
                Name: item.Name || '',
                Type: item.Type || '',
                MediaType: item.MediaType || '',
                SeriesName: item.SeriesName || '',
                SeasonName: item.SeasonName || '',
                Album: item.Album || '',
                Artists: item.Artists || [],
                IndexNumber: item.IndexNumber || 0,
                RunTimeTicks: item.RunTimeTicks || 0,
                Id: item.Id || ''
            };
            console.debug('[Media] notifyMetadata:', meta.Name);
            window.jmpNative.notifyMetadata(JSON.stringify(meta));
            this.fetchAlbumArt(item);
        }

        getImageUrl(item, baseUrl) {
            const imageTags = item.ImageTags || {};
            const itemType = item.Type || '';
            const mediaType = item.MediaType || '';

            if (itemType === 'Episode') {
                if (item.SeriesId && item.SeriesPrimaryImageTag) {
                    return baseUrl + '/Items/' + item.SeriesId + '/Images/Primary?tag=' + item.SeriesPrimaryImageTag + '&maxWidth=512';
                }
                if (item.SeasonId && item.SeasonPrimaryImageTag) {
                    return baseUrl + '/Items/' + item.SeasonId + '/Images/Primary?tag=' + item.SeasonPrimaryImageTag + '&maxWidth=512';
                }
            }

            if (mediaType === 'Audio' || itemType === 'Audio') {
                if (item.AlbumId && item.AlbumPrimaryImageTag) {
                    return baseUrl + '/Items/' + item.AlbumId + '/Images/Primary?tag=' + item.AlbumPrimaryImageTag + '&maxWidth=512';
                }
            }

            if (imageTags.Primary && item.Id) {
                return baseUrl + '/Items/' + item.Id + '/Images/Primary?tag=' + imageTags.Primary + '&maxWidth=512';
            }
            if (item.BackdropImageTags && item.BackdropImageTags.length > 0 && item.Id) {
                return baseUrl + '/Items/' + item.Id + '/Images/Backdrop/0?tag=' + item.BackdropImageTags[0] + '&maxWidth=512';
            }

            return null;
        }

        fetchAlbumArt(item) {
            if (!item || !window.jmpNative) return;

            if (this.artworkAbortController) {
                this.artworkAbortController.abort();
                this.artworkAbortController = null;
            }

            let baseUrl = '';
            if (window.ApiClient && window.ApiClient.serverAddress) {
                baseUrl = window.ApiClient.serverAddress();
            }
            if (!baseUrl) return;

            const imageUrl = this.getImageUrl(item, baseUrl);
            if (!imageUrl) {
                console.debug('[Media] No album art URL found');
                return;
            }

            if (imageUrl === this.pendingArtworkUrl) {
                console.debug('[Media] Album art already pending for:', imageUrl);
                return;
            }

            this.pendingArtworkUrl = imageUrl;
            this.artworkAbortController = new AbortController();
            const signal = this.artworkAbortController.signal;

            console.debug('[Media] Fetching album art:', imageUrl);

            fetch(imageUrl, { signal })
                .then(response => {
                    if (!response.ok) throw new Error('Failed to fetch image');
                    return response.blob();
                })
                .then(blob => {
                    const reader = new FileReader();
                    reader.onloadend = () => {
                        if (signal.aborted) return;
                        const dataUri = reader.result;
                        console.debug('[Media] Album art fetched, sending data URI');
                        window.jmpNative.notifyArtwork(dataUri);
                        this.pendingArtworkUrl = null;
                    };
                    reader.readAsDataURL(blob);
                })
                .catch(err => {
                    if (err.name === 'AbortError') {
                        console.debug('[Media] Album art fetch aborted');
                    } else {
                        console.warn('[Media] Album art fetch failed:', err.message);
                    }
                    this.pendingArtworkUrl = null;
                });
        }

        startPositionUpdates() {
            const pm = this.playbackManager;
            const player = this.attachedPlayer;

            const initialPos = pm.currentTime ? pm.currentTime() : 0;
            if (typeof initialPos === 'number' && initialPos >= 0) {
                window.jmpNative.notifyPosition(Math.floor(initialPos));
            }

            this.positionTracking = {
                startTime: Date.now(),
                startPos: initialPos,
                rate: (player && player.getPlaybackRate) ? player.getPlaybackRate() : 1.0
            };
        }

        resetPositionTracking() {
            const pm = this.playbackManager;
            const player = this.attachedPlayer;
            const pos = pm.currentTime ? pm.currentTime() : 0;
            this.positionTracking = {
                startTime: Date.now(),
                startPos: pos,
                rate: (player && player.getPlaybackRate) ? player.getPlaybackRate() : 1.0
            };
        }

        checkPositionDrift() {
            if (!this.positionTracking || !this.playbackManager) return;
            const pm = this.playbackManager;
            const actual = pm.currentTime ? pm.currentTime() : 0;
            if (typeof actual !== 'number' || actual < 0) return;

            const elapsed = Date.now() - this.positionTracking.startTime;
            const expected = this.positionTracking.startPos + (elapsed * this.positionTracking.rate);
            const drift = actual - expected;

            if (Math.abs(drift) > 2000) {
                console.debug('[Media] Position drift detected: expected=' + Math.floor(expected) + ' actual=' + Math.floor(actual) + ' drift=' + Math.floor(drift));
                if (drift > 0) {
                    window.jmpNative.notifySeek(Math.floor(actual));
                } else {
                    window.jmpNative.notifyRateChange(0.0);
                    window.jmpNative.notifyPosition(Math.floor(actual));
                }
                this.resetPositionTracking();
            }
        }

        stopPositionUpdates() {
            this.positionTracking = null;
        }

        updateQueueState() {
            try {
                if (!window.jmpNative) return;

                const pm = this.playbackManager;
                if (!pm) return;

                const qm = pm._playQueueManager;
                const playlist = qm?.getPlaylist();
                const currentIndex = qm?.getCurrentPlaylistIndex();

                if (!playlist || !Array.isArray(playlist) || playlist.length === 0 ||
                    currentIndex === undefined || currentIndex === null || currentIndex < 0) {
                    console.warn('[Media] updateQueueState: queue invalid (idx=' + currentIndex + ' len=' + (playlist?.length || 0) + '), keeping last state');
                    return;
                }

                const canNext = currentIndex < playlist.length - 1;

                const state = pm.getPlayerState ? pm.getPlayerState() : null;
                const isMusic = state?.NowPlayingItem?.MediaType === 'Audio';
                const canPrev = isMusic ? true : (currentIndex > 0);

                console.debug('[Media] updateQueueState: idx=' + currentIndex + ' len=' + playlist.length + ' canNext=' + canNext + ' canPrev=' + canPrev);
                window.jmpNative.notifyQueueChange(canNext, canPrev);
            } catch (e) {
                console.error('[Media] updateQueueState error:', e);
            }
        }

        setupEvents(pm) {
            console.debug('[Media] Setting up playbackManager events');
            const self = this;

            window.Events.on(pm, 'playbackstart', (e, player) => {
                console.debug('[Media] playbackstart event, player:', !!player);

                const state = pm.getPlayerState ? pm.getPlayerState() : null;

                if (state && state.NowPlayingItem) {
                    self.notifyMetadata(state.NowPlayingItem);
                }

                console.debug('[Media] Sending Playing state from playbackstart');
                if (window.jmpNative) window.jmpNative.notifyPlaybackState('Playing');
                self.startPositionUpdates();
                self.updateQueueState();

                if (player && player !== self.attachedPlayer) {
                    if (self.attachedPlayer) {
                        window.Events.off(self.attachedPlayer, 'playing');
                        window.Events.off(self.attachedPlayer, 'pause');
                        window.Events.off(self.attachedPlayer, 'ratechange');
                        window.Events.off(self.attachedPlayer, 'timeupdate');
                    }
                    self.attachedPlayer = player;

                    window.Events.on(player, 'playing', () => {
                        console.debug('[Media] player.playing event');
                        if (window.jmpNative) window.jmpNative.notifyPlaybackState('Playing');
                        self.updateQueueState();

                        const pos = pm.currentTime ? pm.currentTime() : 0;
                        if (pos !== undefined && pos !== null) {
                            window.jmpNative.notifyPosition(Math.floor(pos));
                        }
                        self.resetPositionTracking();

                        const rate = player.getPlaybackRate ? player.getPlaybackRate() : 1.0;
                        window.jmpNative.notifyRateChange(rate);
                    });

                    window.Events.on(player, 'pause', () => {
                        console.debug('[Media] player.pause event');
                        if (window.jmpNative) {
                            window.jmpNative.notifyPlaybackState('Paused');
                            const pos = pm.currentTime ? pm.currentTime() : 0;
                            if (typeof pos === 'number' && pos >= 0) {
                                window.jmpNative.notifyPosition(Math.floor(pos));
                            }
                        }
                    });

                    window.Events.on(player, 'ratechange', () => {
                        const rate = player.getPlaybackRate ? player.getPlaybackRate() : 1.0;
                        console.debug('[Media] player.ratechange event, rate:', rate);
                        if (window.jmpNative) {
                            window.jmpNative.notifyRateChange(rate);
                            const pos = pm.currentTime ? pm.currentTime() : 0;
                            if (typeof pos === 'number' && pos >= 0) {
                                window.jmpNative.notifyPosition(Math.floor(pos));
                            }
                        }
                        self.resetPositionTracking();
                    });

                    window.Events.on(player, 'timeupdate', () => {
                        self.checkPositionDrift();
                    });
                }
            });

            window.Events.on(pm, 'playbackstop', (e, stopInfo) => {
                try {
                    console.debug('[Media] playbackstop event, stopInfo:', JSON.stringify(stopInfo));
                } catch (err) {
                    console.debug('[Media] playbackstop event, stopInfo: [unserializable]');
                }
                self.stopPositionUpdates();

                const isNavigating = !!(stopInfo && stopInfo.nextMediaType);
                if (!isNavigating) {
                    console.debug('[Media] Playback truly stopped, clearing state');
                    if (window.jmpNative) window.jmpNative.notifyPlaybackState('Stopped');
                } else {
                    console.debug('[Media] Navigating to next item, keeping metadata');
                }
                self.updateQueueState();
            });

            window.Events.on(pm, 'playlistitemremove', () => self.updateQueueState());
            window.Events.on(pm, 'playlistitemadd', () => self.updateQueueState());
            window.Events.on(pm, 'playlistitemchange', () => self.updateQueueState());

            const remap = {
                'play_pause': 'playpause',
                'play': 'play',
                'pause': 'pause',
                'stop': 'stop',
                'next': 'next',
                'previous': 'previous',
                'seek_forward': 'fastforward',
                'seek_backward': 'rewind'
            };

            window.api.input.hostInput.connect((actions) => {
                console.debug('[Media] hostInput received:', actions);
                actions.forEach(action => {
                    const mappedAction = remap[action] || action;
                    console.debug('[Media] Sending to inputManager:', mappedAction);
                    if (self.inputManager && typeof self.inputManager.handleCommand === 'function') {
                        self.inputManager.handleCommand(mappedAction, {});
                    } else {
                        console.warn('[Media] inputManager.handleCommand not available, inputManager:', !!self.inputManager);
                    }
                });
            });

            window.api.input.positionSeek.connect((positionMs) => {
                console.debug('[Media] positionSeek received:', positionMs);
                const currentPlayer = pm.getCurrentPlayer ? pm.getCurrentPlayer() : pm._currentPlayer;
                if (currentPlayer) {
                    const duration = pm.duration ? pm.duration() : 0;
                    if (duration > 0) {
                        const percent = (positionMs * 10000) / duration * 100;
                        console.debug('[Media] Seeking to', percent.toFixed(2), '% (', positionMs, 'ms of', duration, 'ticks)');
                        pm.seekPercent(percent, currentPlayer);
                    }
                }
            });

            window.api.input.rateChanged.connect((rate) => {
                console.debug('[Media] rateChanged received:', rate);
                const currentPlayer = pm.getCurrentPlayer ? pm.getCurrentPlayer() : pm._currentPlayer;
                if (currentPlayer && typeof currentPlayer.setPlaybackRate === 'function') {
                    currentPlayer.setPlaybackRate(rate);
                }
            });

            console.debug('[Media] Events setup complete');
        }

        destroy() {
            this.stopPositionUpdates();
            if (this.artworkAbortController) {
                this.artworkAbortController.abort();
                this.artworkAbortController = null;
            }
            if (this.attachedPlayer && window.Events) {
                window.Events.off(this.attachedPlayer, 'playing');
                window.Events.off(this.attachedPlayer, 'pause');
                window.Events.off(this.attachedPlayer, 'ratechange');
                window.Events.off(this.attachedPlayer, 'timeupdate');
                this.attachedPlayer = null;
            }
            if (this.playbackManager && window.Events) {
                window.Events.off(this.playbackManager, 'playbackstart');
                window.Events.off(this.playbackManager, 'playbackstop');
            }
        }
    }

    window._inputPlugin = inputPlugin;
    console.debug('[Media] inputPlugin class installed');
})();
