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
#include "cli.h"
#include "common.h"
#include "cef/cef_app.h"
#include "cef/cef_client.h"
extern "C" void jfn_overlay_init(JfnCefLayer* main_layer);
extern "C" void jfn_web_init(JfnCefLayer* layer);
extern "C" void jfn_web_exec_js(const char* js_utf8);

// Rust Browsers registry C ABI (src/jfn_cef/src/browsers.rs).
extern "C" void jfn_browsers_init(int lw, int lh, int pw, int ph,
                                  double frame_rate, bool use_shared_textures);
extern "C" void jfn_browsers_shutdown(void);
extern "C" JfnCefLayer* jfn_browsers_create(const char* kind);
extern "C" void jfn_browsers_close_all(void);
extern "C" void jfn_browsers_wait_all_closed(void);
extern "C" bool jfn_browsers_all_closed(void);
extern "C" void jfn_browsers_set_size(int lw, int lh, int pw, int ph);
extern "C" void jfn_browsers_set_scale(double scale);
extern "C" void jfn_browsers_set_refresh_rate(double hz);

#include "mpv/jfn_mpv_api.h"
#include "mpv/jfn_mpv_boot.h"
#include "jellyfin/device_profile.h"
#include "paths/paths.h"
#include "settings.h"
#include "theme_color.h"

#include "playback/coordinator.h"
#if defined(__APPLE__)
#include "playback/sinks/macos/macos_sink.h"
#elif defined(_WIN32)
#include "playback/sinks/windows/windows_sink.h"
#endif

#include "logging.h"
#include "signal_guard.h"
#include "playback/jfn_ingest.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <signal.h>
#include "platform/macos_platform.h"
#else
#include "single_instance/jfn_single_instance.h"
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
#include "wlproxy/wlproxy.h"
#include "jfn_wl_proxy.h"
#include "jfn_wl_proxy.h"
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

// g_platform / g_video_bg definitions live in platform/platform_ops.cpp
// so main.cpp can be progressively shrunk without losing the owners.

// Boot-time mpv log forwarder. Used only by the pre-CEF event loop;
// the Rust-owned event thread routes its own log messages via
// jfn_mpv::forward_log_to_tracing.
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
    default:
        LOG_WARN(LOG_MPV, "[unhandled mpv level {}] {}: {}",
                 (int)msg->log_level, msg->prefix, msg->text); break;
    }
}

// Callbacks consumed by the Rust-owned mpv event thread. The platform
// vtable + macOS query_logical_content_size aren't bridged into Rust,
// so jfn_playback_set_*_provider wires them through here.

static float mpv_event_scale_provider() {
    float s = g_platform.get_scale ? g_platform.get_scale() : 1.0f;
    return s > 0.f ? s : 1.0f;
}

#ifdef __APPLE__
static bool mpv_event_macos_logical(int* lw, int* lh) {
    return macos_platform::query_logical_content_size(lw, lh);
}
#endif

static void mpv_event_fullscreen_handler(bool fs) {
    if (g_platform.set_fullscreen) g_platform.set_fullscreen(fs);
}

static void mpv_event_shutdown_handler() {
    LOG_INFO(LOG_MAIN, "MPV_EVENT_SHUTDOWN received");
    initiate_shutdown();
}



