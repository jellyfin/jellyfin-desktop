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
#include "browser/browsers.h"
#include "browser/web_browser.h"
#include "browser/overlay_browser.h"
#include "browser/about_browser.h"
#include "mpv/event.h"
#include "mpv/options.h"
#include "event_queue.h"
#include "wake_event.h"
#include "paths/paths.h"
#include "settings.h"
#include "titlebar_color.h"

#include "player/media_session.h"
#include "player/media_session_thread.h"

#include "logging.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#else
#include "single_instance.h"
#endif

#include "include/cef_parser.h"
#include "include/cef_version.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#endif
#include <string>
#include <vector>
#include <thread>
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

std::atomic<MediaType> g_media_type{MediaType::Unknown};
std::atomic<PlaybackState> g_playback_state{PlaybackState::Stopped};
TitlebarColor* g_titlebar_color = nullptr;
std::atomic<int> g_display_hz{60};

void update_idle_inhibit() {
    if (g_playback_state.load(std::memory_order_relaxed) != PlaybackState::Playing) {
        g_platform.set_idle_inhibit(IdleInhibitLevel::None);
    } else if (g_media_type.load(std::memory_order_relaxed) == MediaType::Audio) {
        g_platform.set_idle_inhibit(IdleInhibitLevel::System);
    } else {
        g_platform.set_idle_inhibit(IdleInhibitLevel::Display);
    }
}
Platform g_platform{};
WebBrowser* g_web_browser = nullptr;
OverlayBrowser* g_overlay_browser = nullptr;

static void try_close_browser(auto* b) {
    if (b && b->browser()) b->browser()->GetHost()->CloseBrowser(true);
}

void initiate_shutdown() {
    bool expected = false;
    if (!g_shutting_down.compare_exchange_strong(expected, true)) return;
    try_close_browser(g_web_browser);
    try_close_browser(g_overlay_browser);
    try_close_browser(g_about_browser);
    g_shutdown_event.signal();
    // macOS main thread is parked in nextEventMatchingMask — post a sentinel
    // NSEvent so it returns and re-checks g_shutting_down.
    if (g_platform.wake_main_loop) g_platform.wake_main_loop();
}

static void signal_handler(int) {
    initiate_shutdown();
}

// =====================================================================
// Event bus
// =====================================================================

static EventQueue<MpvEvent> g_cef_queue;


static void publish(const MpvEvent& ev) {
    g_cef_queue.try_push(ev);
}

