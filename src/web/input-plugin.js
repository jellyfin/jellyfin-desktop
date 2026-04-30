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

            console.log('[Media] inputPlugin constructed with playbackManager:', !!playbackManager);

            if (playbackManager && window.Events) {
                this.setupEvents(playbackManager);
            }
        }

        notifyMetadata(item) {
            if (!item) return;
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
            window.jmp.send('playback.metadata', { json: JSON.stringify(meta) });
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
            if (!item) return;

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
                console.log('[Media] No album art URL found');
                return;
            }

            if (imageUrl === this.pendingArtworkUrl) {
                console.log('[Media] Album art already pending for:', imageUrl);
                return;
            }

            this.pendingArtworkUrl = imageUrl;
            this.artworkAbortController = new AbortController();
            const signal = this.artworkAbortController.signal;

            console.log('[Media] Fetching album art:', imageUrl);

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
                        window.jmp.send('playback.artwork', { uri: dataUri });
                        this.pendingArtworkUrl = null;
                    };
                    reader.readAsDataURL(blob);
                })
                .catch(err => {
                    if (err.name === 'AbortError') {
                        console.log('[Media] Album art fetch aborted');
                    } else {
                        console.log('[Media] Album art fetch failed:', err.message);
                    }
                    this.pendingArtworkUrl = null;
                });
        }

        startPositionUpdates() {
            const pm = this.playbackManager;
            const player = this.attachedPlayer;

            const initialPos = pm.currentTime ? pm.currentTime() : 0;

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
                if (drift > 0) {
                    window.jmp.send('playback.position', { positionMs: Math.floor(actual) });
                }
                this.resetPositionTracking();
            }
        }

        stopPositionUpdates() {
            this.positionTracking = null;
        }

        updateQueueState() {
            try {
                const pm = this.playbackManager;
                if (!pm) return;

                const qm = pm._playQueueManager;
                const playlist = qm?.getPlaylist();
                const currentIndex = qm?.getCurrentPlaylistIndex();

                if (!playlist || !Array.isArray(playlist) || playlist.length === 0 ||
                    currentIndex === undefined || currentIndex === null || currentIndex < 0) {
                    return;
                }

                const canNext = currentIndex < playlist.length - 1;

                const state = pm.getPlayerState ? pm.getPlayerState() : null;
                const isMusic = state?.NowPlayingItem?.MediaType === 'Audio';
                const canPrev = isMusic ? true : (currentIndex > 0);

                window.jmp.send('playback.queueChange', { canNext, canPrev });
            } catch (e) {
                console.error('[Media] updateQueueState error:', e);
            }
        }

        setupEvents(pm) {
            console.log('[Media] Setting up playbackManager events');
            const self = this;

            window.Events.on(pm, 'playbackstart', (e, player) => {
                console.log('[Media] playbackstart event, player:', !!player);

                const state = pm.getPlayerState ? pm.getPlayerState() : null;

                if (state && state.NowPlayingItem) {
                    self.notifyMetadata(state.NowPlayingItem);
                }

                window.jmp.send('playback.state', { state: 'Playing' });
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
                        window.jmp.send('playback.state', { state: 'Playing' });
                        self.updateQueueState();
                        self.resetPositionTracking();
                    });

                    window.Events.on(player, 'pause', () => {
                        window.jmp.send('playback.state', { state: 'Paused' });
                    });

                    window.Events.on(player, 'ratechange', () => {
                        self.resetPositionTracking();
                    });

                    window.Events.on(player, 'timeupdate', () => {
                        self.checkPositionDrift();
                    });
                }
            });

            window.Events.on(pm, 'playbackstop', (e, stopInfo) => {
                try {
                    console.log('[Media] playbackstop event, stopInfo:', JSON.stringify(stopInfo));
                } catch (err) {
                    console.log('[Media] playbackstop event, stopInfo: [unserializable]');
                }
                self.stopPositionUpdates();

                const isNavigating = !!(stopInfo && stopInfo.nextMediaType);
                if (!isNavigating) {
                    window.jmp.send('playback.state', { state: 'Stopped' });
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
                console.log('[Media] hostInput received:', actions);
                actions.forEach(action => {
                    const mappedAction = remap[action] || action;
                    console.log('[Media] Sending to inputManager:', mappedAction);
                    if (self.inputManager && typeof self.inputManager.handleCommand === 'function') {
                        self.inputManager.handleCommand(mappedAction, {});
                    } else {
                        console.log('[Media] inputManager.handleCommand not available, inputManager:', !!self.inputManager);
                    }
                });
            });

            window.api.input.positionSeek.connect((positionMs) => {
                console.log('[Media] positionSeek received:', positionMs);
                const currentPlayer = pm.getCurrentPlayer ? pm.getCurrentPlayer() : pm._currentPlayer;
                if (currentPlayer) {
                    const duration = pm.duration ? pm.duration() : 0;
                    if (duration > 0) {
                        const percent = (positionMs * 10000) / duration * 100;
                        console.log('[Media] Seeking to', percent.toFixed(2), '% (', positionMs, 'ms of', duration, 'ticks)');
                        pm.seekPercent(percent, currentPlayer);
                    }
                }
            });

            window.api.input.rateChanged.connect((rate) => {
                console.log('[Media] rateChanged received:', rate);
                const currentPlayer = pm.getCurrentPlayer ? pm.getCurrentPlayer() : pm._currentPlayer;
                if (currentPlayer && typeof currentPlayer.setPlaybackRate === 'function') {
                    currentPlayer.setPlaybackRate(rate);
                }
            });

            console.log('[Media] Events setup complete');
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
    console.log('[Media] inputPlugin class installed');
})();
