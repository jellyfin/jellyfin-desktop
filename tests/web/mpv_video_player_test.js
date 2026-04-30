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

function setup({ aspectRatio } = {}) {
  const apiPlayer = makeApiPlayer();
  const jmp = makeJmpBus();
  const events = makeEvents();
  const appSettings = makeAppSettings({ aspectRatio });
  const win = makeWindow({ apiPlayer, jmp });
  loadScript("mpv-player-core.js", win);
  loadScript("mpv-video-player.js", win);
  const Video = win._mpvVideoPlayer;
  const video = new Video({
    events,
    loading: { show: () => {}, hide: () => {} },
    appRouter: { showVideoOsd: () => {} },
    globalize: { translate: (k) => k },
    appHost: {},
    appSettings,
    confirm: () => {},
    dashboard: undefined,
  });
  return { win, apiPlayer, jmp, events, appSettings, video, Video };
}

test("plugin metadata fields are set", () => {
  const { video } = setup();
  assert.equal(video.name, "MPV Video Player");
  assert.equal(video.type, "mediaplayer");
  assert.equal(video.id, "mpvvideoplayer");
  assert.equal(video.priority, -1);
  assert.equal(video.isLocalPlayer, true);
});

test("constructor sets window._mpvVideoPlayerInstance to itself", () => {
  const { win, video } = setup();
  assert.equal(win._mpvVideoPlayerInstance, video);
});

test("canPlayMediaType only matches video", () => {
  const { video } = setup();
  assert.equal(video.canPlayMediaType("Video"), true);
  assert.equal(video.canPlayMediaType("audio"), false);
});

test("supports advertises PlaybackRate and SetAspectRatio", () => {
  const { video } = setup();
  assert.equal(video.supports("PlaybackRate"), true);
  assert.equal(video.supports("SetAspectRatio"), true);
  assert.equal(video.supports("AirPlay"), false);
});

test("getAspectRatio prefers appSettings.aspectRatio() function", () => {
  const { video } = setup({ aspectRatio: "cover" });
  assert.equal(video.getAspectRatio(), "cover");
});

test("getAspectRatio falls back to _currentAspectRatio (v10.10.7)", () => {
  const { video } = setup();
  assert.equal(video.getAspectRatio(), "auto");
  video._currentAspectRatio = "fill";
  assert.equal(video.getAspectRatio(), "fill");
});

test("setAspectRatio writes through appSettings.aspectRatio() and emits player.setAspectRatio", () => {
  const { jmp, appSettings, video } = setup({ aspectRatio: "auto" });
  video.setAspectRatio("cover");
  assert.equal(appSettings.aspectRatio(), "cover");
  assert.deepEqual(lastSend(jmp, "player.setAspectRatio"), { mode: "cover" });
});

test("setAspectRatio falls back to _currentAspectRatio in v10.10.7 mode", () => {
  const { jmp, video } = setup();
  video.setAspectRatio("fill");
  assert.equal(video._currentAspectRatio, "fill");
  assert.deepEqual(lastSend(jmp, "player.setAspectRatio"), { mode: "fill" });
});

test("setCurrentSrc emits player.setAspectRatio then player.load with full mediaSource", () => {
  const { apiPlayer, jmp, video } = setup({ aspectRatio: "auto" });
  const p = video.setCurrentSrc({
    url: "http://stream/x.mkv",
    playerStartPositionTicks: 0,
    item: { Id: "vid" },
    mediaSource: { MediaStreams: [], DefaultAudioStreamIndex: -1, DefaultSubtitleStreamIndex: -1 },
  });
  // Resolve immediately so the test isn't pending.
  apiPlayer.playing.emit();
  return p.then(() => {
    assert.deepEqual(lastSend(jmp, "player.setAspectRatio"), { mode: "auto" });
    const load = lastSend(jmp, "player.load");
    assert.equal(load.url, "http://stream/x.mkv");
    assert.equal(load.startMs, 0);
    assert.equal(load.defaultAudioIdx, -1);
    assert.equal(load.defaultSubIdx, -1);
    assert.ok(load.mediaSource);
  });
});

