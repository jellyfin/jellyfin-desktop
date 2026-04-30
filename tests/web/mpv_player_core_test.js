import { test } from "node:test";
import { strict as assert } from "node:assert";
import { loadScript } from "./helpers/load.js";
import {
  makeWindow,
  makeJmpBus,
  makeApiPlayer,
  makeEvents,
  makeAppSettings,
  lastSend,
  sendsOf,
} from "./helpers/mocks.js";

function setup(opts = {}) {
  const jmp = makeJmpBus();
  const apiPlayer = makeApiPlayer();
  const events = makeEvents();
  const appSettings = makeAppSettings(opts.appSettings);
  const win = makeWindow({ jmp, apiPlayer });
  loadScript("mpv-player-core.js", win);
  const Core = win.MpvPlayerCore;
  const core = new Core(events, appSettings);
  core.player = { id: "test-player" };
  return { win, jmp, apiPlayer, events, appSettings, Core, core };
}

test("constructor uses default volume of 100 when nothing saved", () => {
  const { jmp, core } = setup();
  assert.equal(core.getVolume(), 100);
  assert.deepEqual(lastSend(jmp, "player.setVolume"), { volume: 100 });
});

test("constructor reads saved volume from appSettings", () => {
  const jmp = makeJmpBus();
  const apiPlayer = makeApiPlayer();
  const events = makeEvents();
  const appSettings = makeAppSettings();
  appSettings.set("volume", 0.4);
  const win = makeWindow({ jmp, apiPlayer });
  loadScript("mpv-player-core.js", win);
  const core = new win.MpvPlayerCore(events, appSettings);
  assert.equal(core.getVolume(), 40);
  assert.deepEqual(lastSend(jmp, "player.setVolume"), { volume: 40 });
});

test("constructor with save=false suppresses volumechange event", () => {
  const { events } = setup();
  assert.equal(events.triggers.filter(t => t.name === "volumechange").length, 0);
});

test("pause/play/unpause emit player.* messages", () => {
  const { jmp, core } = setup();
  core.pause();
  core._paused = true;
  core.resume();
  assert.equal(core._paused, false);
  core.unpause();
  assert.equal(sendsOf(jmp, "player.pause").length, 1);
  assert.equal(sendsOf(jmp, "player.play").length, 2);
});

test("setVolume(50) sends player.setVolume, persists, fires volumechange", () => {
  const { jmp, events, appSettings, core } = setup();
  jmp.send.calls.length = 0;
  core.setVolume(50);
  assert.deepEqual(lastSend(jmp, "player.setVolume"), { volume: 50 });
  assert.equal(appSettings.get("volume"), 0.5);
  assert.equal(events.triggers.filter(t => t.name === "volumechange").length, 1);
});

test("setVolume(val, false) skips persist and event", () => {
  const { jmp, events, appSettings, core } = setup();
  jmp.send.calls.length = 0;
  core.setVolume(75, false);
  assert.deepEqual(lastSend(jmp, "player.setVolume"), { volume: 75 });
  assert.equal(appSettings.get("volume"), undefined);
  assert.equal(events.triggers.filter(t => t.name === "volumechange").length, 0);
});

test("setVolume ignores NaN", () => {
  const { jmp, core } = setup();
  jmp.send.calls.length = 0;
  core.setVolume("not-a-number");
  assert.equal(sendsOf(jmp, "player.setVolume").length, 0);
});

test("volumeUp/volumeDown clamp to [0, 100]", () => {
  const { core } = setup();
  core.setVolume(99);
  core.volumeUp();
  assert.equal(core.getVolume(), 100);
  core.volumeUp();
  assert.equal(core.getVolume(), 100);

  core.setVolume(1);
  core.volumeDown();
  assert.equal(core.getVolume(), 0);
  core.volumeDown();
  assert.equal(core.getVolume(), 0);
});

test("setMute(true) sends player.setMuted and fires volumechange", () => {
  const { jmp, events, core } = setup();
  core.setMute(true);
  assert.deepEqual(lastSend(jmp, "player.setMuted"), { muted: true });
  assert.equal(core.isMuted(), true);
  assert.equal(events.triggers.filter(t => t.name === "volumechange").length, 1);
});

