// jellyfin-desktop Linux: native mpv VO + CEF browser overlays.
//
// Threading:
//   mpv digest thread: mpv_wait_event -> normalize -> fan out to consumer queue
//   CEF consumer thread: drains queue -> execJs/resize
//   CEF render thread: multi_threaded_message_loop (autonomous)
//   Input thread: Wayland dispatch -> CEF key/mouse -> mpv async
//   mpv VO thread: configure/close callbacks -> surface ops
//   Main thread: startup -> waitForClose -> cleanup

#include "version.h"
#include "common.h"
#include "cef/cef_app.h"
#include "cef/cef_client.h"
#include "mpv_event.h"
#include "event_queue.h"
#include "wake_event.h"
#include "settings.h"

#include "player/media_session.h"
#include "player/media_session_thread.h"

#include "logging.h"

#ifdef __APPLE__
#include "include/wrapper/cef_library_loader.h"
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include "single_instance.h"
#include "player/windows/media_session_windows.h"
#else
#include "single_instance.h"
#include "player/mpris/media_session_mpris.h"
#endif

#include "include/cef_parser.h"
#include "include/cef_version.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#endif
#include <string>
#include <vector>
#include <thread>
#include <filesystem>
#include <atomic>
#ifndef _WIN32
#include <poll.h>
#endif

// =====================================================================
// Globals
// =====================================================================

MpvHandle g_mpv;
std::atomic<bool> g_shutting_down{false};
WakeEvent g_shutdown_event;
Platform g_platform{};
CefRefPtr<Client> g_client;
CefRefPtr<OverlayClient> g_overlay_client;

// Close a browser if it has one, or mark it closed if it was never created.
// Either way, waitForClose() will return.
static void close_or_mark(CefRefPtr<Client> c) {
    if (!c) return;
    if (c->browser()) c->browser()->GetHost()->CloseBrowser(true);
    else if (!c->isClosed()) {
        // Browser was never created — unblock waitForClose manually
        c->markClosed();
    }
}
static void close_or_mark(CefRefPtr<OverlayClient> c) {
    if (!c) return;
    if (c->browser()) c->browser()->GetHost()->CloseBrowser(true);
    else if (!c->isClosed()) {
        c->markClosed();
    }
}

void initiate_shutdown() {
    bool expected = false;
    if (!g_shutting_down.compare_exchange_strong(expected, true)) return;
    close_or_mark(g_client);
    close_or_mark(g_overlay_client);
    g_shutdown_event.signal();
}

static void signal_handler(int) {
    initiate_shutdown();
}

// =====================================================================
// Event bus
// =====================================================================

static EventQueue<MpvEvent> g_cef_queue;

static MpvEvent digest_property(mpv_event_property* p) {
    MpvEvent ev{};
    if (strcmp(p->name, "osd-dimensions") == 0) {
        ev.type = MpvEventType::OSD_DIMS;
        int64_t w = 0, h = 0;
        g_mpv.GetPropertyInt("osd-width", w);
        g_mpv.GetPropertyInt("osd-height", h);
        ev.pw = static_cast<int>(w);
        ev.ph = static_cast<int>(h);
        float scale = g_platform.get_scale();
        ev.lw = static_cast<int>(ev.pw / scale);
        ev.lh = static_cast<int>(ev.ph / scale);
#ifdef __APPLE__
        int qlw = 0, qlh = 0;
        if (g_platform.query_logical_content_size(&qlw, &qlh) && qlw > 0 && qlh > 0) {
            ev.lw = qlw; ev.lh = qlh;
            ev.pw = static_cast<int>(qlw * scale);
            ev.ph = static_cast<int>(qlh * scale);
        }
#endif
    } else if (strcmp(p->name, "pause") == 0 && p->format == MPV_FORMAT_FLAG) {
        ev.type = MpvEventType::PAUSE;
        ev.flag = *static_cast<int*>(p->data) != 0;
    } else if (strcmp(p->name, "time-pos") == 0 && p->format == MPV_FORMAT_DOUBLE) {
        ev.type = MpvEventType::TIME_POS;
        ev.dbl = *static_cast<double*>(p->data);
    } else if (strcmp(p->name, "duration") == 0 && p->format == MPV_FORMAT_DOUBLE) {
        ev.type = MpvEventType::DURATION;
        ev.dbl = *static_cast<double*>(p->data);
    } else if (strcmp(p->name, "fullscreen") == 0 && p->format == MPV_FORMAT_FLAG) {
        ev.type = MpvEventType::FULLSCREEN;
        ev.flag = *static_cast<int*>(p->data) != 0;
    } else if (strcmp(p->name, "speed") == 0 && p->format == MPV_FORMAT_DOUBLE) {
        ev.type = MpvEventType::SPEED;
        ev.dbl = *static_cast<double*>(p->data);
    } else if (strcmp(p->name, "seeking") == 0 && p->format == MPV_FORMAT_FLAG) {
        ev.type = MpvEventType::SEEKING;
        ev.flag = *static_cast<int*>(p->data) != 0;
    }
    return ev;
}

