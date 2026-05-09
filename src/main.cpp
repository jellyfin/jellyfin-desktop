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
#include "mpv/capabilities.h"
#include "jellyfin/device_profile.h"
#include "paths/paths.h"
#include "settings.h"
#include "theme_color.h"

#include "player/media_session.h"
#include "player/media_session_thread.h"
#include "playback/coordinator.h"
#include "playback/sinks.h"

#include "logging.h"
#include "signal_guard.h"
#include "shutdown.h"
#include "event_dispatcher.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <signal.h>
#else
#include "single_instance.h"
#endif

#include "include/cef_version.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <memory>
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
Color g_video_bg;

PlaybackCoordinator* g_playback_coord = nullptr;
ThemeColor* g_theme_color = nullptr;
std::atomic<int> g_display_hz{60};

Platform g_platform{};
WebBrowser* g_web_browser = nullptr;
OverlayBrowser* g_overlay_browser = nullptr;

static void log_mpv_message(const mpv_event_log_message* msg) {
    switch (msg->log_level) {
    case MPV_LOG_LEVEL_FATAL:
    case MPV_LOG_LEVEL_ERROR:
        LOG_ERROR(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    case MPV_LOG_LEVEL_WARN:
        LOG_WARN(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    case MPV_LOG_LEVEL_INFO:
        LOG_INFO(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    case MPV_LOG_LEVEL_V:
        LOG_DEBUG(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    case MPV_LOG_LEVEL_DEBUG:
        LOG_TRACE(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    default: // unexpected (e.g. TRACE — we cap subscription at debug) or new mpv level
        LOG_WARN(LOG_MPV, "[unhandled mpv level {}] {}: {}",
                 (int)msg->log_level, msg->prefix, msg->text); break;
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

MediaSessionThread* g_media_session = nullptr;

// Shutdown order (reverse of declaration):
//   browsers → CefShutdown → idle_inhibit clear → platform.cleanup
// then main runs mpv terminate + post_window_cleanup.
static int run_with_cef(int mw, int mh,
                        std::string ozone_platform,
                        bool disable_gpu_compositing,
                        int remote_debugging_port,
                        LogLevel log_level) {
#if !defined(_WIN32) && !defined(__APPLE__)
    if (ozone_platform.empty())
        ozone_platform = g_platform.display == DisplayBackend::Wayland ? "wayland" : "x11";
#endif
    g_platform.cef_ozone_platform = ozone_platform;
    PlatformScope platform_scope(g_platform, g_mpv.Get());
    if (!platform_scope.ok()) {
        LOG_ERROR(LOG_MAIN, "Platform init failed");
        return 1;
    }
    LOG_INFO(LOG_MAIN, "Platform init ok");

    IdleInhibitGuard idle_inhibit_guard;

    // Apply titlebar color before CefInitialize so the window doesn't sit
    // with the system default palette for the whole CEF init duration.
    if (Settings::instance().titlebarThemeColor())
        g_platform.set_theme_color(kBgColor);

    // Must run after the VO-init wait loop — sync mpv API calls would
    // deadlock against core_thread's DispatchQueue.main.sync on macOS.
    {
        auto caps = mpv_capabilities::Query(g_mpv.Get());
        jellyfin_device_profile::SetCachedJson(jellyfin_device_profile::Build(
            caps, "Jellyfin Desktop", APP_VERSION_FULL,
            Settings::instance().forceTranscoding()));
    }

    bool use_shared_textures = g_platform.shared_texture_supported && !disable_gpu_compositing;

    CefRuntime::SetLogSeverity(toCefSeverity(log_level));
    CefRuntime::SetRemoteDebuggingPort(remote_debugging_port);
    CefRuntime::SetDisableGpuCompositing(!use_shared_textures);
#ifdef __linux__
    if (!ozone_platform.empty())
        CefRuntime::SetOzonePlatform(ozone_platform);
#endif

    LOG_INFO(LOG_MAIN, "[FLOW] calling CefInitialize...");
    CefRuntimeScope cef_scope;
    if (!cef_scope.ok()) {
        LOG_ERROR(LOG_MAIN, "CefInitialize failed");
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
        mpv::set_window_pixels(mw, mh);
    }

    float scale = display_hidpi_scale > 0.0
        ? static_cast<float>(display_hidpi_scale)
        : g_platform.get_scale();
    int lw = static_cast<int>(mw / scale);
    int lh = static_cast<int>(mh / scale);

    CefWindowInfo wi;
    wi.SetAsWindowless(0);
    wi.shared_texture_enabled = use_shared_textures;
#ifdef __APPLE__
    // Drive BeginFrames from CVDisplayLink (platform/macos.mm:g_display_link)
    // to eliminate phase lag against CEF's internal 60Hz timer.
    wi.external_begin_frame_enabled = true;
#else
    wi.external_begin_frame_enabled = false;
#endif
    CefBrowserSettings bs;
    bs.background_color = 0;
    bs.windowless_frame_rate = g_display_hz.load(std::memory_order_relaxed);

    // Must exist before main browser creation: the pre-loaded page fires
    // its initial theme-color IPC at DOMContentLoaded; onOverlayDismissed
    // needs a color already captured.
    bool titlebar_themed = Settings::instance().titlebarThemeColor();
    ThemeColor theme_color_obj([titlebar_themed](const Color& c) {
        if (titlebar_themed) g_platform.set_theme_color(c);
        g_mpv.SetBackgroundColor(c);
    });
    g_theme_color = &theme_color_obj;

    RenderTarget main_target{g_platform.present, g_platform.present_software};
    auto web_browser_owner = std::make_unique<WebBrowser>(main_target, lw, lh, mw, mh);
    g_web_browser = web_browser_owner.get();
    g_platform.resize(lw, lh, mw, mh);

    std::string server_url = Settings::instance().serverUrl();
    std::string main_url;
    // Eager pre-load: fetch the saved server while the overlay probes in
    // parallel. The overlay fades out on success, revealing the loaded page.
    if (!server_url.empty())
        main_url = server_url;

    LOG_INFO(LOG_MAIN, "[FLOW] CreateBrowser(main) url={} lw={} lh={} pw={} ph={}",
             main_url.c_str(), lw, lh, mw, mh);
    g_web_browser->create(wi, bs, main_url);
    LOG_INFO(LOG_MAIN, "[FLOW] CreateBrowser(main) call returned");

    std::unique_ptr<OverlayBrowser> overlay_browser_owner;
    {
        RenderTarget overlay_target{g_platform.overlay_present, g_platform.overlay_present_software};
        overlay_browser_owner = std::make_unique<OverlayBrowser>(
            overlay_target, *g_web_browser, lw, lh, mw, mh);
        g_overlay_browser = overlay_browser_owner.get();
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

    // Coordinator + sinks must exist before any thread can post inputs or
    // observe playback state. Sinks register before start() so the worker
    // never delivers to a half-built fanout.
    PlaybackCoordinatorScope coord_scope;
    g_playback_coord = &coord_scope.coord();
    auto browser_sink = std::make_shared<BrowserPlaybackSink>();
    auto idle_inhibit_sink = std::make_shared<IdleInhibitSink>();
    auto theme_color_sink = std::make_shared<ThemeColorSink>();
    auto mpv_action_sink = std::make_shared<MpvActionSink>();
    coord_scope.coord().addSink(browser_sink);
    coord_scope.coord().addSink(idle_inhibit_sink);
    coord_scope.coord().addSink(theme_color_sink);
    coord_scope.coord().addSink(
        std::make_shared<MediaSessionPlaybackSink>(&media_session_thread));
    coord_scope.coord().addActionSink(mpv_action_sink);
    register_cef_thread_sinks(
        {browser_sink, idle_inhibit_sink, theme_color_sink},
        {mpv_action_sink});

    // Start before waitForLoad so mpv events (OSD_DIMS especially) reach
    // the platform/browsers during the overlay-only startup phase, before
    // the main browser finishes loading.
    LOG_INFO(LOG_MAIN, "[FLOW] starting digest + cef_consumer threads");
    std::thread digest_thread(mpv_digest_thread);
    std::thread cef_thread(cef_consumer_thread);

#ifndef __APPLE__
    g_web_browser->waitForLoad();
#endif
    LOG_INFO(LOG_MAIN, "Main browser loaded");

    LOG_INFO(LOG_MAIN, "[FLOW] Running — about to enter run_main_loop");

#ifdef __APPLE__
    // Block on the NSApplication run loop until initiate_shutdown calls
    // wake_main_loop. Services NSEvents and GCD main-queue blocks (mpv VO
    // DispatchQueue.main.sync, CEF App::OnScheduleMessagePumpWork).
    g_platform.run_main_loop();
    LOG_INFO(LOG_MAIN, "[FLOW] run_main_loop returned — entering post-run drain");

    // CEF may still have browser-close work in flight after the main loop
    // breaks. Spin the runloop event-driven until all browsers report closed.
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

    g_media_session = nullptr;
    g_theme_color = nullptr;
    media_session_thread.stop();

    cef_thread.join();
    g_mpv.Wakeup();
    digest_thread.join();

    // Producers have joined; coordinator drains any in-flight inputs and
    // stops via PlaybackCoordinatorScope dtor at end of scope. Clear the
    // global pointer first so any late readers see "no coordinator".
    g_playback_coord = nullptr;

    // Save window geometry while mpv is still alive.
    {
        bool fs  = mpv::fullscreen();
        bool max = mpv::window_maximized();

        if (fs) {
            // Don't overwrite the saved windowed size with fullscreen dims;
            // only update the maximized flag for the eventual restore.
            auto geom = Settings::instance().windowGeometry();
            geom.maximized = g_was_maximized_before_fullscreen;
            Settings::instance().setWindowGeometry(geom);
        } else if (max) {
            // Don't overwrite the saved windowed size with monitor dims;
            // on next launch the window opens maximized and unmaximize
            // restores the preserved size.
            auto geom = Settings::instance().windowGeometry();
            geom.maximized = true;
            Settings::instance().setWindowGeometry(geom);
        } else {
            // Capture {pixel, logical, scale} so the next launch can
            // restore losslessly on the same display, or rescale on a
            // display with different DPI. Prefer window_pw/ph (set at boot)
            // over osd_pw/ph which may lag a resize we just issued.
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

                float win_scale = g_platform.get_scale ? g_platform.get_scale() : 1.0f;
                if (win_scale <= 0.f) win_scale = 1.0f;
                geom.scale = win_scale;
                geom.logical_width  = static_cast<int>(std::lround(pw / win_scale));
                geom.logical_height = static_cast<int>(std::lround(ph / win_scale));

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

    // Browsers must be deleted before CefShutdown (waitForClose above
    // guarantees they're closed at the CEF level).
    g_web_browser = nullptr;
    web_browser_owner.reset();
    g_overlay_browser = nullptr;
    overlay_browser_owner.reset();
    // g_about_browser self-deletes via BeforeCloseCallback; cover the
    // shutdown race where we got here before the callback ran.
    if (g_about_browser) { delete g_about_browser; g_about_browser = nullptr; }

    return 0;
}

// =====================================================================
// Main
// =====================================================================

int main(int argc, char* argv[]) {
    // CEF subprocesses (GPU, renderer) re-execute this binary; they must
    // hit CefExecuteProcess immediately, before CLI parsing or anything
    // else touches shared state. Linux platform selection is deferred
    // until after CLI parsing.
#ifdef _WIN32
    g_platform = make_platform(DisplayBackend::Windows);
#elif defined(__APPLE__)
    g_platform = make_platform(DisplayBackend::macOS);
#endif

    if (int rc = CefRuntime::Start(argc, argv); rc >= 0) return rc;

    std::string hwdec_str = kHwdecDefault;
    std::string audio_passthrough_str;
    bool audio_exclusive = false;
    std::string audio_channels_str;
    bool disable_gpu_compositing = false;
    std::string ozone_platform;
    std::string platform_override;
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
                   "\nOptions:\n"
                   "  -h, --help                Show this help\n"
                   "  -v, --version             Show version\n"
                   "  --log-level <level>       trace|debug|info|warn|error (default: %s)\n"
                   "  --log-file <path>         Write logs to file ('' to disable)\n"
                   "  --hwdec <mode>            Hardware decoding mode (default: %s)\n"
                   "  --audio-passthrough <codecs>  e.g. ac3,dts-hd,eac3,truehd\n"
                   "  --audio-exclusive         Exclusive audio output\n"
                   "  --audio-channels <layout> e.g. stereo, 5.1, 7.1\n"
                   "  --remote-debug-port <port> Chrome remote debugging\n"
                   "  --disable-gpu-compositing Disable CEF GPU compositing\n"
                   "  --ozone-platform <plat>   CEF ozone platform (default: follows --platform)\n"
#ifdef HAVE_X11
                   "  --platform <wayland|x11>  Force display backend (Linux only)\n"
#endif
                   ,
                   kDefaultLogLevelName, kHwdecDefault);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("jellyfin-desktop %s\n\nCEF %s\n\n", APP_VERSION_FULL, CEF_VERSION);
            mpv_handle* h = mpv_create();
            if (h && mpv_initialize(h) >= 0) {
                for (const char* prop : {"mpv-version", "ffmpeg-version"}) {
                    char* v = mpv_get_property_string(h, prop);
                    if (v) {
                        printf("%s %s\n", prop, v);
                        mpv_free(v);
                    }
                }
            }
            if (h) mpv_terminate_destroy(h);
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
        } else {
            fprintf(stderr, "Error: unknown argument '%s'\n", argv[i]);
            return 1;
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
    LogLevel log_level = LogLevel::Default;
    if (log_level_str && log_level_str[0]) {
        log_level = parseLogLevel(log_level_str);
        if (log_level == LogLevel::Default) {
            fprintf(stderr, "Invalid log level: '%s' (expected trace|debug|info|warn|error)\n",
                    log_level_str);
            return 1;
        }
    }
    LoggingScope logging(log_path.c_str(), log_level);

    LOG_INFO(LOG_MAIN, "jellyfin-desktop " APP_VERSION_FULL);
    LOG_INFO(LOG_MAIN, "CEF {}", CEF_VERSION);
    if (!log_path.empty()) LOG_INFO(LOG_MAIN, "Log file: {}", log_path.c_str());

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

#ifdef _WIN32
    SetConsoleCtrlHandler([](DWORD) -> BOOL {
        initiate_shutdown();
        return TRUE;
    }, TRUE);
#else
    SignalHandlerGuard signal_guard(signal_handler);
#endif

#ifndef __APPLE__
    if (trySignalExisting()) {
        LOG_INFO(LOG_MAIN, "Signaled existing instance, exiting");
        return 0;
    }
    startListener([](const std::string&) {
        // TODO: raise window via xdg-activation
    });
    // Joins the listener thread on any exit path (a joinable std::thread
    // calls std::terminate from its destructor).
    struct ListenerGuard { ~ListenerGuard() { stopListener(); } } listener_guard;
#endif

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

    // We only ever feed mpv direct media URLs from the Jellyfin server;
    // the youtube-dl/yt-dlp hook would just add startup latency and a
    // failure mode for nothing.
    g_mpv.SetOptionString("ytdl", "no");

    g_mpv.SetOptionString("user-agent", APP_USER_AGENT);

    g_mpv.SetHwdec(hwdec_str);

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

    int init_err = g_mpv.Initialize();
    if (init_err < 0) {
        LOG_ERROR(LOG_MAIN, "mpv_initialize failed: {}", init_err);
        return 1;
    }
    g_mpv.SetLogLevel(log_level);

    // Capture user's mpv.conf bg, then force startup color. Safe here:
    // force-window=yes (not "immediate") defers VO creation, so the user's
    // color never flashes before we override.
    g_video_bg = g_mpv.GetBackgroundColor();
    LOG_INFO(LOG_MAIN, "video bg captured: {}", g_video_bg.hex);
    g_mpv.SetBackgroundColor(kBgColor);

    for (const char* prop : {"mpv-version", "ffmpeg-version"})
        LOG_INFO(LOG_MAIN, "{} {}", prop, g_mpv.GetPropertyString(prop));

    // input-default-bindings=no removes all builtin bindings including
    // CLOSE_WIN → quit.  Re-bind it so the WM close button works.
    {
        const char* cmd[] = {"keybind", "CLOSE_WIN", "quit", nullptr};
        mpv_command(g_mpv.Get(), cmd);
    }

    // Wait for the VO window. Reads osd-dimensions from the event payload
    // (no sync mpv_get_property call) so it stays safe against a
    // DispatchQueue.main.sync deadlock against core_thread on macOS.
    LOG_INFO(LOG_MAIN, "Waiting for mpv window...");
    int64_t mw = 0, mh = 0;
    // Route every PROPERTY_CHANGE through digest_property — this seeds the
    // s_osd_pw/ph, s_fullscreen, s_display_scale atomics from mpv's initial-
    // value events so platform init can read them without sync API calls.
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
            return 0;
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
        if (ev->event_id == MPV_EVENT_SHUTDOWN) return 0;
        if (ev->event_id == MPV_EVENT_END_FILE) return 0;
        if (try_consume_osd_dims(ev)) break;
    }
#endif

    int rc = run_with_cef(static_cast<int>(mw), static_cast<int>(mh),
                          ozone_platform, disable_gpu_compositing,
                          remote_debugging_port, log_level);
    if (rc != 0) return rc;

#ifdef __APPLE__
    // mpv's VO uninit (mac_common.swift:84) does DispatchQueue.main.sync
    // to close its window — calling TerminateDestroy from the main thread
    // would deadlock. Run it on a side thread and pump the runloop here
    // (same pattern as Chromium's MessagePumpCFRunLoop::DoRun).
    std::atomic<bool> mpv_done{false};
    std::thread mpv_teardown([&mpv_done]{
        // CefInitialize reset SIGALRM to SIG_DFL (content_main.cc:108);
        // mpv's PreciseTimer.terminate() sends pthread_kill(SIGALRM), so
        // restore a no-op handler before tearing down the timer.
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

    if (g_platform.post_window_cleanup)
        g_platform.post_window_cleanup();

    return 0;
}