static void log_mpv_message(const mpv_event_log_message* msg) {
    switch (msg->log_level) {
    case MPV_LOG_LEVEL_FATAL:
    case MPV_LOG_LEVEL_ERROR:
        LOG_ERROR(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    case MPV_LOG_LEVEL_WARN:
        LOG_WARN(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    case MPV_LOG_LEVEL_INFO:
        LOG_INFO(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    case MPV_LOG_LEVEL_TRACE:
        LOG_TRACE(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    default: // V, DEBUG
        LOG_DEBUG(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    }
}

static void mpv_digest_thread() {
    while (!g_shutting_down.load(std::memory_order_relaxed)) {
        mpv_event* ev = g_mpv.WaitEvent(-1);
        if (ev->event_id == MPV_EVENT_NONE) continue;

        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            log_mpv_message(static_cast<mpv_event_log_message*>(ev->data));
            continue;
        }

        if (ev->event_id == MPV_EVENT_SHUTDOWN) {
            LOG_INFO(LOG_MAIN, "MPV_EVENT_SHUTDOWN received");
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
            else {
                fe.type = MpvEventType::END_FILE_ERROR;
                // mpv_error_string returns a pointer to a static, never-freed
                // string — safe to carry across threads via MpvEvent.
                fe.err_msg = mpv_error_string(d->error);
            }
            publish(fe);
            continue;
        }

        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* p = static_cast<mpv_event_property*>(ev->data);
            MpvEvent me = digest_property(ev->reply_userdata, p);
            if (me.type == MpvEventType::NONE) continue;
            if (me.type == MpvEventType::OSD_DIMS) {
                if (me.lw <= 0 || me.lh <= 0) continue;
                if (g_platform.in_transition())
                    g_platform.set_expected_size(me.pw, me.ph);
                g_platform.resize(me.lw, me.lh, me.pw, me.ph);
            }
            if (me.type == MpvEventType::FULLSCREEN) {
                g_platform.set_fullscreen(me.flag);
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
            if (!g_web_browser) continue;
            switch (ev.type) {
            case MpvEventType::PAUSE:
                g_playback_state = ev.flag ? PlaybackState::Paused : PlaybackState::Playing;
                update_idle_inhibit();
                g_web_browser->execJs(ev.flag ? "window._nativeEmit('paused')" : "window._nativeEmit('playing')");
                if (g_media_session)
                    g_media_session->setPlaybackState(ev.flag ? PlaybackState::Paused : PlaybackState::Playing);
                break;
            case MpvEventType::TIME_POS: {
                int ms = static_cast<int>(ev.dbl * 1000);
                g_web_browser->execJs("window._nativeUpdatePosition(" + std::to_string(ms) + ")");
                if (g_media_session)
                    g_media_session->setPosition(static_cast<int64_t>(ev.dbl * 1000000));
                break;
            }
            case MpvEventType::DURATION: {
                int ms = static_cast<int>(ev.dbl * 1000);
                g_web_browser->execJs("window._nativeUpdateDuration(" + std::to_string(ms) + ")");
                // Duration is set via metadata, not a separate call
                break;
            }
            case MpvEventType::FULLSCREEN:
                if (ev.flag) {
                    g_was_maximized_before_fullscreen = mpv::window_maximized();
                } else {
                    g_was_maximized_before_fullscreen = false;
                }
                g_web_browser->execJs("window._nativeFullscreenChanged(" + std::string(ev.flag ? "true" : "false") + ")");
                break;
            case MpvEventType::SPEED:
                g_web_browser->execJs("window._nativeSetRate(" + std::to_string(ev.dbl) + ")");
                if (g_media_session)
                    g_media_session->setRate(ev.dbl);
                break;
            case MpvEventType::SEEKING:
                if (ev.flag) {
                    g_web_browser->execJs("window._nativeEmit('seeking')");
                    if (g_media_session) g_media_session->emitSeeking();
                }
                break;
            case MpvEventType::FILE_LOADED:
                g_playback_state = PlaybackState::Playing;
                update_idle_inhibit();
                g_web_browser->execJs("window._nativeEmit('playing')");
                if (g_media_session)
                    g_media_session->setPlaybackState(PlaybackState::Playing);
                break;
            case MpvEventType::END_FILE_EOF:
                g_playback_state = PlaybackState::Stopped;
                update_idle_inhibit();
                g_web_browser->execJs("window._nativeEmit('finished')");
                if (g_media_session)
                    g_media_session->setPlaybackState(PlaybackState::Stopped);
                break;
            case MpvEventType::END_FILE_ERROR: {
                g_playback_state = PlaybackState::Stopped;
                update_idle_inhibit();
                auto val = CefValue::Create();
                val->SetString(ev.err_msg ? ev.err_msg : "Playback error");
                auto json = CefWriteJSON(val, JSON_WRITER_DEFAULT);
                g_web_browser->execJs("window._nativeEmit('error'," + json.ToString() + ")");
                if (g_media_session)
                    g_media_session->setPlaybackState(PlaybackState::Stopped);
                break;
            }
            case MpvEventType::END_FILE_CANCEL:
                g_playback_state = PlaybackState::Stopped;
                update_idle_inhibit();
                g_web_browser->execJs("window._nativeEmit('canceled')");
                if (g_media_session)
                    g_media_session->setPlaybackState(PlaybackState::Stopped);
                break;
            case MpvEventType::OSD_DIMS:
                if (g_web_browser->browser())
                    g_web_browser->resize(ev.lw, ev.lh, ev.pw, ev.ph);
                if (g_overlay_browser && g_overlay_browser->browser()) {
                    g_overlay_browser->resize(ev.lw, ev.lh, ev.pw, ev.ph);
                    g_platform.overlay_resize(ev.lw, ev.lh, ev.pw, ev.ph);
                }
                if (g_about_browser && g_about_browser->browser()) {
                    g_about_browser->resize(ev.lw, ev.lh, ev.pw, ev.ph);
                    g_platform.about_resize(ev.lw, ev.lh, ev.pw, ev.ph);
                }
                break;
            case MpvEventType::BUFFERED_RANGES: {
                auto list = CefListValue::Create();
                for (int i = 0; i < ev.range_count; i++) {
                    auto range = CefDictionaryValue::Create();
                    range->SetDouble("start", static_cast<double>(ev.ranges[i].start_ticks));
                    range->SetDouble("end", static_cast<double>(ev.ranges[i].end_ticks));
                    list->SetDictionary(static_cast<size_t>(i), range);
                }
                auto val = CefValue::Create();
                val->SetList(list);
                auto json = CefWriteJSON(val, JSON_WRITER_DEFAULT);
                g_web_browser->execJs("window._nativeUpdateBufferedRanges(" + json.ToString() + ")");
                break;
            }
            case MpvEventType::DISPLAY_FPS: {
                int hz = g_display_hz.load(std::memory_order_relaxed);
                LOG_INFO(LOG_MAIN, "Display refresh rate changed: {} Hz", hz);
                if (g_web_browser && g_web_browser->browser())
                    g_web_browser->browser()->GetHost()->SetWindowlessFrameRate(hz);
                if (g_overlay_browser && g_overlay_browser->browser())
                    g_overlay_browser->browser()->GetHost()->SetWindowlessFrameRate(hz);
                if (g_about_browser && g_about_browser->browser())
                    g_about_browser->browser()->GetHost()->SetWindowlessFrameRate(hz);
                break;
            }
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
    g_platform = make_platform(DisplayBackend::Windows);
#elif defined(__APPLE__)
    g_platform = make_platform(DisplayBackend::macOS);
#else
    // Linux: runtime detection, overridable with --platform.
    // Deferred to after CLI parsing — CEF subprocesses exit at
    // CefExecuteProcess before any platform use.
#endif

    if (int rc = CefRuntime::Start(argc, argv); rc >= 0) return rc;

    // --- Parse CLI ---
    std::string hwdec_str = kHwdecDefault;
    std::string audio_passthrough_str;
    bool audio_exclusive = false;
    std::string audio_channels_str;
    bool player_mode = false;
    bool disable_gpu_compositing = false;
    std::string ozone_platform;
    std::string platform_override;
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
                   "  --log-level <level>       trace|debug|info|warn|error\n"
                   "  --log-file <path>         Write logs to file ('' to disable)\n"
                   "  --hwdec <mode>            Hardware decoding mode (default: auto)\n"
                   "  --audio-passthrough <codecs>  e.g. ac3,dts-hd,eac3,truehd\n"
                   "  --audio-exclusive         Exclusive audio output\n"
                   "  --audio-channels <layout> e.g. stereo, 5.1, 7.1\n"
                   "  --remote-debug-port <port> Chrome remote debugging\n"
                   "  --disable-gpu-compositing Disable CEF GPU compositing\n"
                   "  --ozone-platform <plat>   CEF ozone platform (default: follows --platform)\n"
#ifdef HAVE_X11
                   "  --platform <wayland|x11>  Force display backend (Linux only)\n"
#endif
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
        } else if (strcmp(argv[i], "--disable-gpu-compositing") == 0) {
            disable_gpu_compositing = true;
        } else if (strcmp(argv[i], "--ozone-platform") == 0 && i + 1 < argc) {
            ozone_platform = argv[++i];
        } else if (strncmp(argv[i], "--ozone-platform=", 17) == 0) {
            ozone_platform = argv[i] + 17;
        } else if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            platform_override = argv[++i];
        } else if (strncmp(argv[i], "--platform=", 11) == 0) {
            platform_override = argv[i] + 11;
        } else if (strcmp(argv[i], "--player") == 0) {
            player_mode = true;
        } else if (argv[i][0] != '-') {
            player_playlist.push_back(argv[i]);
        }
    }

    if (!isValidHwdec(hwdec_str)) hwdec_str = kHwdecDefault;

    // --log-file overrides default; empty argument disables file logging entirely.
    // Default to a platform log file on macOS/Windows (GUI apps have no
    // user-visible stderr there). On Linux, stderr/journalctl is the norm,
    // so file logging is opt-in via --log-file.
    std::string log_path;
    if (log_file_path) {
        log_path = log_file_path;
    } else {
#if !defined(__linux__)
        log_path = paths::getLogPath();
#endif
    }
    int log_level = -1;
    if (log_level_str && log_level_str[0]) {
        log_level = parseLogLevel(log_level_str);
        if (log_level < 0) {
            fprintf(stderr, "Invalid log level: '%s' (expected trace|debug|info|warn|error)\n",
                    log_level_str);
            return 1;
        }
    }
    initLogging(log_path.c_str(), log_level);

    if (player_mode && player_playlist.empty()) {
        fprintf(stderr, "Error: --player requires at least one file or URL\n");
        return 1;
    }

    LOG_INFO(LOG_MAIN, "jellyfin-desktop " APP_VERSION_STRING);
    LOG_INFO(LOG_MAIN, "CEF {}", CEF_VERSION);
    if (!log_path.empty()) LOG_INFO(LOG_MAIN, "Log file: {}", log_path.c_str());

    // --- Linux platform selection ---
#if !defined(_WIN32) && !defined(__APPLE__)
    {
        DisplayBackend backend;
        if (platform_override == "wayland")
            backend = DisplayBackend::Wayland;
        else if (platform_override == "x11")
            backend = DisplayBackend::X11;
        else if (!platform_override.empty()) {
            fprintf(stderr, "Unknown platform: %s (expected wayland or x11)\n",
                    platform_override.c_str());
            return 1;
        } else {
            backend = (getenv("WAYLAND_DISPLAY") || !getenv("DISPLAY"))
                    ? DisplayBackend::Wayland : DisplayBackend::X11;
        }
#ifndef HAVE_X11
        if (backend == DisplayBackend::X11) {
            fprintf(stderr, "X11 detected but X11 support not compiled in\n");
            return 1;
        }
#endif
        g_platform = make_platform(backend);
    }
    LOG_INFO(LOG_MAIN, "Display backend: {}",
             g_platform.display == DisplayBackend::Wayland ? "wayland" : "x11");
#endif

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
    std::string mpv_home = paths::getMpvHome();
#ifdef _WIN32
    SetEnvironmentVariableA("MPV_HOME", mpv_home.c_str());
#else
    setenv("MPV_HOME", mpv_home.c_str(), 1);
#endif

    g_mpv = MpvHandle::Create(g_platform.display);
    if (!g_mpv.IsValid()) { LOG_ERROR(LOG_MAIN, "mpv_create failed"); return 1; }

    // libmpv defaults config=no (opposite of the mpv CLI); enable it so
    // users' $MPV_HOME/mpv.conf is loaded.
    g_mpv.SetOptionString("config", "yes");

    g_mpv.SetHwdec(hwdec_str);
    g_mpv.SetOptionString("background-color", kBgColor.hex);

    // Restore saved window geometry. mpv's --geometry is always physical
    // pixels (m_geometry_apply at third_party/mpv/options/m_option.c:2296
    // assigns gm->w/h to widw/widh without applying dpi_scale), so we pass
    // physical pixels here. If the live display scale differs from what
    // these pixels were computed against, the post-CEF-init resize block
    // below corrects the window size once display-hidpi-scale is known.
    {
        using WG = Settings::WindowGeometry;
        auto saved_geom = Settings::instance().windowGeometry();

        int w, h;
        if (saved_geom.width > 0 && saved_geom.height > 0) {
            w = saved_geom.width;
            h = saved_geom.height;
        } else {
            w = WG::kDefaultPhysicalWidth;
            h = WG::kDefaultPhysicalHeight;
        }

        int x = saved_geom.x, y = saved_geom.y;
        if (g_platform.clamp_window_geometry)
            g_platform.clamp_window_geometry(&w, &h, &x, &y);
        std::string geom_str = std::to_string(w) + "x" + std::to_string(h);
        if (x >= 0 && y >= 0) {
            geom_str += "+" + std::to_string(x) + "+" + std::to_string(y);
            g_mpv.SetOptionString("force-window-position", "yes");
        }
        g_mpv.SetOptionString("geometry", geom_str);
        if (saved_geom.maximized)
            g_mpv.SetOptionString("window-maximized", "yes");
    }

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
        g_mpv.SetAudioSpdif(audio_passthrough_str);
    }
    if (audio_exclusive)
        g_mpv.SetAudioExclusive(true);
    if (!audio_channels_str.empty())
        g_mpv.SetAudioChannels(audio_channels_str);

    // Register property observations before mpv_initialize. On macOS,
    // core_thread races to DispatchQueue.main.sync immediately after init
    // returns — main must enter the GCD pump loop without delay.
    g_mpv.SetWakeupCallback([](void*) {}, nullptr);
    observe_properties(g_mpv);

    // Load file if in player mode (before init so it's in the playlist)
    if (player_mode) {
        g_mpv.LoadFile(player_playlist[0], {});
    }

    int init_err = g_mpv.Initialize();
    if (init_err < 0) {
        LOG_ERROR(LOG_MAIN, "mpv_initialize failed: {}", init_err);
        g_mpv.TerminateDestroy();
        return 1;
    }
    g_mpv.RequestLogMessages("info");

    // input-default-bindings=no removes all builtin bindings including
    // CLOSE_WIN → quit.  Re-bind it so the WM close button works.
    {
        const char* cmd[] = {"keybind", "CLOSE_WIN", "quit", nullptr};
        mpv_command(g_mpv.Get(), cmd);
    }

    // --- Wait for VO (mpv needs a window before we can get platform handles) ---
    // Both loops wait for an osd-dimensions property-change event carrying
    // positive w/h, captured into mw/mh via mpv::read_osd_dims_from_event.
    // That's a struct read of the event payload — no mpv_get_property call —
    // so it's safe on macOS's main thread during VO init, where a synchronous
    // property read can deadlock against core_thread's DispatchQueue.main.sync.
    LOG_INFO(LOG_MAIN, "Waiting for mpv window...");
    int64_t mw = 0, mh = 0;
    // Route every PROPERTY_CHANGE through digest_property, not just osd-dims.
    // Side effect: seeds the atomics (s_osd_pw/ph, s_fullscreen,
    // s_display_scale, ...) as mpv fires initial-value events, so platform
    // init code can read them instead of issuing sync mpv_get_property calls.
    // Caller breaks out of the loop only on a valid osd-dimensions event.
    auto try_consume_osd_dims = [&](mpv_event* ev) -> bool {
        if (ev->event_id != MPV_EVENT_PROPERTY_CHANGE) return false;
        MpvEvent me = digest_property(
            ev->reply_userdata, static_cast<mpv_event_property*>(ev->data));
        if (me.type != MpvEventType::OSD_DIMS || me.pw <= 0 || me.ph <= 0)
            return false;
        mw = me.pw;
        mh = me.ph;
        return true;
    };

#ifdef __APPLE__
    while (true) {
        g_platform.pump();
        mpv_event* ev = g_mpv.WaitEvent(0);
        if (ev->event_id == MPV_EVENT_NONE) { usleep(10000); continue; }
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            log_mpv_message(static_cast<mpv_event_log_message*>(ev->data));
            continue;
        }
        if (ev->event_id == MPV_EVENT_SHUTDOWN || ev->event_id == MPV_EVENT_END_FILE) {
            g_mpv.TerminateDestroy(); return 0;
        }
        if (try_consume_osd_dims(ev)) break;
    }
#else
    while (true) {
        mpv_event* ev = g_mpv.WaitEvent(1.0);
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            log_mpv_message(static_cast<mpv_event_log_message*>(ev->data));
            continue;
        }
        if (ev->event_id == MPV_EVENT_SHUTDOWN) { g_mpv.TerminateDestroy(); return 0; }
        if (ev->event_id == MPV_EVENT_END_FILE) { g_mpv.TerminateDestroy(); return 0; }
        if (try_consume_osd_dims(ev)) break;
    }
#endif

    // --- Platform init ---
    // Resolve effective ozone platform so CEF clients can check it.
#if !defined(_WIN32) && !defined(__APPLE__)
    if (ozone_platform.empty())
        ozone_platform = g_platform.display == DisplayBackend::Wayland ? "wayland" : "x11";
#endif
    g_platform.cef_ozone_platform = ozone_platform;
    if (!g_platform.init(g_mpv.Get())) {
        LOG_ERROR(LOG_MAIN, "Platform init failed");
        g_mpv.TerminateDestroy();
        return 1;
    }
    LOG_INFO(LOG_MAIN, "Platform init ok");

    // --- CEF init ---
    bool use_shared_textures = g_platform.shared_texture_supported && !disable_gpu_compositing;

    CefRuntime::SetLogSeverity(toCefSeverity(log_level));
    CefRuntime::SetRemoteDebuggingPort(remote_debugging_port);
    CefRuntime::SetDisableGpuCompositing(!use_shared_textures);
#ifdef __linux__
    if (!ozone_platform.empty())
        CefRuntime::SetOzonePlatform(ozone_platform);
#endif

    LOG_INFO(LOG_MAIN, "[FLOW] calling CefInitialize...");
    if (!CefRuntime::Initialize()) {
        LOG_ERROR(LOG_MAIN, "CefInitialize failed");
        g_platform.cleanup();
        g_mpv.TerminateDestroy();
        return 1;
    }
    LOG_INFO(LOG_MAIN, "[FLOW] CefInitialize returned ok");

    double display_hidpi_scale = 0.0;
    mpv_get_property(g_mpv.Get(), "display-hidpi-scale",
                     MPV_FORMAT_DOUBLE, &display_hidpi_scale);
    int fs_flag = 0;
    mpv_get_property(g_mpv.Get(), "fullscreen", MPV_FORMAT_FLAG, &fs_flag);
    LOG_INFO(LOG_MAIN, "[FLOW] display-hidpi-scale={} fullscreen={}",
             display_hidpi_scale, fs_flag);

    // If the live display-hidpi-scale differs from the saved scale, the
    // pixels we passed to --geometry were sized for the wrong scale.
    // Resize using the saved logical × the live scale.
    //
    // When the compositor has forced fullscreen, still issue SetGeometry so
    // mpv's stored unfullscreen size (wl->window_size) is scale-corrected for
    // the eventual restore, but don't overwrite mw/mh — the fullscreen surface
    // size is authoritative for browser creation.
    {
        using WG = Settings::WindowGeometry;
        const auto& saved = Settings::instance().windowGeometry();
        float saved_scale = saved.scale > 0.f ? saved.scale : WG::kDefaultScale;
        int logical_w = saved.logical_width  > 0 ? saved.logical_width
                                                 : WG::kDefaultLogicalWidth;
        int logical_h = saved.logical_height > 0 ? saved.logical_height
                                                 : WG::kDefaultLogicalHeight;
        if (display_hidpi_scale > 0.0 &&
            std::fabs(display_hidpi_scale - saved_scale) >= 0.01) {
            int new_pw = static_cast<int>(
                std::lround(logical_w * display_hidpi_scale));
            int new_ph = static_cast<int>(
                std::lround(logical_h * display_hidpi_scale));
            std::string geom_str = std::to_string(new_pw) + "x"
                                 + std::to_string(new_ph);
            LOG_INFO(LOG_MAIN,
                     "[FLOW] scale {:.3f} -> {:.3f}, resize to {}",
                     saved_scale, display_hidpi_scale, geom_str.c_str());
            g_mpv.SetGeometry(geom_str);
            if (!fs_flag) {
                mw = new_pw;
                mh = new_ph;
            }
        }
        mpv::set_window_pixels(static_cast<int>(mw), static_cast<int>(mh));
    }

    // --- Create browsers ---
    float scale = display_hidpi_scale > 0.0
        ? static_cast<float>(display_hidpi_scale)
        : g_platform.get_scale();
    int lw = static_cast<int>(mw / scale);
    int lh = static_cast<int>(mh / scale);

    CefWindowInfo wi;
    wi.SetAsWindowless(0);
    wi.shared_texture_enabled = use_shared_textures;
#ifdef __APPLE__
    // macOS: drive BeginFrames from CVDisplayLink, aligned with display
    // vsync. Eliminates the phase lag from CEF's internal 60Hz BeginFrame
    // timer. See platform/macos.mm:g_display_link setup.
    wi.external_begin_frame_enabled = true;
#else
    wi.external_begin_frame_enabled = false;
#endif
    CefBrowserSettings bs;
    bs.background_color = 0;
    bs.windowless_frame_rate = g_display_hz.load(std::memory_order_relaxed);

    // Must exist before we create the main browser: the pre-loaded page fires
    // its initial theme-color IPC at DOMContentLoaded, and we need to capture
    // it so onOverlayDismissed has a color to apply.
    TitlebarColor titlebar_color_obj(g_platform, Settings::instance().titlebarThemeColor());
    g_titlebar_color = &titlebar_color_obj;

    // Main browser
    RenderTarget main_target{g_platform.present, g_platform.present_software};
    g_web_browser = new WebBrowser(main_target, lw, lh, (int)mw, (int)mh);
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
        // Eager pre-load: begin fetching the saved server while the overlay
        // probes in parallel. The overlay fades out on success, revealing the
        // already-loaded page.
        main_url = server_url;
    }
    // else: main browser starts blank; the overlay handles server selection.

    LOG_INFO(LOG_MAIN, "[FLOW] CreateBrowser(main) url={} lw={} lh={} pw={} ph={}",
             main_url.c_str(), lw, lh, (long long)mw, (long long)mh);
    g_web_browser->create(wi, bs, main_url);
    LOG_INFO(LOG_MAIN, "[FLOW] CreateBrowser(main) call returned");

    // Overlay browser (server selection UI) -- only in full app mode
    if (!player_mode) {
        RenderTarget overlay_target{g_platform.overlay_present, g_platform.overlay_present_software};
        g_overlay_browser = new OverlayBrowser(overlay_target, *g_web_browser,
                                               lw, lh, (int)mw, (int)mh);
        g_platform.set_overlay_visible(true);

        CefWindowInfo owi;
        owi.SetAsWindowless(0);
        owi.shared_texture_enabled = use_shared_textures;
#ifdef __APPLE__
        owi.external_begin_frame_enabled = true;
#else
        owi.external_begin_frame_enabled = false;
#endif
        CefBrowserSettings obs;
        obs.background_color = 0;
        obs.windowless_frame_rate = g_display_hz.load(std::memory_order_relaxed);
        LOG_INFO(LOG_MAIN, "[FLOW] CreateBrowser(overlay)");
        CefBrowserHost::CreateBrowser(owi, g_overlay_browser->client(), "app://resources/overlay.html", obs,
                                      OverlayBrowser::injectionProfile(), nullptr);
        LOG_INFO(LOG_MAIN, "[FLOW] CreateBrowser(overlay) call returned");
    }

    auto media_session_obj = MediaSession::create();

    MediaSessionThread media_session_thread;
    media_session_thread.start(media_session_obj.get());
    g_media_session = &media_session_thread;

    // --- Start threads ---
    // Start before waitForLoad so mpv events (OSD_DIMS in particular) drain
    // into the platform and browsers even while we're still sitting on a
    // blank main browser. Without this, the overlay-only startup path never
    // sees resize/fullscreen events until the user picks a server and the
    // main browser finishes its first load.
    LOG_INFO(LOG_MAIN, "[FLOW] starting digest + cef_consumer threads");
    std::thread digest_thread(mpv_digest_thread);
    std::thread cef_thread(cef_consumer_thread);

#ifdef __APPLE__
    // nothing — main thread pump happens below
#else
    g_web_browser->waitForLoad();
#endif
    LOG_INFO(LOG_MAIN, "Main browser loaded");

    LOG_INFO(LOG_MAIN, "[FLOW] Running — about to enter run_main_loop");

    // --- Wait for shutdown ---
#ifdef __APPLE__
    // macOS: block on the NSApplication run loop. Returns when
    // initiate_shutdown calls wake_main_loop ([NSApp stop:nil] from main).
    // Everything that needs the main thread fires from inside this call,
    // event-driven, no polling:
    //   - NSEvents → [NSApp sendEvent:]
    //   - GCD main-queue blocks: mpv VO's DispatchQueue.main.sync, and
    //     CEF pump work enqueued by App::OnScheduleMessagePumpWork via
    //     dispatch_async_f / dispatch_after_f
    g_platform.run_main_loop();
    LOG_INFO(LOG_MAIN, "[FLOW] run_main_loop returned — entering post-run drain");

    // After shutdown breaks the main run loop, CEF may still have browser-
    // close work in flight (IO/Network cleanup posting back to the UI
    // thread). Keep the pump alive and spin the runloop until both browsers
    // report closed; dispatch blocks queued by OnScheduleMessagePumpWork
    // keep running CefDoMessageLoopWork as CEF posts new tasks. Event-
    // driven — CFRunLoopRunInMode wakes on any source firing.
    while (!g_web_browser->isClosed() ||
           (g_overlay_browser && !g_overlay_browser->isClosed()) ||
           (g_about_browser && !g_about_browser->isClosed())) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 60.0, true);
    }