static void publish(const MpvEvent& ev) {
    g_cef_queue.try_push(ev);
}

static void mpv_digest_thread() {
    while (!g_shutting_down.load(std::memory_order_relaxed)) {
        mpv_event* ev = g_mpv.WaitEvent(-1);
        if (ev->event_id == MPV_EVENT_NONE) continue;

        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            auto* msg = static_cast<mpv_event_log_message*>(ev->data);
            LOG_DEBUG(LOG_MPV, "[%s] %s", msg->prefix, msg->text);
            continue;
        }

        if (ev->event_id == MPV_EVENT_SHUTDOWN) {
            MpvEvent se{MpvEventType::SHUTDOWN};
            publish(se);
            initiate_shutdown();
            return;
        }

        if (ev->event_id == MPV_EVENT_FILE_LOADED) {
            MpvEvent fe{MpvEventType::FILE_LOADED};
            publish(fe);
            continue;
        }

        if (ev->event_id == MPV_EVENT_END_FILE) {
            auto* d = static_cast<mpv_event_end_file*>(ev->data);
            MpvEvent fe{};
            if (d->reason == MPV_END_FILE_REASON_EOF)
                fe.type = MpvEventType::END_FILE_EOF;
            else if (d->reason == MPV_END_FILE_REASON_STOP)
                fe.type = MpvEventType::END_FILE_CANCEL;
            else
                fe.type = MpvEventType::END_FILE_ERROR;
            publish(fe);
            continue;
        }

        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* p = static_cast<mpv_event_property*>(ev->data);
            if (!p->name) continue;
            MpvEvent me = digest_property(p);
            if (me.type == MpvEventType::NONE) continue;
            if (me.type == MpvEventType::OSD_DIMS) {
                if (me.lw <= 0 || me.lh <= 0) continue;
                if (g_platform.in_transition())
                    g_platform.set_expected_size(me.pw, me.ph);
                g_platform.resize(me.lw, me.lh, me.pw, me.ph);
            }
            publish(me);
        }
    }
}

// =====================================================================
// CEF consumer thread
// =====================================================================

MediaSessionThread* g_media_session = nullptr;
static bool g_was_maximized_before_fullscreen = false;

static void cef_consumer_thread() {
    // Wait for main browser to load before processing events
    g_client->waitForLoad();

#ifdef _WIN32
    HANDLE handles[2] = {
        g_cef_queue.wake_handle(),
        g_shutdown_event.handle()
    };
#else
    int wake_fd = g_cef_queue.wake().fd();
    int shutdown_fd = g_shutdown_event.fd();
    struct pollfd fds[2] = {
        {wake_fd, POLLIN, 0},
        {shutdown_fd, POLLIN, 0},
    };
#endif

    while (true) {
#ifdef _WIN32
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        // Check shutdown
        if (WaitForSingleObject(handles[1], 0) == WAIT_OBJECT_0) break;
#else
        poll(fds, 2, -1);
        if (fds[1].revents & POLLIN) break;
#endif

        g_cef_queue.drain_wake();
        MpvEvent ev;
        while (g_cef_queue.try_pop(ev)) {
            if (!g_client) continue;
            switch (ev.type) {
            case MpvEventType::PAUSE:
                g_client->execJs(ev.flag ? "window._nativeEmit('paused')" : "window._nativeEmit('playing')");
                if (g_media_session)
                    g_media_session->setPlaybackState(ev.flag ? PlaybackState::Paused : PlaybackState::Playing);
                break;
            case MpvEventType::TIME_POS: {
                int ms = static_cast<int>(ev.dbl * 1000);
                g_client->execJs("window._nativeUpdatePosition(" + std::to_string(ms) + ")");
                if (g_media_session)
                    g_media_session->setPosition(static_cast<int64_t>(ev.dbl * 1000000));
                break;
            }
            case MpvEventType::DURATION: {
                int ms = static_cast<int>(ev.dbl * 1000);
                g_client->execJs("window._nativeUpdateDuration(" + std::to_string(ms) + ")");
                // Duration is set via metadata, not a separate call
                break;
            }
            case MpvEventType::FULLSCREEN:
                if (ev.flag) {
                    // Entering fullscreen: capture maximized state for save/restore
                    bool maximized = false;
                    g_mpv.GetWindowMaximized(maximized);
                    g_was_maximized_before_fullscreen = maximized;
                } else {
                    g_was_maximized_before_fullscreen = false;
                }
                g_client->execJs("window._nativeFullscreenChanged(" + std::string(ev.flag ? "true" : "false") + ")");
                break;
            case MpvEventType::SPEED:
                g_client->execJs("window._nativeSetRate(" + std::to_string(ev.dbl) + ")");
                if (g_media_session)
                    g_media_session->setRate(ev.dbl);
                break;
            case MpvEventType::SEEKING:
                if (ev.flag) {
                    g_client->execJs("window._nativeEmit('seeking')");
                    if (g_media_session) g_media_session->emitSeeking();
                }
                break;
            case MpvEventType::FILE_LOADED:
                g_client->execJs("window._nativeEmit('playing')");
                if (g_media_session)
                    g_media_session->setPlaybackState(PlaybackState::Playing);
                break;
            case MpvEventType::END_FILE_EOF:
                g_client->execJs("window._nativeEmit('finished')");
                if (g_media_session)
                    g_media_session->setPlaybackState(PlaybackState::Stopped);
                break;
            case MpvEventType::END_FILE_ERROR:
                g_client->execJs("window._nativeEmit('error','Playback error')");
                if (g_media_session)
                    g_media_session->setPlaybackState(PlaybackState::Stopped);
                break;
            case MpvEventType::END_FILE_CANCEL:
                g_client->execJs("window._nativeEmit('canceled')");
                if (g_media_session)
                    g_media_session->setPlaybackState(PlaybackState::Stopped);
                break;
            case MpvEventType::OSD_DIMS:
                if (g_client->browser())
                    g_client->resize(ev.lw, ev.lh, ev.pw, ev.ph);
                if (g_overlay_client && g_overlay_client->browser()) {
                    g_overlay_client->resize(ev.lw, ev.lh, ev.pw, ev.ph);
                    g_platform.overlay_resize(ev.lw, ev.lh, ev.pw, ev.ph);
                }
                break;
            case MpvEventType::SHUTDOWN:
                return;
            default:
                break;
            }
        }
    }
}