// Shutdown order (reverse of declaration):
//   browsers → CefShutdown → idle_inhibit clear → platform.cleanup
// then main runs mpv terminate + post_window_cleanup.
static int run_with_cef(int mw, int mh,
                        const cli::Args& args) {
    std::string ozone_platform = args.ozone_platform;
#if !defined(_WIN32) && !defined(__APPLE__)
    if (ozone_platform.empty())
        ozone_platform = g_platform.display == DisplayBackend::Wayland ? "wayland" : "x11";
#endif
    std::snprintf(g_platform.cef_ozone_platform,
                  sizeof(g_platform.cef_ozone_platform),
                  "%s", ozone_platform.c_str());
    PlatformScope platform_scope(g_platform, jfn_mpv_handle_get());
    if (!platform_scope.ok()) {
        LOG_ERROR(LOG_MAIN, "Platform init failed");
        return 1;
    }
    LOG_INFO(LOG_MAIN, "Platform init ok");

    IdleInhibitGuard idle_inhibit_guard;

    // Apply titlebar color before CefInitialize so the window doesn't sit
    // with the system default palette for the whole CEF init duration.
    if (Settings::instance().titlebarThemeColor())
        g_platform.set_theme_color(kBgColor.rgb);

    // Must run after the VO-init wait loop — sync mpv API calls would
    // deadlock against core_thread's DispatchQueue.main.sync on macOS.
    {
        JfnMpvCapabilities* caps = jfn_mpv_capabilities_query(jfn_mpv_handle_get());
        std::string profile = jellyfin_device_profile::Build(
            caps, "Jellyfin Desktop", APP_VERSION_FULL,
            Settings::instance().forceTranscoding());
        jfn_mpv_capabilities_free(caps);
        LOG_INFO(LOG_MAIN, "Device profile: {}", profile);
        jellyfin_device_profile::SetCachedJson(profile);
        jfn_cef_set_device_profile_json(profile.data(), profile.size());
    }

    bool use_shared_textures = g_platform.shared_texture_supported && !args.disable_gpu_compositing;

    jfn_cef_set_log_severity(static_cast<int>(toCefSeverity(effectiveLogLevel(LOG_CEF))));
    jfn_cef_set_remote_debugging_port(args.remote_debugging_port);
    jfn_cef_set_disable_gpu_compositing(!use_shared_textures);
#ifdef __linux__
    if (!ozone_platform.empty())
        jfn_cef_set_ozone_platform(ozone_platform.c_str());
#endif

    LOG_INFO(LOG_MAIN, "[FLOW] calling CefInitialize...");
    CefRuntimeScope cef_scope;
    if (!cef_scope.ok()) {
        LOG_ERROR(LOG_MAIN, "CefInitialize failed");
        return 1;
    }
    LOG_INFO(LOG_MAIN, "[FLOW] CefInitialize returned ok");

    double display_hidpi_scale = 0.0;
    mpv_get_property(jfn_mpv_handle_get(), "display-hidpi-scale",
                     MPV_FORMAT_DOUBLE, &display_hidpi_scale);
    int fs_flag = 0;
    mpv_get_property(jfn_mpv_handle_get(), "fullscreen", MPV_FORMAT_FLAG, &fs_flag);
    jfn_playback_seed_display_hz_sync();
    LOG_INFO(LOG_MAIN, "[FLOW] display-hidpi-scale={} fullscreen={} display-hz={}",
             display_hidpi_scale, fs_flag, jfn_playback_display_hz());

    // Scale-correct the window size when live display scale differs from
    // saved. Skip while the compositor has the surface locked
    // (fullscreen/maximized): mpv's wayland set_geometry runtime path
    // unconditionally writes wl->window_size and fires VO_EVENT_RESIZE,
    // which makes osd-dimensions emit the corrected size and CEF resize to
    // it — while the actual surface stays at the locked size. Internal/
    // visual size diverge ("sometimes" depending on whether the compositor
    // re-issues a configure). Defer: the next clean unmaximize/unfullscreen
    // restores to mpv's pre-init geometry value, the user resizes once, and
    // shutdown saves a matching scale so subsequent launches need no
    // correction.
    {
        const auto& saved = Settings::instance().windowGeometry();
        bool locked = fs_flag || jfn_playback_window_maximized();
        // Only correct when we have a real saved scale that differs from
        // live. Fresh-config (saved.scale == 0) was already computed at the
        // live scale by the pre-init probe; re-issuing SetGeometry here
        // takes mpv's runtime geometry path which bypasses configure_bounds.
        if (!locked && display_hidpi_scale > 0.0 && saved.scale > 0.f &&
            std::fabs(display_hidpi_scale - saved.scale) >= 0.01) {
            int new_pw = static_cast<int>(
                std::lround(saved.logical_width  * display_hidpi_scale));
            int new_ph = static_cast<int>(
                std::lround(saved.logical_height * display_hidpi_scale));
            int dummy_x = -1, dummy_y = -1;
            if (g_platform.clamp_window_geometry)
                g_platform.clamp_window_geometry(&new_pw, &new_ph,
                                                 &dummy_x, &dummy_y);
            std::string geom_str = std::to_string(new_pw) + "x"
                                 + std::to_string(new_ph);
            LOG_INFO(LOG_MAIN,
                     "[FLOW] scale {:.3f} -> {:.3f}, resize to {}",
                     saved.scale, display_hidpi_scale, geom_str.c_str());
            jfn_mpv_set_geometry(geom_str.c_str());
            mw = new_pw;
            mh = new_ph;
        }
        jfn_playback_set_window_pixels(mw, mh);
    }

    float scale = display_hidpi_scale > 0.0
        ? static_cast<float>(display_hidpi_scale)
        : g_platform.get_scale();
    int lw = static_cast<int>(mw / scale);
    int lh = static_cast<int>(mh / scale);

    // Must exist before main browser creation: the pre-loaded page fires
    // its initial theme-color IPC at DOMContentLoaded; onOverlayDismissed
    // needs a color already captured.
    bool titlebar_themed = Settings::instance().titlebarThemeColor();
    jfn_theme_color_init(
        titlebar_themed ? +[](uint32_t rgb) { g_platform.set_theme_color(rgb); } : nullptr,
        +[](const char* hex) { jfn_mpv_set_background_color_hex(hex); });
    jfn_theme_color_set_video_bg(g_video_bg.rgb);

    jfn_browsers_init(lw, lh, mw, mh, jfn_playback_display_hz(), use_shared_textures);
    jfn_shutdown_set_handler(+[]() {
        jfn_browsers_close_all();
        if (g_platform.wake_main_loop) g_platform.wake_main_loop();
    });

    JfnCefLayer* main_layer = jfn_browsers_create("web");
    jfn_web_init(main_layer);

    std::string server_url = Settings::instance().serverUrl();
    std::string main_url;
    // Eager pre-load: fetch the saved server while the overlay probes in
    // parallel. The overlay fades out on success, revealing the loaded page.
    if (!server_url.empty())
        main_url = server_url;

    LOG_INFO(LOG_MAIN, "[FLOW] CreateBrowser(main) url={} lw={} lh={} pw={} ph={}",
             main_url.c_str(), lw, lh, mw, mh);
    jfn_cef_layer_create(main_layer, main_url.data(), main_url.size());
    LOG_INFO(LOG_MAIN, "[FLOW] CreateBrowser(main) call returned");

    LOG_INFO(LOG_MAIN, "[FLOW] jfn_overlay_init(main_layer)");
    jfn_overlay_init(main_layer);
    LOG_INFO(LOG_MAIN, "[FLOW] jfn_overlay_init returned");

    // Coordinator + sinks must exist before any thread can post inputs or
    // observe playback state. Sinks register before start() so the worker
    // never delivers to a half-built fanout.
    PlaybackCoordinatorScope coord_scope;
    // Builtin idle_inhibit sink (Rust-side) calls into the platform vtable
    // through this handler. Install before any event posts.
    jfn_playback_set_idle_inhibit_handler([](uint32_t level) {
        g_platform.set_idle_inhibit(static_cast<IdleInhibitLevel>(level));
    });
    jfn_playback_set_theme_video_mode_handler([](bool active) {
        jfn_theme_color_set_video_mode(active);
    });
    // Rust-side builtin browser sink forwards UI events through exec_js
    // and the side-channel handlers below.
    jfn_playback_set_web_exec_js_handler([](const char* js) {
        if (js) jfn_web_exec_js(js);
    });
    jfn_playback_set_browsers_size_handler([](int32_t lw, int32_t lh, int32_t pw, int32_t ph) {
        jfn_browsers_set_size(lw, lh, pw, ph);
    });
    jfn_playback_set_browsers_refresh_rate_handler([](double hz) {
        LOG_INFO(LOG_MAIN, "Display refresh rate changed: {} Hz", hz);
        jfn_browsers_set_refresh_rate(hz);
    });
#if defined(__APPLE__)
    auto media_sink = std::make_shared<MacosSink>();
    playback::register_event_sink(media_sink);
    media_sink->start();
#elif defined(_WIN32)
    int64_t wid = 0;
    jfn_mpv_get_property_int("window-id", &wid);
    auto media_sink = std::make_shared<WindowsSink>(reinterpret_cast<HWND>(static_cast<intptr_t>(wid)));
    playback::register_event_sink(media_sink);
    media_sink->start();
#else
    // MPRIS sink lives Rust-side (jfn_playback::mpris_sink). The
    // coordinator's builtin event fanout (register_builtin_sinks) forwards
    // every event into the sink thread, which speaks D-Bus.
    jfn_mpris_sink_start("");
#endif
    // MpvActionSink lives Rust-side (jfn_playback::ffi::register_builtin_sinks).
    jfn_playback_set_display_scale_handler([](double s) {
        if (s > 0) jfn_browsers_set_scale(s);
    });
    jfn_playback_set_scale_provider(&mpv_event_scale_provider);
#ifdef __APPLE__
    jfn_playback_set_macos_logical_provider(&mpv_event_macos_logical);
#endif
    jfn_playback_set_fullscreen_handler(&mpv_event_fullscreen_handler);
    jfn_playback_set_shutdown_handler(&mpv_event_shutdown_handler);

    // Start before waitForLoad so mpv events (OSD_DIMS especially) reach
    // the platform/browsers during the overlay-only startup phase, before
    // the main browser finishes loading.
    LOG_INFO(LOG_MAIN, "[FLOW] starting Rust-owned mpv event thread");
    if (!jfn_playback_start_mpv_event_thread()) {
        LOG_ERROR(LOG_MAIN, "failed to start mpv event thread");
        return 1;
    }

#ifndef __APPLE__
    jfn_cef_layer_wait_for_load(main_layer);
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
    while (!jfn_browsers_all_closed()) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 60.0, true);
    }

