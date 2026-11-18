const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');

function createPlayer(fullscreen = false, forceAlwaysFS = false) {
    let dialog;
    let htmlFullscreen = false;
    const restores = [];
    const signal = { connect() {}, disconnect() {} };
    const settings = { fullscreen, forceAlwaysFS };
    const context = {
        window: {
            jmpInfo: { settings: { main: settings } },
            api: {
                player: new Proxy({ stop() {}, setVideoRectangle() {} }, {
                    get: (target, key) => target[key] || signal
                }),
                window: { setFullScreen(value) { restores.push(value); settings.fullscreen = value; } },
                input: { executeActions() { settings.fullscreen = !settings.fullscreen; } }
            }
        },
        document: {
            get webkitIsFullScreen() { return htmlFullscreen; },
            webkitExitFullscreen() { htmlFullscreen = false; },
            querySelector() { return dialog; },
            createElement() { return { classList: { add() {} }, style: {} }; },
            body: {
                classList: { add() {}, remove() {} },
                insertBefore(element) {
                    dialog = element;
                    element.parentNode = { removeChild() { dialog = undefined; } };
                }
            }
        }
    };
    vm.createContext(context);
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../native/mpvVideoPlayer.js'), 'utf8'), context);
    const listeners = new WeakMap();
    const events = {
        on(target, name, callback) {
            let handlers = listeners.get(target);
            if (!handlers) {
                handlers = {};
                listeners.set(target, handlers);
            }
            (handlers[name] ||= []).push(callback);
        },
        trigger(target, name, args = []) {
            for (const callback of listeners.get(target)?.[name] || []) {
                callback({}, ...args);
            }
        },
        off(target, name, callback) {
            const handlers = listeners.get(target);
            if (handlers) {
                handlers[name] = callback ? (handlers[name] || []).filter(handler => handler !== callback) : [];
            }
        }
    };
    const newPlayer = () => new context.window._mpvVideoPlayer({
        events,
        dashboard: { default: { setBackdropTransparency() {} } }
    });
    const player = newPlayer();
    // jellyfin-web emits playbackstop with NextMediaType before final destruction.
    const playbackStopped = (target = player, nextMediaType = null) => {
        events.trigger(target, 'playbackstop', [{ NextMediaType: nextMediaType }]);
    };
    const finish = (target = player) => {
        events.on(target, 'stopped', () => {
            playbackStopped(target);
            target.destroy();
        });
        target.onEnded();
    };
    return { player, settings, restores, finish, playbackStopped, newPlayer,
        detachPlayerListeners() { events.off(player, 'playbackstop'); },
        removeDialogExternally() { dialog = undefined; },
        setHtmlFullscreen(value) { htmlFullscreen = value; },
        isHtmlFullscreen: () => htmlFullscreen };
}

test('natural end exits native fullscreen when HTML fullscreen is inactive', async () => {
    const { player, restores, finish } = createPlayer();
    await player.createMediaElement({ fullscreen: true });
    player.toggleFullscreen();
    finish();
    assert.equal(player.isFullscreen(), false);
    assert.deepEqual(restores, [false]);
});

test('fullscreen selected before playback is preserved', async () => {
    const { player, restores, finish } = createPlayer(true);
    await player.createMediaElement({ fullscreen: true });
    finish();
    assert.equal(player.isFullscreen(), true);
    assert.deepEqual(restores, []);
});

test('queued videos preserve fullscreen until the final video ends', async () => {
    const { player, restores, finish, playbackStopped } = createPlayer();
    await player.createMediaElement({ fullscreen: true });
    player.toggleFullscreen();
    player.onEnded();
    playbackStopped(player, 'Video');
    await player.createMediaElement({ fullscreen: true });
    assert.equal(player.isFullscreen(), true);
    assert.deepEqual(restores, []);
    finish();
    assert.equal(player.isFullscreen(), false);
});