// =====================================================================
// Main
// =====================================================================

int main(int argc, char* argv[]) {
    // --- Platform early init + CEF subprocess check ---
    // Must be first: CEF subprocesses (GPU, renderer) re-execute this binary.
    // They must hit CefExecuteProcess immediately and exit — before CLI parsing,
    // settings, single instance, or anything else touches shared state.
#ifdef _WIN32
    g_platform = make_windows_platform();
#elif defined(__APPLE__)
    g_platform = make_macos_platform();
#else
    g_platform = make_wayland_platform();
#endif
    g_platform.early_init();

#ifdef __APPLE__
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInMain()) {
        fprintf(stderr, "Failed to load CEF library\n");
        return 1;
    }
#endif

#ifdef _WIN32
    SetEnvironmentVariableA("JELLYFIN_CEF_SUBPROCESS", "1");
#else
    setenv("JELLYFIN_CEF_SUBPROCESS", "1", 1);
#endif
#ifdef _WIN32
    CefMainArgs main_args(GetModuleHandle(NULL));
#else
    CefMainArgs main_args(argc, argv);
#endif
    CefRefPtr<App> app(new App());
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) return exit_code;

    // --- Parse CLI ---
    std::string hwdec_str = "auto-safe";
    std::string audio_passthrough_str;
    bool audio_exclusive = false;
    std::string audio_channels_str;
    bool player_mode = false;
    std::vector<std::string> player_playlist;
    int remote_debugging_port = 0;
    const char* log_level_str = nullptr;
    const char* log_file_path = nullptr;

    Settings::instance().load();
    auto& saved = Settings::instance();
    if (!saved.hwdec().empty()) hwdec_str = saved.hwdec();
    if (!saved.audioPassthrough().empty()) audio_passthrough_str = saved.audioPassthrough();
    audio_exclusive = saved.audioExclusive();
    if (!saved.audioChannels().empty()) audio_channels_str = saved.audioChannels();
    std::string saved_log_level = saved.logLevel();
    if (!saved_log_level.empty()) log_level_str = saved_log_level.c_str();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: jellyfin-desktop [options]\n"
                   "       jellyfin-desktop --player [options] <file|url>...\n"
                   "\nOptions:\n"
                   "  -h, --help                Show this help\n"
                   "  -v, --version             Show version\n"
                   "  --log-level <level>       verbose|debug|info|warn|error\n"
                   "  --log-file <path>         Write logs to file\n"
                   "  --hwdec <mode>            Hardware decoding mode (default: auto-safe)\n"
                   "  --audio-passthrough <codecs>  e.g. ac3,dts-hd,eac3,truehd\n"
                   "  --audio-exclusive         Exclusive audio output\n"
                   "  --audio-channels <layout> e.g. stereo, 5.1, 7.1\n"
                   "  --remote-debug-port <port> Chrome remote debugging\n"
                   "  --player                  Standalone player mode\n");
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("jellyfin-desktop %s\nCEF %s\n", APP_VERSION_STRING, CEF_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            log_level_str = argv[++i];
        } else if (strncmp(argv[i], "--log-level=", 12) == 0) {
            log_level_str = argv[i] + 12;
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            log_file_path = argv[++i];
        } else if (strncmp(argv[i], "--log-file=", 11) == 0) {
            log_file_path = argv[i] + 11;
        } else if (strcmp(argv[i], "--hwdec") == 0 && i + 1 < argc) {
            hwdec_str = argv[++i];
        } else if (strncmp(argv[i], "--hwdec=", 8) == 0) {
            hwdec_str = argv[i] + 8;
        } else if (strcmp(argv[i], "--audio-passthrough") == 0 && i + 1 < argc) {
            audio_passthrough_str = argv[++i];
        } else if (strncmp(argv[i], "--audio-passthrough=", 20) == 0) {
            audio_passthrough_str = argv[i] + 20;
        } else if (strcmp(argv[i], "--audio-exclusive") == 0) {
            audio_exclusive = true;
        } else if (strcmp(argv[i], "--audio-channels") == 0 && i + 1 < argc) {
            audio_channels_str = argv[++i];
        } else if (strncmp(argv[i], "--audio-channels=", 17) == 0) {
            audio_channels_str = argv[i] + 17;
        } else if (strcmp(argv[i], "--remote-debug-port") == 0 && i + 1 < argc) {
            remote_debugging_port = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--remote-debug-port=", 20) == 0) {
            remote_debugging_port = atoi(argv[i] + 20);
        } else if (strcmp(argv[i], "--player") == 0) {
            player_mode = true;
        } else if (argv[i][0] != '-') {
            player_playlist.push_back(argv[i]);
        }
    }

    if (log_level_str && log_level_str[0]) {
        int level = parseLogLevel(log_level_str);
        if (level >= 0) initLogging(static_cast<int>(level));
    } else {
        initLogging();
    }
    if (log_file_path && log_file_path[0]) {
        g_log_file = fopen(log_file_path, "w");
    }

    if (player_mode && player_playlist.empty()) {
        fprintf(stderr, "Error: --player requires at least one file or URL\n");
        return 1;
    }

    LOG_INFO(LOG_MAIN, "jellyfin-desktop " APP_VERSION_STRING);
    LOG_INFO(LOG_MAIN, "CEF %s", CEF_VERSION);

    // --- Signal handlers ---