#else
    g_web_browser->waitForClose();
    if (g_overlay_browser)
        g_overlay_browser->waitForClose();
#endif

    // --- Cleanup ---
    // Stop our threads first (they don't depend on CEF/mpv shutdown order)
    g_media_session = nullptr;
    g_titlebar_color = nullptr;
    media_session_thread.stop();
    g_platform.set_idle_inhibit(IdleInhibitLevel::None);

    cef_thread.join();
    g_mpv.Wakeup();
    digest_thread.join();

    // Save window geometry while mpv is still alive.
    {
        bool fs  = mpv::fullscreen();
        bool max = mpv::window_maximized();

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
            // Normal windowed: save current size and position.
            // Capture {pixel, logical, scale} so the next launch can restore
            // losslessly on the same display, or rescale correctly when moved
            // to a display with a different DPI.
            // Prefer the effective pixel size we recorded during boot so
            // save reflects what we asked mpv for, even if osd-dimensions
            // lags behind a resize we issued.
            int pw = mpv::window_pw();
            int ph = mpv::window_ph();
            if (pw <= 0 || ph <= 0) {
                pw = mpv::osd_pw();
                ph = mpv::osd_ph();
            }
            if (pw > 0 && ph > 0) {
                Settings::WindowGeometry geom;
                geom.width = pw;
                geom.height = ph;

                float scale = g_platform.get_scale ? g_platform.get_scale() : 1.0f;
                if (scale <= 0.f) scale = 1.0f;
                geom.scale = scale;
                geom.logical_width  = static_cast<int>(std::lround(pw / scale));
                geom.logical_height = static_cast<int>(std::lround(ph / scale));

                geom.maximized = false;
                int wx, wy;
                if (g_platform.query_window_position &&
                    g_platform.query_window_position(&wx, &wy)) {
                    geom.x = wx;
                    geom.y = wy;
                }
                Settings::instance().setWindowGeometry(geom);
            }
        }
        Settings::instance().save();
    }

    // CEF shutdown: all browsers must be closed first (guaranteed by waitForClose above)
    delete g_web_browser; g_web_browser = nullptr;
    delete g_overlay_browser; g_overlay_browser = nullptr;
    // g_about_browser is normally self-deleted via its BeforeCloseCallback.
    // If shutdown races the callback (unlikely), we take responsibility here.
    if (g_about_browser) { delete g_about_browser; g_about_browser = nullptr; }
    CefRuntime::Shutdown();

    // Platform cleanup (joins input thread, destroys subsurfaces)
    // Must happen after CefShutdown (CEF may still present during shutdown)
    // but before mpv_terminate_destroy (mpv destroys the parent surface)
    g_platform.cleanup();

