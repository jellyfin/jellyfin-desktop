(function() {
const remap = {
    "play_pause": "playpause",
    "seek_forward": "fastforward",
    "seek_backward": "rewind",
    "host:fullscreen": "togglefullscreen",
    "cycle_audio": "changeaudiotrack",
    "cycle_subtitles": "changesubtitletrack",
    "increase_volume": "volumeup",
    "decrease_volume": "volumedown",
    "step_backward": "previouschapter",
    "step_forward": "nextchapter",
    "enter": "select",
}

class inputPlugin {
    constructor({ inputManager, playbackManager }) {
        this.name = 'Input Plugin';
        this.type = 'input';
        this.id = 'inputPlugin';

        this.durationCheckInterval = null;
        this.positionUpdateInterval = null;
        this.attachedPlayer = null;

        (async () => {
            const api = await window.apiPromise;

            api.input.hostInput.connect((actions) => {
                actions.forEach(action => {
                    if (action === 'shuffle') {
                        playbackManager.setQueueShuffleMode('Shuffle');
                    } else if (action === 'sorted') {
                        playbackManager.setQueueShuffleMode('Sorted');
                    } else if (action === 'previous') {
                        const currentPlayer = playbackManager._currentPlayer;
                        if (currentPlayer && playbackManager.isPlayingAudio(currentPlayer)) {
                            const currentTime = playbackManager.currentTime(currentPlayer);
                            const currentIndex = playbackManager.getCurrentPlaylistIndex(currentPlayer);

                            if (currentTime >= 5 * 1000 || currentIndex <= 0) {
                                playbackManager.seekPercent(0, currentPlayer);
                            } else {
                                playbackManager.previousTrack(currentPlayer);
                            }
                        } else if (currentPlayer) {
                            playbackManager.previousTrack(currentPlayer);
                        }
                    } else {
                        if (remap.hasOwnProperty(action)) {
                            action = remap[action];
                        }
                        inputManager.handleCommand(action, {});
                    }
                });
            });

            const updateQueueState = function() {
                try {
                    if (!api || !api.player) {
                        return;
                    }

                    const playlist = playbackManager._playQueueManager?.getPlaylist();
                    if (!playlist || !Array.isArray(playlist)) {
                        return;
                    }

                    const currentIndex = playbackManager._playQueueManager?.getCurrentPlaylistIndex();
                    if (currentIndex === undefined || currentIndex === null || currentIndex < 0) {
                        return;
                    }

                    const canNext = currentIndex < playlist.length - 1;

                    const state = playbackManager.getPlayerState();
                    const isMusic = state?.NowPlayingItem?.MediaType === 'Audio';
                    const canPrevious = isMusic ? true : currentIndex > 0;

                    api.player.notifyQueueChange(canNext, canPrevious);
                } catch (e) {
                    console.error('PlayerMedia: Error in updateQueueState:', e);
                }
            };

            let globalListenersAttached = false;
            let lastReportedPosition = 0;

            if (!globalListenersAttached) {
                let lastFullscreenState = window.jmpInfo.settings.main.fullscreen;
                window.jmpInfo.settingsUpdate.push(function(section) {
                    if (section === 'main') {
                        const currentFullscreenState = window.jmpInfo.settings.main.fullscreen;
                        if (currentFullscreenState !== lastFullscreenState) {
                            lastFullscreenState = currentFullscreenState;
                            const currentPlayer = playbackManager._currentPlayer;
                            if (currentPlayer) {
                                window.Events.trigger(currentPlayer, 'fullscreenchange');
                            }
                        }
                    }
                });

                if (window.api && window.api.player) {
                    window.api.player.playbackRateChanged.connect(function(rate) {
                        const currentPlayer = playbackManager._currentPlayer;

                        if (currentPlayer && currentPlayer._playRate !== undefined) {
                            currentPlayer._playRate = rate;
                        }

                        if (window.Events && currentPlayer) {
                            window.Events.trigger(currentPlayer, 'ratechange');
                        }
                    });
                } else {
                    console.log('SystemMedia: Cannot attach playbackRateChanged listener - api:', !!window.api, 'player:', !!window.api?.player);
                }

                if (window.api && window.api.input) {
                    window.api.input.volumeChanged.connect(function(volume) {
                        const currentPlayer = playbackManager._currentPlayer;
                        if (currentPlayer && typeof currentPlayer.setVolume === 'function') {
                            currentPlayer.setVolume(volume);
                        }
                    });

                    window.api.input.rateChanged.connect(function(rate) {
                        const currentPlayer = playbackManager._currentPlayer;
                        if (currentPlayer && typeof currentPlayer.setPlaybackRate === 'function') {
                            currentPlayer.setPlaybackRate(rate);
                        }
                    });

                    // WORKAROUND: MPRIS spec has no "Buffering" playback state
                    // During seek buffering, we set Rate to 0.0 to stop MPRIS clients from
                    // auto-incrementing position. Once buffering completes, we restore Rate to 1.0.

                    window.api.input.positionSeek.connect(function(positionMs) {
                        const currentPlayer = playbackManager._currentPlayer;
                        if (currentPlayer) {
                            const duration = playbackManager.duration();
                            if (duration) {
                                const percent = (positionMs * 10000) / duration * 100;
                                playbackManager.seekPercent(percent, currentPlayer);
                            }
                        }

                        api.player.notifyPosition(Math.floor(positionMs));
                        lastReportedPosition = positionMs;

                        api.player.notifyRateChange(0.0);
                    });
                }

                window.Events.on(playbackManager, 'playlistitemremove', updateQueueState);
                window.Events.on(playbackManager, 'playlistitemadd', updateQueueState);
                window.Events.on(playbackManager, 'playlistitemchange', updateQueueState);

                window.Events.on(playbackManager, 'playbackstop', function(e, playbackStopInfo) {
                    updateQueueState();

                    const isNavigating = !!(playbackStopInfo && playbackStopInfo.nextMediaType);
                    api.player.notifyPlaybackStop(isNavigating);
                });

                globalListenersAttached = true;
            }

            window.Events.on(playbackManager, 'playbackstart', (e, player) => {
                if (!player) return;

                const state = playbackManager.getPlayerState();
                if (state && state.NowPlayingItem) {
                    let apiToken = '';
                    try {
                        // Attempt to get API token.
                        if (window.ApiClient) {
                            if (typeof window.ApiClient.accessToken === 'function') {
                                apiToken = window.ApiClient.accessToken() || '';
                            } else if (window.ApiClient.accessToken) {
                                apiToken = window.ApiClient.accessToken || '';
                            } else if (window.ApiClient._serverInfo?.AccessToken) {
                                apiToken = window.ApiClient._serverInfo.AccessToken;
                            }
                        }
                    } catch (e) {
                        console.error('inputPlugin: failed to get API token:', e);
                    }
                    
                    // Get server URL
                    let serverUrl = '';
                    try {
                        if (window.ApiClient && typeof window.ApiClient.serverAddress === 'function') {
                            serverUrl = window.ApiClient.serverAddress() || '';
                        }
                    } catch (e) {
                        console.error('inputPlugin: failed to get serverAddress:', e);
                    }
                    if (!serverUrl) {
                        // Possible backup solution.
                        serverUrl = window.location.origin;
                    }
                    const metadata = Object.assign({}, state.NowPlayingItem, {
                        _serverUrl: serverUrl,
                        _apiToken: apiToken
                    });
                    try {
                        if (typeof api.player.notifyServerUrl === 'function') {
                            api.player.notifyServerUrl(serverUrl);
                        }
                    } catch (e) {
                        console.error('inputPlugin: notifyServerUrl failed:', e);
                    }
                    api.player.notifyMetadata(metadata);

                    const initialPos = playbackManager.currentTime();
                    if (initialPos !== undefined && initialPos !== null) {
                        api.player.notifyPosition(Math.floor(initialPos));
                        lastReportedPosition = initialPos;
                    }

                    if (player && typeof player.getVolume === 'function') {
                        const volume = player.getVolume();
                        api.player.notifyVolumeChange(volume / 100.0);
                    }
                }

                let lastDuration = 0;
                const checkDuration = function() {
                    const duration = playbackManager.duration();
                    if (duration && duration !== lastDuration) {
                        lastDuration = duration;
                        const durationMs = Math.floor(duration / 10000);
                        api.player.notifyDurationChange(durationMs);
                    }
                };

                if (player !== this.attachedPlayer) {
                    if (this.attachedPlayer) {
                        window.Events.off(this.attachedPlayer, 'shufflequeuemodechange');
                        window.Events.off(this.attachedPlayer, 'repeatmodechange');
                        window.Events.off(this.attachedPlayer, 'playing');
                        window.Events.off(this.attachedPlayer, 'pause');
                        window.Events.off(this.attachedPlayer, 'playbackstop');
                        window.Events.off(this.attachedPlayer, 'volumechange');
                        window.Events.off(this.attachedPlayer, 'ratechange');
                        window.Events.off(this.attachedPlayer, 'timeupdate');
                    }

                    this.attachedPlayer = player;

                    window.Events.on(player, 'shufflequeuemodechange', () => {
                    const mode = playbackManager.getQueueShuffleMode();
                    const enabled = (mode === 'Shuffle');
                    api.player.notifyShuffleChange(enabled);
                });

                window.Events.on(player, 'repeatmodechange', () => {
                    const mode = playbackManager.getRepeatMode();
                    api.player.notifyRepeatChange(mode);
                });

                window.Events.on(player, 'playing', () => {
                    api.player.notifyPlaybackState('Playing');

                    updateQueueState();

                    api.player.notifyRateChange(1.0);

                    if (this.durationCheckInterval) {
                        clearInterval(this.durationCheckInterval);
                    }
                    checkDuration();
                    this.durationCheckInterval = setInterval(checkDuration, 1000);

                    const initialPos = playbackManager.currentTime();
                    if (initialPos !== undefined && initialPos !== null) {
                        api.player.notifyPosition(Math.floor(initialPos));
                    }

                    if (this.positionUpdateInterval) {
                        clearInterval(this.positionUpdateInterval);
                    }
                    this.positionUpdateInterval = setInterval(() => {
                        const positionMs = playbackManager.currentTime();
                        if (positionMs !== undefined && positionMs !== null) {
                            const positionDiff = Math.abs(positionMs - lastReportedPosition);
                            if (lastReportedPosition > 0 && positionDiff > 2000) {
                                api.player.notifyRateChange(0.0);
                                api.player.notifySeek(Math.floor(positionMs));
                            } else {
                                api.player.notifyPosition(Math.floor(positionMs));
                            }
                            lastReportedPosition = positionMs;
                        }
                    }, 500);
                });

                window.Events.on(player, 'pause', () => {
                    api.player.notifyPlaybackState('Paused');
                    api.player.notifyRateChange(0.0);

                    if (this.durationCheckInterval) {
                        clearInterval(this.durationCheckInterval);
                        this.durationCheckInterval = null;
                    }

                    const currentPos = playbackManager.currentTime();
                    if (currentPos !== undefined && currentPos !== null) {
                        lastReportedPosition = currentPos;
                    }
                });

                window.Events.on(player, 'playbackstop', () => {
                    if (this.durationCheckInterval) {
                        clearInterval(this.durationCheckInterval);
                        this.durationCheckInterval = null;
                    }

                    if (this.positionUpdateInterval) {
                        clearInterval(this.positionUpdateInterval);
                        this.positionUpdateInterval = null;
                    }

                    lastDuration = 0;
                    lastReportedPosition = 0;
                });

                    window.Events.on(player, 'volumechange', () => {
                        if (player && typeof player.getVolume === 'function') {
                            const volume = player.getVolume();
                            api.player.notifyVolumeChange(volume / 100.0);
                        }
                    });

                    window.Events.on(player, 'ratechange', () => {
                        if (player && typeof player.getPlaybackRate === 'function') {
                            const rate = player.getPlaybackRate();
                            api.player.notifyRateChange(rate);
                        }
                    });

                    window.Events.on(player, 'timeupdate', () => {
                        const positionMs = playbackManager.currentTime();
                        if (positionMs !== undefined && positionMs !== null) {
                            const positionDiff = Math.abs(positionMs - lastReportedPosition);
                            if (lastReportedPosition > 0 && positionDiff > 2000) {
                                // Player-initiated seek - set rate to 0 during buffering
                                api.player.notifyRateChange(0.0);
                                api.player.notifySeek(Math.floor(positionMs));
                            } else if (positionDiff > 100) {
                                api.player.notifyPosition(Math.floor(positionMs));
                            }
                            lastReportedPosition = positionMs;
                        }
                    });
                }
            });

            api.system.hello("jmpInputPlugin");
        })();
    }

    destroy() {
        if (this.durationCheckInterval) {
            clearInterval(this.durationCheckInterval);
            this.durationCheckInterval = null;
        }

        if (this.positionUpdateInterval) {
            clearInterval(this.positionUpdateInterval);
            this.positionUpdateInterval = null;
        }

        if (this.attachedPlayer) {
            window.Events.off(this.attachedPlayer, 'shufflequeuemodechange');
            window.Events.off(this.attachedPlayer, 'repeatmodechange');
            window.Events.off(this.attachedPlayer, 'playing');
            window.Events.off(this.attachedPlayer, 'pause');
            window.Events.off(this.attachedPlayer, 'playbackstop');
            window.Events.off(this.attachedPlayer, 'volumechange');
            window.Events.off(this.attachedPlayer, 'timeupdate');
            this.attachedPlayer = null;
        }

        if (window.playbackManager) {
            window.Events.off(window.playbackManager, 'playlistitemremove');
            window.Events.off(window.playbackManager, 'playlistitemadd');
            window.Events.off(window.playbackManager, 'playlistitemchange');
            window.Events.off(window.playbackManager, 'playbackstop');
        }

        console.log('SystemMedia: inputPlugin destroyed and cleaned up');
    }
}

window._inputPlugin = inputPlugin;
})();