#ifdef _WIN32
    SetConsoleCtrlHandler([](DWORD) -> BOOL {
        initiate_shutdown();
        return TRUE;
    }, TRUE);
#else
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif

#ifndef __APPLE__
    // --- Single instance ---
    if (trySignalExisting()) {
        LOG_INFO(LOG_MAIN, "Signaled existing instance, exiting");
        return 0;
    }
    startListener([](const std::string&) {
        // TODO: raise window via xdg-activation
    });
    // Ensure listener thread is joined on any exit path (std::thread
    // destructor calls std::terminate if joinable).
    struct ListenerGuard { ~ListenerGuard() { stopListener(); } } listener_guard;
#endif

    // --- mpv setup ---
    g_mpv = MpvHandle::Create();
    if (!g_mpv.IsValid()) { LOG_ERROR(LOG_MAIN, "mpv_create failed"); return 1; }

#ifdef __APPLE__
    setenv("MPVBUNDLE", "true", 1);
#endif

    g_mpv.SetOptionString("vo", "gpu-next");
    g_mpv.SetOptionString("gpu-api", "vulkan");
    g_mpv.SetOptionString("hwdec", hwdec_str);
    g_mpv.SetOptionString("target-colorspace-hint", "yes");
    g_mpv.SetOptionString("osd-level", "0");
    g_mpv.SetOptionString("osc", "no");
    g_mpv.SetOptionString("input-default-bindings", "no");
    g_mpv.SetOptionString("input-vo-keyboard", "no");
    g_mpv.SetOptionString("input-vo-cursor", "no");
    g_mpv.SetOptionString("keepaspect-window", "no");
    g_mpv.SetOptionString("auto-window-resize", "no");
    g_mpv.SetOptionString("border", "yes");
    g_mpv.SetOptionString("title", "Jellyfin Desktop");
    g_mpv.SetOptionString("wayland-app-id", "org.jellyfin.JellyfinDesktop");
#ifdef _WIN32
    // Tell mpv to load window icon from our exe resources (read at class
    // registration time, before any window is created — no icon flash)
    SetEnvironmentVariableW(L"MPV_WINDOW_ICON", L"IDI_ICON1");