#else
    jfn_browsers_wait_all_closed();
#endif

    jfn_theme_color_shutdown();
#if defined(__APPLE__) || defined(_WIN32)
    media_sink->stop();
#else
    jfn_mpris_sink_stop();
#endif

    jfn_playback_stop_mpv_event_thread();

    // Producers have joined; coordinator drains any in-flight inputs and
    // stops via PlaybackCoordinatorScope dtor at end of scope.

    // Save window geometry while mpv is still alive.
    {
        bool fs  = jfn_playback_fullscreen();
        bool max = jfn_playback_window_maximized();

        if (fs) {
            // Don't overwrite the saved windowed size with fullscreen dims;
            // only update the maximized flag for the eventual restore.
            auto geom = Settings::instance().windowGeometry();
            geom.maximized = jfn_playback_was_maximized_before_fullscreen();
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
            int pw = jfn_playback_window_pw();
            int ph = jfn_playback_window_ph();
            if (pw <= 0 || ph <= 0) {
                pw = jfn_playback_osd_pw();
                ph = jfn_playback_osd_ph();
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
    // Drain any async writes issued from browser callbacks above, then join
    // the worker so nothing is lost when CEF/platform teardown runs next.
    Settings::instance().shutdownSaveWorker();

    // All three business browsers are Rust-side singletons; their
    // BeforeClose callbacks already removed themselves from Browsers
    // during the close drain above. Browsers shutdown frees any
    // straggler surface and the Vec of layer handles.
    jfn_browsers_shutdown();

    return 0;
}

// =====================================================================
// Main
// =====================================================================

// Owns subprocess dispatch + settings load + CLI parse + logging
// initialisation. Returns >= 0 to terminate the process, -1 to continue.
struct JfnBootArgs {
    const char* hwdec;
    const char* audio_passthrough;
    const char* audio_channels;
    const char* log_level;
    const char* ozone_platform;
    const char* platform_override;
    bool audio_exclusive;
    bool disable_gpu_compositing;
    int  remote_debugging_port;
};
extern "C" int jfn_app_main(int argc, const char* const* argv);
extern "C" const JfnBootArgs* jfn_app_boot_args(void);
extern "C" void jfn_app_teardown(void);

int main(int argc, char* argv[]) {
    // Linux platform selection is deferred until after CLI parsing; on
    // Windows/macOS the choice is fixed so we can populate g_platform now
    // (before jfn_app_main runs CefExecuteProcess for subprocesses).
#ifdef _WIN32
    g_platform = make_platform(DisplayBackend::Windows);
#elif defined(__APPLE__)
    g_platform = make_platform(DisplayBackend::macOS);
#endif

    if (int rc = jfn_app_main(argc, const_cast<const char* const*>(argv)); rc >= 0)
        return rc;

    const JfnBootArgs* ba = jfn_app_boot_args();
    cli::Args args;
    args.hwdec = ba && ba->hwdec ? ba->hwdec : "";
    args.audio_passthrough = ba && ba->audio_passthrough ? ba->audio_passthrough : "";
    args.audio_exclusive = ba && ba->audio_exclusive;
    args.audio_channels = ba && ba->audio_channels ? ba->audio_channels : "";
    args.log_level = ba && ba->log_level ? ba->log_level : "";
    args.ozone_platform = ba && ba->ozone_platform ? ba->ozone_platform : "";
    args.platform_override = ba && ba->platform_override ? ba->platform_override : "";
    args.disable_gpu_compositing = ba && ba->disable_gpu_compositing;
    args.remote_debugging_port = ba ? ba->remote_debugging_port : 0;

    // Platform selection, signal handler, single-instance check, MPV_HOME,
    // and wlproxy startup are all owned by jfn_app_main now.

    // Restore saved window geometry. mpv's --geometry is always physical
    // pixels (m_geometry_apply at third_party/mpv/options/m_option.c:2296
    // assigns gm->w/h to widw/widh without applying dpi_scale), so we pass
    // physical pixels here. If the live display scale differs from what
    // these pixels were computed against, the post-CEF-init resize block
    // below corrects the window size once display-hidpi-scale is known.
    std::string boot_geometry;
    bool boot_force_position = false;
    bool boot_window_max = false;
    {
        using WG = Settings::WindowGeometry;
        auto saved_geom = Settings::instance().windowGeometry();

        int x = saved_geom.x, y = saved_geom.y;
        float scale = g_platform.get_display_scale(x, y);
        int w, h;
        if (saved_geom.logical_width > 0 && saved_geom.logical_height > 0) {
            w = static_cast<int>(std::lround(saved_geom.logical_width  * scale));
            h = static_cast<int>(std::lround(saved_geom.logical_height * scale));
        } else if (saved_geom.width > 0 && saved_geom.height > 0) {
            w = saved_geom.width;
            h = saved_geom.height;
        } else {
            w = static_cast<int>(std::lround(WG::kDefaultLogicalWidth  * scale));
            h = static_cast<int>(std::lround(WG::kDefaultLogicalHeight * scale));
        }
        LOG_DEBUG(LOG_MAIN, "initial scale: {} -> {}x{}", scale, w, h);

        if (g_platform.clamp_window_geometry)
            g_platform.clamp_window_geometry(&w, &h, &x, &y);
        boot_geometry = std::to_string(w) + "x" + std::to_string(h);
        if (x >= 0 && y >= 0) {
            boot_geometry += "+" + std::to_string(x) + "+" + std::to_string(y);
            boot_force_position = true;
        }
        boot_window_max = saved_geom.maximized;
    }

    if (!args.audio_passthrough.empty()) {
        // Normalize: dts-hd subsumes dts
        if (args.audio_passthrough.find("dts-hd") != std::string::npos) {
            std::string filtered;
            size_t pos = 0;
            while (pos < args.audio_passthrough.size()) {
                size_t comma = args.audio_passthrough.find(',', pos);
                if (comma == std::string::npos) comma = args.audio_passthrough.size();
                std::string codec = args.audio_passthrough.substr(pos, comma - pos);
                if (codec != "dts") {
                    if (!filtered.empty()) filtered += ',';
                    filtered += codec;
                }
                pos = comma + 1;
            }
            args.audio_passthrough = filtered;
        }
    }

    // Pick the libmpv log subscription level to match what jfn-logging
    // would actually surface for LOG_MPV. Cap at "debug"; mpv's "trace"
    // is extreme and not worth the IPC. mpv's "v" maps to our Debug;
    // mpv's "debug" maps to our Trace.
    const char* mpv_log_level = "no";
    if (jfn_log_enabled(LOG_MPV, (uint8_t)LogLevel::Trace))      mpv_log_level = "debug";
    else if (jfn_log_enabled(LOG_MPV, (uint8_t)LogLevel::Debug)) mpv_log_level = "v";
    else if (jfn_log_enabled(LOG_MPV, (uint8_t)LogLevel::Info))  mpv_log_level = "info";
    else if (jfn_log_enabled(LOG_MPV, (uint8_t)LogLevel::Warn))  mpv_log_level = "warn";
    else if (jfn_log_enabled(LOG_MPV, (uint8_t)LogLevel::Error)) mpv_log_level = "error";

    JfnMpvBoot boot{};
    boot.display_backend          = static_cast<uint8_t>(g_platform.display);
    boot.hwdec                    = args.hwdec.c_str();
    boot.user_agent               = APP_USER_AGENT;
    boot.audio_passthrough        = args.audio_passthrough.empty()
                                  ? nullptr : args.audio_passthrough.c_str();
    boot.audio_exclusive          = args.audio_exclusive;
    boot.audio_channels           = args.audio_channels.empty()
                                  ? nullptr : args.audio_channels.c_str();
    boot.geometry                 = boot_geometry.c_str();
    boot.force_window_position    = boot_force_position;
    boot.window_maximized_at_boot = boot_window_max;
    boot.mpv_log_level            = mpv_log_level;

    mpv_handle* raw = jfn_mpv_handle_init(&boot);
    if (!raw) { LOG_ERROR(LOG_MAIN, "mpv handle init failed"); return 1; }

    // Register property observations after init via the Rust ingest layer.
    jfn_playback_observe_mpv_properties(static_cast<uint8_t>(g_platform.display));

    // Capture user's mpv.conf bg, then force startup color. Safe here:
    // force-window=yes (not "immediate") defers VO creation, so the user's
    // color never flashes before we override.
    g_video_bg = Color{jfn_mpv_get_background_color()};
    LOG_INFO(LOG_MAIN, "video bg captured: {}", g_video_bg.hex);
    jfn_mpv_set_background_color_hex(kBgColor.hex);

    for (const char* prop : {"mpv-version", "ffmpeg-version"}) {
        char* v = jfn_mpv_get_property_string(prop);
        LOG_INFO(LOG_MAIN, "{} {}", prop, v ? v : "");
        jfn_mpv_free_string(v);
    }

    // input-default-bindings=no removes all builtin bindings including
    // CLOSE_WIN → quit.  Re-bind it so the WM close button works.
    {
        const char* cmd[] = {"keybind", "CLOSE_WIN", "quit", nullptr};
        mpv_command(jfn_mpv_handle_get(), cmd);
    }

    // Wait for the VO window. Reads osd-dimensions from the event payload
    // (no sync mpv_get_property call) so it stays safe against a
    // DispatchQueue.main.sync deadlock against core_thread on macOS.
    LOG_INFO(LOG_MAIN, "Waiting for mpv window...");
    int64_t mw = 0, mh = 0;
    // First OSD_DIMS event reflects the pre-configure geometry hint, not the
    // post-configure surface size. When maximized startup is requested, also
    // wait for the window-maximized property to flip true (proves mpv has
    // processed the compositor's maximize configure) and take the OSD_DIMS
    // that follows.
    //
    // On Wayland we don't observe osd-dimensions: the proxy's
    // xdg_toplevel.configure intercept (jfn-wayland::proxy::on_configure)
    // calls jfn_playback_post_osd_pixels directly, filling the same
    // osd_pw/osd_ph atomics from a non-mpv-event source.
    // The poll below reads the atomics every iteration to pick up the
    // value regardless of whether a mpv property-change event arrived.
    bool need_max = Settings::instance().windowGeometry().maximized;
    // On Wayland the initial logical-pixel computation in run_with_cef
    // needs cached_scale populated by the proxy's preferred_scale callback.
    // Wait for it explicitly — otherwise CEF starts at physical*1.0 size on
    // fractional displays.
#if !defined(_WIN32) && !defined(__APPLE__)
    const bool wait_for_scale = g_platform.display == DisplayBackend::Wayland;
#else
    const bool wait_for_scale = false;
#endif
    auto consume = [&](mpv_event* ev) -> bool {
        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            float scale = g_platform.get_scale ? g_platform.get_scale() : 1.0f;
            if (scale <= 0.f) scale = 1.0f;
            bool has_macos_logical = false;
            int  mac_lw = 0, mac_lh = 0;
#ifdef __APPLE__
            has_macos_logical = macos_platform::query_logical_content_size(
                &mac_lw, &mac_lh);
#endif
            jfn_playback_ingest_mpv_event(
                ev, scale, has_macos_logical, mac_lw, mac_lh);
            if (ev->reply_userdata == JFN_OBSERVE_WINDOW_MAX &&
                jfn_playback_window_maximized())
                need_max = false;
        }
        if (jfn_playback_osd_pw() > 0 && jfn_playback_osd_ph() > 0) {
            mw = jfn_playback_osd_pw();
            mh = jfn_playback_osd_ph();
        }
#if !defined(_WIN32) && !defined(__APPLE__)
        bool scale_ready = !wait_for_scale || jfn_wl_scale_known();
#else
        bool scale_ready = true;
#endif
        return mw > 0 && !need_max && scale_ready;
    };

#ifdef __APPLE__
    while (true) {
        g_platform.pump();
        mpv_event* ev = jfn_mpv_wait_event(0);
        if (ev->event_id == MPV_EVENT_NONE) { usleep(10000); continue; }
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            log_mpv_message(static_cast<mpv_event_log_message*>(ev->data));
            continue;
        }
        if (ev->event_id == MPV_EVENT_SHUTDOWN || ev->event_id == MPV_EVENT_END_FILE) {
            return 0;
        }
        if (consume(ev)) break;
    }
#else
    // Short timeout so the loop polls jfn_playback_osd_pw/ph on Wayland
    // too — the proxy can update those atomics without producing any mpv
    // event.
    const double wait_timeout = g_platform.display == DisplayBackend::Wayland
        ? 0.1 : 1.0;
    while (true) {
        mpv_event* ev = jfn_mpv_wait_event(wait_timeout);
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            log_mpv_message(static_cast<mpv_event_log_message*>(ev->data));
            continue;
        }
        if (ev->event_id == MPV_EVENT_SHUTDOWN) return 0;
        if (ev->event_id == MPV_EVENT_END_FILE) return 0;
        if (consume(ev)) break;
    }
#endif

    int rc = run_with_cef(static_cast<int>(mw), static_cast<int>(mh), args);
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
        jfn_mpv_handle_terminate();
        mpv_done.store(true, std::memory_order_release);
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
    while (!mpv_done.load(std::memory_order_acquire))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,
                           std::numeric_limits<CFTimeInterval>::max(), true);
    mpv_teardown.join();
#else
    jfn_mpv_handle_terminate();
#endif

    jfn_app_teardown();

    if (g_platform.post_window_cleanup)
        g_platform.post_window_cleanup();

    return 0;
}
