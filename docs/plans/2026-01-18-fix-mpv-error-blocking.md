# Fix mpv Error Blocking Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix the app hang when mpv cannot play media content by implementing async command execution and proper error handling, following patterns from the old jellyfin-desktop Qt app.

**Architecture:** The issue is that mpv commands (`mpv_command()`, `mpv_set_property()`) are synchronous and can block for extended periods when media fails to load. The old Qt app solved this by: (1) using Qt's queued connections to keep mpv event processing off the main thread's blocking path, (2) setting `audio-fallback-to-null=yes` to prevent audio device errors from blocking, and (3) properly handling `MPV_END_FILE_REASON_ERROR` to surface errors to the UI. We will implement async mpv commands and add the missing error handling.

**Tech Stack:** C++, libmpv (mpv_command_async), CEF

---

### Task 1: Add audio-fallback-to-null Option

**Files:**
- Modify: `/home/ar/src/github/jellyfin-labs/jellyfin-desktop/src/player/mpv/mpv_player_vk.cpp:169-175`

**Step 1: Add the option in init()**

After `mpv_set_option_string(mpv_, "ytdl", "no");` (around line 174), add:

```cpp
// Discard audio output if no audio device could be opened
// Prevents blocking/crashes on audio errors (like jellyfin-desktop)
mpv_set_option_string(mpv_, "audio-fallback-to-null", "yes");
```

**Step 2: Verify build compiles**

Run: `cmake --build /home/ar/src/github/jellyfin-labs/jellyfin-desktop/build`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/player/mpv/mpv_player_vk.cpp
git commit -m "$(cat <<'EOF'
fix(mpv): add audio-fallback-to-null to prevent blocking on audio errors

This option makes mpv discard audio output instead of blocking when no
audio device can be opened, matching jellyfin-desktop behavior.
EOF
)"
```

---

### Task 2: Add Error Callback Infrastructure

**Files:**
- Modify: `/home/ar/src/github/jellyfin-labs/jellyfin-desktop/src/player/mpv/mpv_player_vk.h:26,76-82,102`

**Step 1: Add error callback typedef and member**

In the header, after the existing callback typedefs (around line 26), add:

```cpp
using ErrorCallback = std::function<void(const std::string& error)>;
```

Around line 82, after `setBufferedRangesCallback`, add:

```cpp
void setErrorCallback(ErrorCallback cb) { on_error_ = cb; }
```

Around line 106, after the other callback members, add:

```cpp
ErrorCallback on_error_;
```

**Step 2: Verify build compiles**

Run: `cmake --build /home/ar/src/github/jellyfin-labs/jellyfin-desktop/build`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/player/mpv/mpv_player_vk.h
git commit -m "$(cat <<'EOF'
feat(mpv): add error callback for playback failures
EOF
)"
```

---

### Task 3: Handle MPV_END_FILE_REASON_ERROR

**Files:**
- Modify: `/home/ar/src/github/jellyfin-labs/jellyfin-desktop/src/player/mpv/mpv_player_vk.cpp:128-139`

**Step 1: Add error handling in handleMpvEvent()**

Replace the existing `MPV_EVENT_END_FILE` case (lines 128-139) with:

```cpp
case MPV_EVENT_END_FILE: {
    mpv_event_end_file* ef = static_cast<mpv_event_end_file*>(event->data);
    std::cerr << "[MPV] END_FILE reason=" << ef->reason << " (0=EOF, 2=STOP, 4=ERROR)" << std::endl;
    // With keep-open=yes, EOF reason won't fire (handled by eof-reached property)
    // STOP reason fires on explicit stop command
    if (ef->reason == MPV_END_FILE_REASON_STOP) {
        playing_ = false;
        if (on_canceled_) on_canceled_();
    } else if (ef->reason == MPV_END_FILE_REASON_ERROR) {
        playing_ = false;
        std::string error = mpv_error_string(ef->error);
        std::cerr << "[MPV] Playback error: " << error << std::endl;
        if (on_error_) on_error_(error);
    }
    // Note: EOF/QUIT/REDIRECT reasons are handled by eof-reached property observation
    break;
}
```