#endif
#ifdef __APPLE__
    // macOS VO (mac_common.swift:37) uses DispatchQueue.main.sync for window
    // creation. mp_initialize (main.c:435) calls handle_force_window when
    // force_vo==2 ("immediate"), which deadlocks because main is blocked in
    // mpv_initialize and can't service GCD.
    //
    // force-window=yes (force_vo=1) SKIPS the init-time call. idle=yes makes
    // core_thread enter idle_loop (playloop.c:1349), which calls
    // handle_force_window(mpctx, true) from the core thread. By that point,
    // main has returned from mpv_initialize and is pumping GCD in the VO
    // wait loop — DispatchQueue.main.sync succeeds.
    g_mpv.SetOptionString("force-window", "yes");
    g_mpv.SetOptionString("idle", "yes");
#else
    g_mpv.SetOptionString("force-window", "yes");
#endif

#ifndef __APPLE__
    // Restore saved window geometry
    {
        auto saved_geom = Settings::instance().windowGeometry();
        int w = saved_geom.width > 0 ? saved_geom.width : 1280;
        int h = saved_geom.height > 0 ? saved_geom.height : 720;
        std::string geom_str = std::to_string(w) + "x" + std::to_string(h);
        g_mpv.SetOptionString("geometry", geom_str);
        if (saved_geom.maximized)
            g_mpv.SetOptionString("window-maximized", "yes");
    }
#endif

    if (!audio_passthrough_str.empty()) {
        // Normalize: dts-hd subsumes dts
        if (audio_passthrough_str.find("dts-hd") != std::string::npos) {
            std::string filtered;
            size_t pos = 0;
            while (pos < audio_passthrough_str.size()) {
                size_t comma = audio_passthrough_str.find(',', pos);
                if (comma == std::string::npos) comma = audio_passthrough_str.size();
                std::string codec = audio_passthrough_str.substr(pos, comma - pos);
                if (codec != "dts") {
                    if (!filtered.empty()) filtered += ',';
                    filtered += codec;
                }
                pos = comma + 1;
            }
            audio_passthrough_str = filtered;
        }
        g_mpv.SetOptionString("audio-spdif", audio_passthrough_str);
    }
    if (audio_exclusive)
        g_mpv.SetOptionString("audio-exclusive", "yes");
    if (!audio_channels_str.empty())
        g_mpv.SetOptionString("audio-channels", audio_channels_str);

    // Register property observations before mpv_initialize. On macOS,
    // core_thread races to DispatchQueue.main.sync immediately after init
    // returns — main must enter the GCD pump loop without delay.
    g_mpv.SetWakeupCallback([](void*) {}, nullptr);
    g_mpv.ObservePropertyNode(1, "video-params");
    g_mpv.ObservePropertyNode(2, "osd-dimensions");
    g_mpv.ObservePropertyFlag(3, "fullscreen");
    g_mpv.ObservePropertyFlag(4, "pause");
    g_mpv.ObservePropertyDouble(5, "time-pos");
    g_mpv.ObservePropertyDouble(6, "duration");
    g_mpv.ObservePropertyDouble(7, "speed");
    g_mpv.ObservePropertyFlag(8, "seeking");

    // Load file if in player mode (before init so it's in the playlist)
    if (player_mode) {
        g_mpv.LoadFile(player_playlist[0]);
    }

    int init_err = g_mpv.Initialize();
    if (init_err < 0) {
        LOG_ERROR(LOG_MAIN, "mpv_initialize failed: %d", init_err);
        g_mpv.TerminateDestroy();
        return 1;
    }

    // --- Wait for VO (mpv needs a window before we can get platform handles) ---
    LOG_INFO(LOG_MAIN, "Waiting for mpv window...");
#ifdef __APPLE__
    // macOS: pump GCD + poll mpv events. NEVER call mpv_get_property here —
    // it uses lock_core (mp_dispatch_lock) which deadlocks if core_thread
    // holds the dispatch lock during VO init → DispatchQueue.main.sync.
    // Wait for osd-dimensions property change event instead.
    while (true) {
        g_platform.pump();
        mpv_event* ev = g_mpv.WaitEvent(0);
        if (ev->event_id == MPV_EVENT_NONE) { usleep(10000); continue; }
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            auto* msg = static_cast<mpv_event_log_message*>(ev->data);
            LOG_DEBUG(LOG_MPV, "[%s] %s", msg->prefix, msg->text);
            continue;
        }
        if (ev->event_id == MPV_EVENT_SHUTDOWN || ev->event_id == MPV_EVENT_END_FILE) {
            g_mpv.TerminateDestroy(); return 0;
        }
        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* p = static_cast<mpv_event_property*>(ev->data);
            if (p->name && strcmp(p->name, "video-params") == 0 && p->data) break;
            if (p->name && strcmp(p->name, "osd-dimensions") == 0 && p->data) break;
        }
    }