test("setCurrentSrc passes raw Jellyfin default audio index (no relative translation)", () => {
  const { apiPlayer, jmp, video } = setup({ aspectRatio: "auto" });
  const p = video.setCurrentSrc({
    url: "u",
    item: {},
    mediaSource: {
      MediaStreams: [],
      DefaultAudioStreamIndex: 3,
      DefaultSubtitleStreamIndex: -1,
    },
  });
  apiPlayer.playing.emit();
  return p.then(() => {
    assert.equal(lastSend(jmp, "player.load").defaultAudioIdx, 3);
  });
});

test("setSubtitleStreamIndex(null) sends jellyfinIndex: null", () => {
  const { jmp, video } = setup();
  video.setSubtitleStreamIndex(null);
  assert.deepEqual(lastSend(jmp, "player.selectSubtitle"), { jellyfinIndex: null });
});

test("setSubtitleStreamIndex(-1) sends jellyfinIndex: null", () => {
  const { jmp, video } = setup();
  video.setSubtitleStreamIndex(-1);
  assert.deepEqual(lastSend(jmp, "player.selectSubtitle"), { jellyfinIndex: null });
});

test("setSubtitleStreamIndex passes Jellyfin index unchanged (resolver in C++)", () => {
  const { jmp, video } = setup();
  video.setSubtitleStreamIndex(4);
  assert.deepEqual(lastSend(jmp, "player.selectSubtitle"), { jellyfinIndex: 4 });
});

test("resetSubtitleOffset emits player.setSubtitleOffset(0)", () => {
  const { jmp, video } = setup();
  video.resetSubtitleOffset();
  assert.deepEqual(lastSend(jmp, "player.setSubtitleOffset"), { seconds: 0 });
});

test("setSubtitleOffset sends seconds (no scaling)", () => {
  const { jmp, video } = setup();
  video.setSubtitleOffset(1.234);
  assert.deepEqual(lastSend(jmp, "player.setSubtitleOffset"), { seconds: 1.234 });
});

test("setAudioStreamIndex(null) sends jellyfinIndex: null", () => {
  const { jmp, video } = setup();
  video.setAudioStreamIndex(null);
  assert.deepEqual(lastSend(jmp, "player.selectAudio"), { jellyfinIndex: null });
});

test("setAudioStreamIndex passes Jellyfin index unchanged", () => {
  const { jmp, video } = setup();
  video.setAudioStreamIndex(2);
  assert.deepEqual(lastSend(jmp, "player.selectAudio"), { jellyfinIndex: 2 });
});

test("onPlaying handler triggers player.setVideoRectangle", () => {
  const { jmp, video } = setup();
  video._videoDialog = null;
  video._currentPlayOptions = { fullscreen: false };
  video._core.handlers.onPlaying();
  assert.deepEqual(lastSend(jmp, "player.setVideoRectangle"), { x: 0, y: 0, w: 0, h: 0 });
});

test("setPlaybackRate emits player.setRate and input.rateChanged", () => {
  const { jmp, video } = setup();
  video.setPlaybackRate(2);
  assert.deepEqual(lastSend(jmp, "player.setRate"), { rate: 2 });
  assert.deepEqual(lastSend(jmp, "input.rateChanged"), { rate: 2 });
});

test("toggleFullscreen sends fullscreen.toggle", () => {
  const { jmp, video } = setup();
  video.toggleFullscreen();
  assert.equal(sendsOf(jmp, "fullscreen.toggle").length, 1);
});

test("getStats returns categories array", async () => {
  const { video } = setup();
  const stats = await video.getStats();
  assert.equal(Array.isArray(stats.categories), true);
});

test("static getSupportedFeatures includes PlaybackRate and SetAspectRatio", () => {
  const { Video } = setup();
  const features = Video.getSupportedFeatures();
  assert.ok(features.includes("PlaybackRate"));
  assert.ok(features.includes("SetAspectRatio"));
});