**Step 2: Verify build compiles**

Run: `cmake --build /home/ar/src/github/jellyfin-labs/jellyfin-desktop/build`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/player/mpv/mpv_player_vk.cpp
git commit -m "$(cat <<'EOF'
fix(mpv): handle END_FILE_REASON_ERROR for proper error reporting

Like jellyfin-desktop, we now extract the error string from mpv when
playback fails and invoke the error callback to notify the UI.
EOF
)"
```

---

### Task 4: Wire Error Callback in main.cpp

**Files:**
- Modify: `/home/ar/src/github/jellyfin-labs/jellyfin-desktop/src/main.cpp:825-829`

**Step 1: Add error callback setup**

After the `mpv.setCoreIdleCallback(...)` block (around line 828), add:

```cpp
mpv.setErrorCallback([&](const std::string& error) {
    std::cerr << "[MAIN] Playback error: " << error << std::endl;
    has_video = false;
#ifndef __APPLE__
    if (has_subsurface) {
        subsurface.setVisible(false);
    }
#endif
    client->emitError(error);
    mediaSession.setPlaybackState(PlaybackState::Stopped);
});
```

**Step 2: Verify build compiles**

Run: `cmake --build /home/ar/src/github/jellyfin-labs/jellyfin-desktop/build`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
fix: wire mpv error callback to emit error to web UI

When mpv fails to play content, the error message is now propagated to
the web UI via emitError() instead of silently failing.
EOF
)"
```

---

### Task 5: Convert loadFile to Async Command

**Files:**
- Modify: `/home/ar/src/github/jellyfin-labs/jellyfin-desktop/src/player/mpv/mpv_player_vk.cpp:282-302`

**Step 1: Replace synchronous mpv_command with async version**

Replace the `loadFile` method:

```cpp
bool MpvPlayerVk::loadFile(const std::string& path, double startSeconds) {
    // Set start position before loading (mpv uses this for the next file)
    if (startSeconds > 0.0) {
        std::string startStr = std::to_string(startSeconds);
        mpv_set_option_string(mpv_, "start", startStr.c_str());
    } else {
        mpv_set_option_string(mpv_, "start", "0");
    }

    // Clear pause state before loading - ensures playback starts
    // (pause may persist from previous track)
    int pause = 0;
    mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &pause);

    // Use async command to avoid blocking main thread on load failures
    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    int ret = mpv_command_async(mpv_, 0, cmd);
    if (ret >= 0) {
        playing_ = true;
    } else {
        std::cerr << "[MPV] loadFile async failed: " << mpv_error_string(ret) << std::endl;
    }
    return ret >= 0;
}
```

**Step 2: Verify build compiles**

Run: `cmake --build /home/ar/src/github/jellyfin-labs/jellyfin-desktop/build`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/player/mpv/mpv_player_vk.cpp
git commit -m "$(cat <<'EOF'
fix(mpv): use async commands for loadFile to prevent blocking

mpv_command() is synchronous and can block for extended periods when
media fails to load. Using mpv_command_async() returns immediately
and reports errors via MPV_EVENT_END_FILE callback.
EOF
)"
```

---

### Task 6: Convert Other Commands to Async (stop, seek)

**Files:**
- Modify: `/home/ar/src/github/jellyfin-labs/jellyfin-desktop/src/player/mpv/mpv_player_vk.cpp:304-328`

**Step 1: Update stop() and seek() methods**

Replace the `stop()` method (around line 304-309):

```cpp
void MpvPlayerVk::stop() {
    if (!mpv_) return;
    const char* cmd[] = {"stop", nullptr};
    mpv_command_async(mpv_, 0, cmd);
    playing_ = false;
}
```

Replace the `seek()` method (around line 323-328):

```cpp
void MpvPlayerVk::seek(double seconds) {
    if (!mpv_) return;
    std::string time_str = std::to_string(seconds);
    const char* cmd[] = {"seek", time_str.c_str(), "absolute", nullptr};
    mpv_command_async(mpv_, 0, cmd);
}
```

**Step 2: Verify build compiles**

Run: `cmake --build /home/ar/src/github/jellyfin-labs/jellyfin-desktop/build`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/player/mpv/mpv_player_vk.cpp
git commit -m "$(cat <<'EOF'
fix(mpv): use async commands for stop/seek to prevent blocking
EOF
)"
```