#else
    while (true) {
        mpv_event* ev = g_mpv.WaitEvent(1.0);
        if (ev->event_id == MPV_EVENT_SHUTDOWN) { g_mpv.TerminateDestroy(); return 0; }
        if (ev->event_id == MPV_EVENT_END_FILE) { g_mpv.TerminateDestroy(); return 0; }
        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* p = static_cast<mpv_event_property*>(ev->data);
            if (p->name && strcmp(p->name, "video-params") == 0 && p->data) break;
        }
        int64_t ow = 0;
        g_mpv.GetPropertyInt("osd-width", ow);
        if (ow > 0) break;
    }
#endif

    // --- Platform init ---
    if (!g_platform.init(g_mpv.Get())) {
        LOG_ERROR(LOG_MAIN, "Platform init failed");
        g_mpv.TerminateDestroy();
        return 1;
    }
    LOG_INFO(LOG_MAIN, "Platform init ok");

#ifdef __APPLE__
    int64_t mw = 1280, mh = 720;
#else
    int64_t mw = 0, mh = 0;
    g_mpv.GetPropertyInt("osd-width", mw);
    g_mpv.GetPropertyInt("osd-height", mh);
    if (mw <= 0 || mh <= 0) { mw = 1280; mh = 720; }
#endif

    // --- CEF init ---
    CefSettings settings{};
    settings.windowless_rendering_enabled = true;
#ifdef __APPLE__
    settings.external_message_pump = true;
#else
    settings.multi_threaded_message_loop = true;
#endif
    settings.no_sandbox = true;
    CefString(&settings.locale).FromASCII("en-US");

#ifdef __APPLE__
    char exe_buf[4096];
    uint32_t exe_size = sizeof(exe_buf);
    _NSGetExecutablePath(exe_buf, &exe_size);
    auto exe = std::filesystem::canonical(exe_buf);
    auto app_contents = exe.parent_path().parent_path();
    auto fw_path = (app_contents / "Frameworks" / "Chromium Embedded Framework.framework").string();
    CefString(&settings.framework_dir_path).FromString(fw_path);
    CefString(&settings.browser_subprocess_path).FromString(exe.string());
    auto home = std::string(getenv("HOME"));
    CefString(&settings.root_cache_path).FromString(home + "/Library/Caches/jellyfin-desktop");
#elif defined(_WIN32)
    char exe_buf[MAX_PATH];
    GetModuleFileNameA(NULL, exe_buf, MAX_PATH);
    auto exe_path = std::filesystem::canonical(exe_buf);
    auto exe_dir = exe_path.parent_path();
    CefString(&settings.browser_subprocess_path).FromString(exe_path.string());
    CefString(&settings.resources_dir_path).FromString(exe_dir.string());
    CefString(&settings.locales_dir_path).FromString((exe_dir / "locales").string());
    // Cache: %LOCALAPPDATA%\jellyfin-desktop
    const char* localappdata = getenv("LOCALAPPDATA");
    std::string cache_path;
    if (localappdata && localappdata[0])
        cache_path = std::string(localappdata) + "\\jellyfin-desktop";
    else
        cache_path = (exe_dir / "cache").string();
    CefString(&settings.root_cache_path).FromString(cache_path);
#else
    CefString(&settings.browser_subprocess_path).FromString(
        std::filesystem::canonical("/proc/self/exe").string());
    auto exe_dir = std::filesystem::canonical("/proc/self/exe").parent_path();
#ifdef CEF_RESOURCES_DIR
    CefString(&settings.resources_dir_path).FromString(CEF_RESOURCES_DIR);
    CefString(&settings.locales_dir_path).FromString(std::string(CEF_RESOURCES_DIR) + "/locales");
#else
    CefString(&settings.resources_dir_path).FromString(exe_dir.string());
    CefString(&settings.locales_dir_path).FromString((exe_dir / "locales").string());
#endif
    const char* xdg_cache = getenv("XDG_CACHE_HOME");
    std::string cache_path;
    if (xdg_cache && xdg_cache[0]) {
        cache_path = std::string(xdg_cache) + "/jellyfin-desktop";
    } else {
        const char* lhome = getenv("HOME");
        cache_path = std::string(lhome ? lhome : "/tmp") + "/.cache/jellyfin-desktop";
    }
    CefString(&settings.root_cache_path).FromString(cache_path);
#endif

    if (remote_debugging_port > 0)
        settings.remote_debugging_port = remote_debugging_port;

    if (!CefInitialize(main_args, settings, app, nullptr)) {
        LOG_ERROR(LOG_MAIN, "CefInitialize failed");
        g_platform.cleanup();
        g_mpv.TerminateDestroy();
        return 1;
    }
    LOG_INFO(LOG_MAIN, "CefInitialize ok");

#ifdef __APPLE__
    App::InitWakePipe();