#ifdef __APPLE__
    // mpv's macOS VO uninit (mac_common.swift:84) calls
    // DispatchQueue.main.sync to close its window. That blocks the VO
    // thread until the main thread services the GCD block. If we call
    // mpv_terminate_destroy synchronously here, the main thread is
    // blocked waiting for mpv to finish while mpv is blocked waiting for
    // the main thread — classic deadlock.
    //
    // Fix: run mpv teardown on a separate thread and keep the main
    // thread pumping the runloop so GCD blocks dispatch normally.
    // mpv's VO teardown needs the main thread for two reasons:
    //   1. DispatchQueue.main.sync in mac_common.swift:84 (window close)
    //   2. PreciseTimer.terminate() sends pthread_kill(SIGALRM) — CefInitialize
    //      reset SIGALRM to SIG_DFL (content_main.cc:108), so restore a no-op
    //      handler before mpv tears down the timer.
    //
    // CFRunLoopSourceSignal latches: if the teardown thread finishes before
    // CFRunLoopRun enters, the signal is pending and fires immediately on
    // entry — no race with CFRunLoopStop (which is a no-op if the runloop
    // isn't running yet).
    // mpv's VO teardown (mac_common.swift:84) uses DispatchQueue.main.sync
    // to close its window — the main thread must service GCD blocks.
    // Loop CFRunLoopRunInMode until mpv is done (same pattern as Chromium's
    // MessagePumpCFRunLoop::DoRun in message_pump_apple.mm:676-680).
    // Each iteration blocks until a source fires (event-driven); we check
    // the done flag after each wake.
    std::atomic<bool> mpv_done{false};
    std::thread mpv_teardown([&mpv_done]{
        // CefInitialize reset SIGALRM to SIG_DFL (content_main.cc:108).
        // mpv's PreciseTimer.terminate() sends pthread_kill(SIGALRM) to
        // wake its timer thread — restore a no-op handler so the default
        // action (terminate process) doesn't fire.
        signal(SIGALRM, [](int){});
        g_mpv.TerminateDestroy();
        mpv_done.store(true, std::memory_order_release);
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
    while (!mpv_done.load(std::memory_order_acquire))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,
                           std::numeric_limits<CFTimeInterval>::max(), true);
    mpv_teardown.join();
#else
    g_mpv.TerminateDestroy();
#endif

    shutdownLogging();
    return 0;
}