---

### Task 7: Convert Property Sets to Async

**Files:**
- Modify: `/home/ar/src/github/jellyfin-labs/jellyfin-desktop/src/player/mpv/mpv_player_vk.cpp:311-370`

**Step 1: Update pause(), play(), and setters to use async**

Replace `pause()` (line 311-315):

```cpp
void MpvPlayerVk::pause() {
    if (!mpv_) return;
    int pause = 1;
    mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &pause);
}
```

Replace `play()` (line 317-321):

```cpp
void MpvPlayerVk::play() {
    if (!mpv_) return;
    int pause = 0;
    mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &pause);
}
```

Replace `setVolume()` (line 330-334):

```cpp
void MpvPlayerVk::setVolume(int volume) {
    if (!mpv_) return;
    double vol = static_cast<double>(volume);
    mpv_set_property_async(mpv_, 0, "volume", MPV_FORMAT_DOUBLE, &vol);
}
```

Replace `setMuted()` (line 336-340):

```cpp
void MpvPlayerVk::setMuted(bool muted) {
    if (!mpv_) return;
    int m = muted ? 1 : 0;
    mpv_set_property_async(mpv_, 0, "mute", MPV_FORMAT_FLAG, &m);
}
```

Replace `setSpeed()` (line 342-345):

```cpp
void MpvPlayerVk::setSpeed(double speed) {
    if (!mpv_) return;
    mpv_set_property_async(mpv_, 0, "speed", MPV_FORMAT_DOUBLE, &speed);
}
```

Replace `setSubtitleTrack()` (line 362-370):

```cpp
void MpvPlayerVk::setSubtitleTrack(int sid) {
    if (!mpv_) return;
    if (sid < 0) {
        mpv_set_property_string(mpv_, "sid", "no");
    } else {
        int64_t id = sid;
        mpv_set_property_async(mpv_, 0, "sid", MPV_FORMAT_INT64, &id);
    }
}
```

**Step 2: Verify build compiles**

Run: `cmake --build /home/ar/src/github/jellyfin-labs/jellyfin-desktop/build`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/player/mpv/mpv_player_vk.cpp
git commit -m "$(cat <<'EOF'
fix(mpv): use async property sets to prevent main thread blocking

Converting pause, play, volume, mute, speed, and subtitle track
settings to async ensures the main event loop remains responsive
even when mpv is busy processing.
EOF
)"
```

---

### Task 8: Test Error Handling

**Step 1: Build the app**

Run: `cmake --build /home/ar/src/github/jellyfin-labs/jellyfin-desktop/build`
Expected: Build succeeds with no errors

**Step 2: Manual Test - Invalid URL**

Test with an invalid URL:
1. Start the app
2. Try to play `http://invalid-host/nonexistent.mp4`
3. Verify: App should NOT hang, error message should appear in console and UI

**Step 3: Manual Test - Invalid File**

Test with a corrupted file:
1. Start the app
2. Try to play a text file renamed to `.mp4`
3. Verify: App should NOT hang, error message should appear

**Step 4: Document test results**

Note in commit message if tests passed or any issues found.

---

## Summary of Changes

1. **audio-fallback-to-null**: Prevents audio device errors from blocking playback
2. **Error callback**: New callback infrastructure for mpv errors
3. **MPV_END_FILE_REASON_ERROR handling**: Proper error extraction and reporting
4. **Async commands**: `mpv_command_async()` for loadFile, stop, seek
5. **Async property sets**: `mpv_set_property_async()` for all runtime property changes

These changes mirror how jellyfin-desktop (Qt) handles mpv errors:
- Qt uses `QueuedConnection` to keep event processing off the blocking path
- We use `mpv_command_async()` and `mpv_set_property_async()` for the same effect
- Error handling via `MPV_END_FILE_REASON_ERROR` with `mpv_error_string()`
- `audio-fallback-to-null=yes` to prevent audio init blocking