#endif

    // --- Create browsers ---
    float scale = g_platform.get_scale();
    int lw = static_cast<int>(mw / scale);
    int lh = static_cast<int>(mh / scale);

    CefWindowInfo wi;
    wi.SetAsWindowless(0);
    wi.shared_texture_enabled = true;
    wi.external_begin_frame_enabled = false;
    CefBrowserSettings bs;
    bs.background_color = 0;
    bs.windowless_frame_rate = 60;

    // Main browser
    g_client = new Client();
    g_client->resize(lw, lh, (int)mw, (int)mh);
    g_platform.resize(lw, lh, (int)mw, (int)mh);

    std::string server_url = Settings::instance().serverUrl();
    std::string main_url;

    if (player_mode) {
        // Build player URL with playlist
        auto list = CefListValue::Create();
        for (size_t i = 0; i < player_playlist.size(); i++)
            list->SetString(i, player_playlist[i]);
        auto val = CefValue::Create();
        val->SetList(list);
        auto json = CefWriteJSON(val, JSON_WRITER_DEFAULT);
        auto encoded = CefURIEncode(json, false);
        main_url = "app://resources/player.html#" + encoded.ToString();
    } else if (!server_url.empty()) {
        main_url = server_url;
    } else {
        // No server URL known -- load overlay for server selection
        main_url = "app://resources/index.html";
    }

#ifdef __APPLE__
    App::DoWork();
#endif
    CefBrowserHost::CreateBrowser(wi, g_client, main_url, bs, nullptr, nullptr);

    // Overlay browser (server selection UI) -- only in full app mode
    if (!player_mode) {
        g_overlay_client = new OverlayClient();
        g_overlay_client->resize(lw, lh, (int)mw, (int)mh);
        g_platform.set_overlay_visible(true);

        CefWindowInfo owi;
        owi.SetAsWindowless(0);
        owi.shared_texture_enabled = true;
        owi.external_begin_frame_enabled = false;
        CefBrowserSettings obs;
        obs.background_color = 0;
        obs.windowless_frame_rate = 60;
        CefBrowserHost::CreateBrowser(owi, g_overlay_client, "app://resources/index.html", obs, nullptr, nullptr);
    }

#ifdef __APPLE__
    // nothing — main thread pump happens below
#else
    g_client->waitForLoad();
#endif
    LOG_INFO(LOG_MAIN, "Main browser loaded");

#ifdef __APPLE__
    // nothing -- macOS media session not yet implemented in new arch
#elif defined(_WIN32)
    // --- Windows SMTC media session ---
    MediaSession media_session_obj;
    int64_t wid = 0;
    g_mpv.GetPropertyInt("window-id", wid);
    auto win_backend = createWindowsMediaBackend(&media_session_obj, (HWND)(intptr_t)wid);
    media_session_obj.addBackend(std::move(win_backend));

    // Wire transport controls.
    // Play/pause/stop go directly to mpv (authoritative source).
    // Next/previous/seek go through JS (jellyfin-web manages the playlist).
    media_session_obj.onPlay = []() {
        g_mpv.Play();
    };
    media_session_obj.onPause = []() {
        g_mpv.Pause();
    };
    media_session_obj.onPlayPause = []() {
        g_mpv.TogglePause();
    };
    media_session_obj.onStop = []() {
        g_mpv.Stop();
    };
    media_session_obj.onNext = []() {
        if (g_client) g_client->execJs("if(window._nativeHostInput) window._nativeHostInput(['next']);");
    };
    media_session_obj.onPrevious = []() {
        if (g_client) g_client->execJs("if(window._nativeHostInput) window._nativeHostInput(['previous']);");
    };
    media_session_obj.onSeek = [](int64_t position_us) {
        int ms = static_cast<int>(position_us / 1000);
        if (g_client) g_client->execJs("if(window._nativeSeek) window._nativeSeek(" + std::to_string(ms) + ");");
    };
    media_session_obj.onSetRate = [](double rate) {
        double clamped = rate < 0.25 ? 0.25 : (rate > 2.0 ? 2.0 : rate);
        g_mpv.SetSpeed(clamped);
    };

    MediaSessionThread media_session_thread;
    media_session_thread.start(&media_session_obj);
    g_media_session = &media_session_thread;