test("setMute(true, false) suppresses event but still sends", () => {
  const { jmp, events, core } = setup();
  core.setMute(true, false);
  assert.deepEqual(lastSend(jmp, "player.setMuted"), { muted: true });
  assert.equal(events.triggers.filter(t => t.name === "volumechange").length, 0);
});

test("setPlaybackRate sends rate as plain double (no scaling)", () => {
  const { jmp, core } = setup();
  core.setPlaybackRate(1.5);
  assert.deepEqual(lastSend(jmp, "player.setRate"), { rate: 1.5 });
  assert.equal(core.getPlaybackRate(), 1.5);
});

test("getPlaybackRate falls back to 1 when unset", () => {
  const { core } = setup();
  core._playRate = 0;
  assert.equal(core.getPlaybackRate(), 1);
});

test("currentTime(val) sends player.seek with positionMs", () => {
  const { jmp, core } = setup();
  core.currentTime(42);
  assert.deepEqual(lastSend(jmp, "player.seek"), { positionMs: 42 });
  assert.equal(core._currentTime, 42);
});

test("currentTime() returns last known position", () => {
  const { core } = setup();
  core._currentTime = 1234;
  assert.equal(core.currentTime(), 1234);
});

test("currentTimeAsync resolves via api.player.getPositionAsync", async () => {
  const { apiPlayer, core } = setup();
  // The mock's getPositionAsync stashes the cb; resolve it manually.
  apiPlayer.getPositionAsync = (cb) => cb(987);
  const got = await core.currentTimeAsync();
  assert.equal(got, 987);
});

test("getBufferedRanges proxies api.player.getBufferedRanges", () => {
  const { apiPlayer, core } = setup();
  apiPlayer._bufferedRanges = [{ start: 0, end: 10 }];
  const got = core.getBufferedRanges();
  assert.equal(got.length, 1);
  assert.equal(got[0].start, 0);
});

test("seekable reflects whether _duration is set", () => {
  const { core } = setup();
  assert.equal(core.seekable(), false);
  core._duration = 12345;
  assert.equal(core.seekable(), true);
});

test("duration returns _duration or null", () => {
  const { core } = setup();
  assert.equal(core.duration(), null);
  core._duration = 5000;
  assert.equal(core.duration(), 5000);
});

test("connectSignals wires handlers; signals deliver", () => {
  const { apiPlayer, core } = setup();
  let playingCalled = 0;
  core.handlers.onPlaying = () => { playingCalled++; };
  core.handlers.onTimeUpdate = () => {};
  core.handlers.onSeeking = () => {};
  core.handlers.onEnded = () => {};
  core.handlers.onPause = () => {};
  core.handlers.onDuration = () => {};
  core.handlers.onError = () => {};
  core.connectSignals();
  apiPlayer.playing.emit();
  apiPlayer.playing.emit();
  assert.equal(playingCalled, 2);

  core.disconnectSignals();
  apiPlayer.playing.emit();
  assert.equal(playingCalled, 2);
});

test("connectSignals is idempotent", () => {
  const { apiPlayer, core } = setup();
  for (const k of Object.keys(core.handlers)) core.handlers[k] = () => {};
  core.connectSignals();
  core.connectSignals();
  assert.equal(apiPlayer.playing.handlers.length, 1);
});

test("startTimeUpdateTimer is a no-op when already running; stop clears it", () => {
  const { core } = setup();
  core.startTimeUpdateTimer();
  const first = core._timeUpdateTimer;
  core.startTimeUpdateTimer();
  assert.equal(core._timeUpdateTimer, first);
  core.stopTimeUpdateTimer();
  assert.equal(core._timeUpdateTimer, null);
});

test("getSupportedPlaybackRates returns ascending name/id pairs", () => {
  const { core } = setup();
  const rates = core.getSupportedPlaybackRates();
  assert.equal(rates[0].id, 0.5);
  assert.equal(rates[0].name, "0.5x");
  assert.ok(rates.every(r => typeof r.id === "number" && typeof r.name === "string"));
});

test("saveVolume(0) does NOT persist (truthy guard)", () => {
  const { appSettings, core } = setup();
  appSettings._store.delete("volume");
  core.saveVolume(0);
  assert.equal(appSettings.get("volume"), undefined);
  core.saveVolume(0.7);
  assert.equal(appSettings.get("volume"), 0.7);
});
