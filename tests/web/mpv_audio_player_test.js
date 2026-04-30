import { test } from "node:test";
import { strict as assert } from "node:assert";
import { loadScript } from "./helpers/load.js";
import {
  makeWindow,
  makeApiPlayer,
  makeJmpBus,
  makeEvents,
  makeAppSettings,
  lastSend,
  sendsOf,
} from "./helpers/mocks.js";

function setup() {
  const apiPlayer = makeApiPlayer();
  const jmp = makeJmpBus();
  const events = makeEvents();
  const appSettings = makeAppSettings();
  const win = makeWindow({ apiPlayer, jmp });
  loadScript("mpv-player-core.js", win);
  loadScript("mpv-audio-player.js", win);
  const Audio = win._mpvAudioPlayer;
  const audio = new Audio({
    events,
    appHost: { getDeviceProfile: () => Promise.resolve({ profileTag: "x" }) },
    appSettings,
  });
  return { win, apiPlayer, jmp, events, appSettings, audio };
}

test("plugin metadata fields are set", () => {
  const { audio } = setup();
  assert.equal(audio.name, "MPV Audio Player");
  assert.equal(audio.type, "mediaplayer");
  assert.equal(audio.id, "mpvaudioplayer");
  assert.equal(audio.syncPlayWrapAs, "htmlaudioplayer");
  assert.equal(audio.useServerPlaybackInfoForAudio, true);
});

test("constructor wires MpvPlayerCore from window", () => {
  const { win, audio } = setup();
  assert.ok(audio._core instanceof win.MpvPlayerCore);
  assert.equal(audio._core.player, audio);
});

test("canPlayMediaType is case-insensitive and only matches audio", () => {
  const { audio } = setup();
  assert.equal(audio.canPlayMediaType("Audio"), true);
  assert.equal(audio.canPlayMediaType("audio"), true);
  assert.equal(audio.canPlayMediaType("video"), false);
  assert.equal(audio.canPlayMediaType(undefined), false);
});

test("supports advertises only PlaybackRate", () => {
  const { audio } = setup();
  assert.equal(audio.supports("PlaybackRate"), true);
  assert.equal(audio.supports("SetAspectRatio"), false);
});

test("getDeviceProfile delegates to appHost when present", async () => {
  const { audio } = setup();
  const result = await audio.getDeviceProfile({}, {});
  assert.equal(result.profileTag, "x");
});

test("getDeviceProfile returns {} when appHost has no getDeviceProfile", async () => {
  const apiPlayer = makeApiPlayer();
  const jmp = makeJmpBus();
  const events = makeEvents();
  const appSettings = makeAppSettings();
  const win = makeWindow({ apiPlayer, jmp });
  loadScript("mpv-player-core.js", win);
  loadScript("mpv-audio-player.js", win);
  const audio = new win._mpvAudioPlayer({ events, appHost: {}, appSettings });
  const result = await audio.getDeviceProfile({}, {});
  assert.equal(typeof result, "object");
});

test("setCurrentSrc emits player.load with null defaults (resolver auto-selects)", async () => {
  const { apiPlayer, jmp, audio } = setup();

  const p = audio.setCurrentSrc({
    url: "http://stream/song.mp3",
    playerStartPositionTicks: 50_000_000, // 5000ms
    item: { Id: "abc" },
  });
  apiPlayer.playing.emit();
  await p;

  const load = lastSend(jmp, "player.load");
  assert.equal(load.url, "http://stream/song.mp3");
  assert.equal(load.startMs, 5000);
  assert.equal(load.defaultAudioIdx, null);
  assert.equal(load.defaultSubIdx, null);
  assert.equal(load.item.Id, "abc");
  assert.equal(audio._currentSrc, "http://stream/song.mp3");
});

test("play() resets state and connects signals before delegating to setCurrentSrc", async () => {
  const { audio } = setup();
  audio._core._duration = 1234;
  // Don't fire `playing` — just probe state set up by play() itself.
  audio.play({ url: "u", playerStartPositionTicks: 0, item: {} });
  assert.equal(audio._started, false);
  assert.equal(audio._core._currentTime, 0);
  assert.equal(audio._core._duration, undefined);
  assert.equal(audio._core._hasConnection, true);
});

test("stop without destroyPlayer pauses, fires stopped, clears src", async () => {
  const { apiPlayer, jmp, events, audio } = setup();
  const p1 = audio.play({ url: "u", playerStartPositionTicks: 0, item: {} });
  apiPlayer.playing.emit();
  await p1;

  await audio.stop(false);

  assert.ok(sendsOf(jmp, "player.pause").length >= 1);
  assert.equal(events.triggers.filter(t => t.name === "stopped").length, 1);
  assert.equal(audio.currentSrc(), null);
});

test("destroy emits player.stop, disconnects signals, clears duration", async () => {
  const { apiPlayer, jmp, audio } = setup();
  const p = audio.play({ url: "u", playerStartPositionTicks: 0, item: {} });
  apiPlayer.playing.emit();
  await p;
  audio._core._duration = 1000;
  audio.destroy();
  assert.ok(sendsOf(jmp, "player.stop").length >= 1);
  assert.equal(audio._core._hasConnection, false);
  assert.equal(audio._core._duration, undefined);
});

test("onPlaying handler fires 'playing' event and starts time update timer", () => {
  const { events, audio } = setup();
  audio._core.handlers.onPlaying();
  assert.equal(events.triggers.filter(t => t.name === "playing").length, 1);
  assert.ok(audio._core._timeUpdateTimer);
  audio._core.stopTimeUpdateTimer();
});

test("onError handler fires 'error' with mediadecodeerror payload", () => {
  const { events, audio } = setup();
  audio._core.handlers.onError(new Error("boom"));
  const errs = events.triggers.filter(t => t.name === "error");
  assert.equal(errs.length, 1);
  assert.equal(errs[0].args[0].type, "mediadecodeerror");
});

test("onPause handler fires 'pause' and stops timer", () => {
  const { events, audio } = setup();
  audio._core.startTimeUpdateTimer();
  audio._core.handlers.onPause();
  assert.equal(events.triggers.filter(t => t.name === "pause").length, 1);
  assert.equal(audio._core._timeUpdateTimer, null);
  assert.equal(audio._core._paused, true);
});