test('recreating the dialog between episodes preserves the original window state', async () => {
    const { player, restores, finish, playbackStopped, removeDialogExternally } = createPlayer();
    await player.createMediaElement({ fullscreen: true });
    player.toggleFullscreen();
    playbackStopped(player, 'Video');
    removeDialogExternally();
    await player.createMediaElement({ fullscreen: true });
    assert.equal(player.isFullscreen(), true);
    assert.deepEqual(restores, []);
    finish();
    assert.equal(player.isFullscreen(), false);
    assert.deepEqual(restores, [false]);
});

test('replacing the player between episodes preserves fullscreen until the queue ends', async () => {
    const { player, restores, finish, playbackStopped, newPlayer } = createPlayer();
    await player.createMediaElement({ fullscreen: true });
    player.toggleFullscreen();
    playbackStopped(player, 'Video');
    player.destroy();
    const nextPlayer = newPlayer();
    await nextPlayer.createMediaElement({ fullscreen: true });
    assert.equal(nextPlayer.isFullscreen(), true);
    assert.deepEqual(restores, []);
    finish(nextPlayer);
    assert.equal(nextPlayer.isFullscreen(), false);
    assert.deepEqual(restores, [false]);
});

test('switching from video to audio ends the video fullscreen session', async () => {
    const { player, restores, playbackStopped } = createPlayer();
    await player.createMediaElement({ fullscreen: true });
    player.toggleFullscreen();
    playbackStopped(player, 'Audio');
    assert.equal(player.isFullscreen(), false);
    assert.deepEqual(restores, [false]);
});

test('manual stop restores the native window', async () => {
    const { player, restores, playbackStopped } = createPlayer();
    await player.createMediaElement({ fullscreen: true });
    player.toggleFullscreen();
    playbackStopped();
    await player.stop(true);
    assert.deepEqual(restores, [false]);
});

test('playback in a window does not change the window state', async () => {
    const { player, restores, finish } = createPlayer();
    await player.createMediaElement({ fullscreen: true });
    finish();
    assert.deepEqual(restores, []);
});

test('leaving fullscreen manually does not request another window restore', async () => {
    const { player, restores, finish } = createPlayer();
    await player.createMediaElement({ fullscreen: true });
    player.toggleFullscreen();
    player.toggleFullscreen();
    finish();
    assert.deepEqual(restores, []);
});

test('kiosk mode never requests a fullscreen exit', async () => {
    const { player, restores, finish } = createPlayer(false, true);
    await player.createMediaElement({ fullscreen: true });
    player.toggleFullscreen();
    finish();
    assert.equal(player.isFullscreen(), true);
    assert.deepEqual(restores, []);
});

test('HTML fullscreen cleanup still works and repeated cleanup is harmless', async () => {
    const harness = createPlayer();
    await harness.player.createMediaElement({ fullscreen: true });
    harness.player.toggleFullscreen();
    harness.setHtmlFullscreen(true);
    harness.finish();
    harness.player.removeMediaDialog();
    assert.equal(harness.isHtmlFullscreen(), false);
    assert.deepEqual(harness.restores, [false]);
});

test('a new session captures the window state again', async () => {
    const { player, restores, finish, settings } = createPlayer();
    await player.createMediaElement({ fullscreen: true });
    player.toggleFullscreen();
    finish();
    settings.fullscreen = true;
    await player.createMediaElement({ fullscreen: true });
    finish();
    assert.equal(player.isFullscreen(), true);
    assert.deepEqual(restores, [false]);
});

test('reusing a detached video player reconnects the fullscreen restoration handler', async () => {
    const { player, restores, finish, detachPlayerListeners } = createPlayer();
    await player.createMediaElement({ fullscreen: true });
    finish();
    detachPlayerListeners();
    await player.createMediaElement({ fullscreen: true });
    player.toggleFullscreen();
    finish();
    assert.equal(player.isFullscreen(), false);
    assert.deepEqual(restores, [false]);
});
