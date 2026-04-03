(function() {
    'use strict';

    // --- Playlist state ---
    const playlist = JSON.parse(decodeURIComponent(location.hash.slice(1)));
    let currentIndex = 0;
    let isPlaying = false;
    let isSeeking = false;
    let duration = 0;
    let position = 0;
    let volume = 100;
    let muted = false;
    let hideTimer = null;

    // --- DOM refs ---
    const idleScreen = document.getElementById('idle-screen');
    const osd = document.getElementById('osd');
    const titleEl = document.getElementById('osd-title');
    const currentTimeEl = document.getElementById('osd-current-time');
    const durationEl = document.getElementById('osd-duration');
    const seekSlider = document.getElementById('osd-seek');
    const volumeSlider = document.getElementById('osd-volume');
    const btnPlay = document.getElementById('btn-play');
    const btnPrev = document.getElementById('btn-prev');
    const btnNext = document.getElementById('btn-next');
    const btnRewind = document.getElementById('btn-rewind');
    const btnForward = document.getElementById('btn-forward');
    const btnMute = document.getElementById('btn-mute');
    const btnFullscreen = document.getElementById('btn-fullscreen');
    const iconPlay = document.getElementById('icon-play');
    const iconPause = document.getElementById('icon-pause');
    const iconVolOn = document.getElementById('icon-vol-on');
    const iconVolOff = document.getElementById('icon-vol-off');
    const iconFsEnter = document.getElementById('icon-fs-enter');
    const iconFsExit = document.getElementById('icon-fs-exit');

    // --- Helpers ---
    function formatTime(ms) {
        const totalSec = Math.floor(ms / 1000);
        const h = Math.floor(totalSec / 3600);
        const m = Math.floor((totalSec % 3600) / 60);
        const s = totalSec % 60;
        if (h > 0) return h + ':' + String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0');
        return m + ':' + String(s).padStart(2, '0');
    }

    function filenameFromPath(path) {
        // Extract filename from path or URL
        const parts = path.replace(/\\/g, '/').split('/');
        return decodeURIComponent(parts[parts.length - 1]) || path;
    }

    function updateSliderFill(slider, pct) {
        slider.style.setProperty('--progress', pct + '%');
    }

    // --- Playlist control ---
    function loadItem(index) {
        if (index < 0 || index >= playlist.length) return;
        currentIndex = index;
        duration = 0;
        position = 0;
        isPlaying = false;
        seekSlider.value = 0;
        updateSliderFill(seekSlider, 0);
        currentTimeEl.textContent = '0:00';
        durationEl.textContent = '0:00';
        titleEl.textContent = filenameFromPath(playlist[index]);
        window.api.player.load(playlist[index]);
    }

    function goIdle() {
        isPlaying = false;
        document.body.classList.remove('playing');
        hideOsd();
        updatePlayPauseIcon();
    }

    // --- OSD show/hide ---
    function showOsd() {
        if (!isPlaying) return;
        osd.classList.remove('osd-hidden');
        resetHideTimer();
    }

    function hideOsd() {
        osd.classList.add('osd-hidden');
        clearTimeout(hideTimer);
    }

    function resetHideTimer() {
        clearTimeout(hideTimer);
        hideTimer = setTimeout(hideOsd, 3000);
    }

    // --- Icon updates ---
    function updatePlayPauseIcon() {
        iconPlay.style.display = isPlaying ? 'none' : '';
        iconPause.style.display = isPlaying ? '' : 'none';
    }

    function updateMuteIcon() {
        iconVolOn.style.display = muted ? 'none' : '';
        iconVolOff.style.display = muted ? '' : 'none';
    }

    function updateFullscreenIcon() {
        const fs = !!document.fullscreenElement;
        iconFsEnter.style.display = fs ? 'none' : '';
        iconFsExit.style.display = fs ? '' : 'none';
    }

    // --- Signal handlers ---
    window.api.player.playing.connect(function() {
        isPlaying = true;
        document.body.classList.add('playing');
        updatePlayPauseIcon();
        showOsd();
    });

    window.api.player.paused.connect(function() {
        isPlaying = false;
        updatePlayPauseIcon();
        showOsd();
    });

    window.api.player.finished.connect(function() {
        if (currentIndex + 1 < playlist.length) {
            loadItem(currentIndex + 1);
        } else {
            goIdle();
        }
    });

    window.api.player.canceled.connect(function() {
        goIdle();
    });

    window.api.player.error.connect(function(msg) {
        console.error('[Player] Playback error:', msg);
        goIdle();
    });

    window.api.player.positionUpdate.connect(function(ms) {
        position = ms;
        if (!isSeeking) {
            currentTimeEl.textContent = formatTime(ms);
            if (duration > 0) {
                const pct = (ms / duration) * 100;
                seekSlider.value = pct;
                updateSliderFill(seekSlider, pct);
            }
        }
    });

    window.api.player.updateDuration.connect(function(ms) {
        duration = ms;
        durationEl.textContent = formatTime(ms);
    });

    // --- Button handlers ---
    btnPlay.addEventListener('click', function() {
        if (isPlaying) {
            window.api.player.pause();
        } else {
            window.api.player.play();
        }
    });

    btnPrev.addEventListener('click', function() {
        if (currentIndex > 0) {
            window.api.player.stop();
            loadItem(currentIndex - 1);
        }
    });

    btnNext.addEventListener('click', function() {
        if (currentIndex + 1 < playlist.length) {
            window.api.player.stop();
            loadItem(currentIndex + 1);
        }
    });

    btnRewind.addEventListener('click', function() {
        window.api.player.seekTo(Math.max(0, position - 30000));
    });

    btnForward.addEventListener('click', function() {
        window.api.player.seekTo(Math.min(duration, position + 30000));
    });

    btnMute.addEventListener('click', function() {
        muted = !muted;
        window.api.player.setMuted(muted);
        updateMuteIcon();
    });

    btnFullscreen.addEventListener('click', function() {
        if (document.fullscreenElement) {
            document.exitFullscreen();
        } else {
            document.documentElement.requestFullscreen();
        }
    });

    document.addEventListener('fullscreenchange', updateFullscreenIcon);

    // --- Seek slider ---
    seekSlider.addEventListener('input', function() {
        isSeeking = true;
        const pct = parseFloat(seekSlider.value);
        updateSliderFill(seekSlider, pct);
        currentTimeEl.textContent = formatTime((pct / 100) * duration);
    });

    seekSlider.addEventListener('change', function() {
        const pct = parseFloat(seekSlider.value);
        window.api.player.seekTo((pct / 100) * duration);
        isSeeking = false;
    });

    // --- Volume slider ---
    volumeSlider.addEventListener('input', function() {
        volume = parseInt(volumeSlider.value, 10);
        window.api.player.setVolume(volume);
        updateSliderFill(volumeSlider, volume);
        if (volume > 0 && muted) {
            muted = false;
            window.api.player.setMuted(false);
            updateMuteIcon();
        }
    });
    updateSliderFill(volumeSlider, volume);

    // --- Mouse activity -> show OSD ---
    document.addEventListener('mousemove', showOsd);
    document.addEventListener('click', function(e) {
        // Only show OSD if clicking outside the controls
        if (!osd.contains(e.target)) {
            showOsd();
        }
    });

    // --- Keyboard shortcuts ---
    document.addEventListener('keydown', function(e) {
        switch (e.key) {
            case ' ':
            case 'k':
                e.preventDefault();
                btnPlay.click();
                showOsd();
                break;
            case 'ArrowLeft':
                e.preventDefault();
                btnRewind.click();
                showOsd();
                break;
            case 'ArrowRight':
                e.preventDefault();
                btnForward.click();
                showOsd();
                break;
            case 'ArrowUp':
                e.preventDefault();
                volume = Math.min(100, volume + 5);
                volumeSlider.value = volume;
                window.api.player.setVolume(volume);
                updateSliderFill(volumeSlider, volume);
                showOsd();
                break;
            case 'ArrowDown':
                e.preventDefault();
                volume = Math.max(0, volume - 5);
                volumeSlider.value = volume;
                window.api.player.setVolume(volume);
                updateSliderFill(volumeSlider, volume);
                showOsd();
                break;
            case 'm':
                btnMute.click();
                showOsd();
                break;
            case 'f':
                btnFullscreen.click();
                break;
        }
    });

    // --- Init ---
    if (playlist.length <= 1) {
        document.body.classList.add('single-item');
    }

    // Start playback of first item
    loadItem(0);
})();