#else
    // --- MPRIS media session ---
    MediaSession media_session_obj;
    auto mpris_backend = std::make_unique<MprisBackend>(&media_session_obj);
    media_session_obj.addBackend(std::move(mpris_backend));

    // Wire MPRIS transport controls.
    // Play/pause/stop go directly to mpv (authoritative source).
    // Next/previous/seek go through JS (jellyfin-web manages the playlist).
    media_session_obj.onPlay = []() {
        g_mpv.Play();
    };
    media_session_obj.onPause = []() {
        g_mpv.Pause();
    };
    media_session_obj.onPlayPause = []() {
        g_mpv.TogglePause();
    };
    media_session_obj.onStop = []() {
        g_mpv.Stop();
    };
    media_session_obj.onNext = []() {
        if (g_client) g_client->execJs("if(window._nativeHostInput) window._nativeHostInput(['next']);");
    };
    media_session_obj.onPrevious = []() {
        if (g_client) g_client->execJs("if(window._nativeHostInput) window._nativeHostInput(['previous']);");
    };
    media_session_obj.onSeek = [](int64_t position_us) {
        int ms = static_cast<int>(position_us / 1000);
        if (g_client) g_client->execJs("if(window._nativeSeek) window._nativeSeek(" + std::to_string(ms) + ");");
    };
    media_session_obj.onSetRate = [](double rate) {
        double clamped = rate < 0.25 ? 0.25 : (rate > 2.0 ? 2.0 : rate);
        g_mpv.SetSpeed(clamped);
    };

    MediaSessionThread media_session_thread;
    media_session_thread.start(&media_session_obj);
    g_media_session = &media_session_thread;
#endif

    // --- Start threads ---
    std::thread digest_thread(mpv_digest_thread);
    std::thread cef_thread(cef_consumer_thread);

    LOG_INFO(LOG_MAIN, "Running");

    // --- Wait for shutdown ---
#ifdef __APPLE__
    // macOS: main thread must pump both GCD (for mpv VO DispatchQueue.main.sync)
    // and CEF (CefDoMessageLoopWork must run on the init thread = main).
    //
    // poll() on CEF wake pipe + shutdown fd. CEF work wakes immediately via pipe.
    // GCD has no pollable fd in Apple's SDK, so poll() uses a timeout as fallback
    // for GCD dispatch processing (mpv VO init, fullscreen transitions — rare
    // events where up to 100ms latency is imperceptible).
    {
        int cef_fd = App::WakeFd();
        int shutdown_fd = g_shutdown_event.fd();
        struct pollfd fds[2] = {
            {cef_fd, POLLIN, 0},
            {shutdown_fd, POLLIN, 0},
        };
        App::ScheduleWork();
        while (true) {
            g_platform.pump();
            int ret = poll(fds, 2, 100);
            if (fds[1].revents & POLLIN) break;
            if (fds[0].revents & POLLIN || ret == 0)
                App::DoWork();
        }
    }
#else
    g_client->waitForClose();
    if (g_overlay_client)
        g_overlay_client->waitForClose();
#endif

    // --- Cleanup ---
    // Stop our threads first (they don't depend on CEF/mpv shutdown order)
#ifndef __APPLE__
    g_media_session = nullptr;
    media_session_thread.stop();
#endif

    cef_thread.join();
    g_mpv.Wakeup();
    digest_thread.join();

#ifndef __APPLE__
    // Save window geometry while mpv is still alive.
    // Three paths match old SDL implementation:
    //   Fullscreen: preserve previous saved geometry, update only maximized flag
    //   Maximized:  zero out size (sentinel), set maximized=true
    //   Normal:     save current logical size
    {
        bool fs = false, max = false;
        g_mpv.GetFullscreen(fs);
        g_mpv.GetWindowMaximized(max);

        if (fs) {
            // Preserve previous saved geometry; only update the maximized flag
            // to reflect whether the user was maximized before entering fullscreen.
            auto geom = Settings::instance().windowGeometry();
            geom.maximized = g_was_maximized_before_fullscreen;
            Settings::instance().setWindowGeometry(geom);
        } else if (max) {
            // Preserve the previous saved windowed size (don't save the
            // maximized dimensions — they're the monitor size). On next
            // launch the window opens maximized; on unmaximize, the
            // preserved size is used.
            auto geom = Settings::instance().windowGeometry();
            geom.maximized = true;
            Settings::instance().setWindowGeometry(geom);
        } else {
            // Normal windowed: save current logical size.
            int64_t pw = 0, ph = 0;
            g_mpv.GetOsdWidth(pw);
            g_mpv.GetOsdHeight(ph);
            if (pw > 0 && ph > 0) {
                float scale = g_platform.get_scale();
                Settings::WindowGeometry geom;
                geom.width = static_cast<int>(pw / scale);
                geom.height = static_cast<int>(ph / scale);
                geom.maximized = false;
                Settings::instance().setWindowGeometry(geom);
            }
        }
        Settings::instance().save();
    }
#endif

    // CEF shutdown: all browsers must be closed first (guaranteed by waitForClose above)
    g_client = nullptr;
    g_overlay_client = nullptr;
    CefShutdown();

    // Platform cleanup (joins input thread, destroys subsurfaces)
    // Must happen after CefShutdown (CEF may still present during shutdown)
    // but before mpv_terminate_destroy (mpv destroys the parent surface)
    g_platform.cleanup();
    g_mpv.TerminateDestroy();

    if (g_log_file) fclose(g_log_file);
    return 0;
}
