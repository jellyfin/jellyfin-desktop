#include "web_browser.h"
#include "app_menu.h"
#include "browsers.h"
#include <cmath>
#include "../common.h"
#include "../settings.h"
#include "logging.h"
#include "../mpv/event.h"
#include "../player/media_session.h"
#include "../player/media_session_thread.h"
#include "../titlebar_color.h"
#include "../input/dispatch.h"
#include "../cjson/cJSON.h"
#include "../paths/paths.h"
#include "../cef/message_bus.h"
#include "../player/player_router.h"

extern void update_idle_inhibit();

// =====================================================================
// Helpers
// =====================================================================

static MediaMetadata parseMetadataJson(const std::string& json) {
    MediaMetadata meta;
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return meta;

    cJSON* item;
    if ((item = cJSON_GetObjectItem(root, "Name")) && cJSON_IsString(item))
        meta.title = item->valuestring;
    if ((item = cJSON_GetObjectItem(root, "SeriesName")) && cJSON_IsString(item))
        meta.artist = item->valuestring;
    if (meta.artist.empty()) {
        if ((item = cJSON_GetObjectItem(root, "Artists")) && cJSON_IsArray(item)) {
            cJSON* first = cJSON_GetArrayItem(item, 0);
            if (first && cJSON_IsString(first))
                meta.artist = first->valuestring;
        }
    }
    if ((item = cJSON_GetObjectItem(root, "SeasonName")) && cJSON_IsString(item))
        meta.album = item->valuestring;
    if (meta.album.empty()) {
        if ((item = cJSON_GetObjectItem(root, "Album")) && cJSON_IsString(item))
            meta.album = item->valuestring;
    }
    if ((item = cJSON_GetObjectItem(root, "IndexNumber")) && cJSON_IsNumber(item))
        meta.track_number = item->valueint;
    if ((item = cJSON_GetObjectItem(root, "RunTimeTicks")) && cJSON_IsNumber(item))
        meta.duration_us = static_cast<int64_t>(item->valuedouble) / 10;
    if ((item = cJSON_GetObjectItem(root, "Type")) && cJSON_IsString(item)) {
        std::string type = item->valuestring;
        if (type == "Audio") meta.media_type = MediaType::Audio;
        else if (type == "Movie" || type == "Episode" || type == "Video" || type == "MusicVideo")
            meta.media_type = MediaType::Video;
    }
    cJSON_Delete(root);
    return meta;
}

static void applySettingValue(const std::string& section, const std::string& key, const std::string& value) {
    auto& s = Settings::instance();
    if (key == "hwdec") s.setHwdec(value);
    else if (key == "audioPassthrough") s.setAudioPassthrough(value);
    else if (key == "audioExclusive") s.setAudioExclusive(value == "true");
    else if (key == "audioChannels") s.setAudioChannels(value);
    else if (key == "logLevel") s.setLogLevel(value);
    else LOG_WARN(LOG_CEF, "Unknown setting key: {}.{}", section.c_str(), key.c_str());
    s.saveAsync();
}

// =====================================================================
// WebBrowser
// =====================================================================

CefRefPtr<CefDictionaryValue> WebBrowser::injectionProfile() {
    static const char* const kFunctions[] = {
        "send",
        "menuItemSelected", "menuDismissed",
    };
    static const char* const kScripts[] = {
        "jmp-bus.js",
        "native-shim.js",
        "mpv-player-core.js",
        "mpv-video-player.js",
        "mpv-audio-player.js",
        "input-plugin.js",
        "client-settings.js",
        "context-menu.js",
    };

    CefRefPtr<CefListValue> fns = CefListValue::Create();
    for (size_t i = 0; i < sizeof(kFunctions) / sizeof(*kFunctions); i++)
        fns->SetString(i, kFunctions[i]);
    CefRefPtr<CefListValue> scripts = CefListValue::Create();
    for (size_t i = 0; i < sizeof(kScripts) / sizeof(*kScripts); i++)
        scripts->SetString(i, kScripts[i]);

    CefRefPtr<CefDictionaryValue> d = CefDictionaryValue::Create();
    d->SetList("functions", fns);
    d->SetList("scripts", scripts);
    return d;
}

WebBrowser::WebBrowser(RenderTarget target, int w, int h, int pw, int ph)
    : client_(new CefLayer(target, w, h, pw, ph))
{
    client_->setCreatedCallback([](CefRefPtr<CefBrowser> browser) {
        // Main browser takes input only if the overlay isn't currently active.
        if (!g_overlay_browser)
            input::set_active_browser(browser);
        // All web-browser-owned namespaces route to this CefBrowser for
        // outbound bus emissions. Idempotent — safe across reloads.
        g_bus.registerNamespace("player",     browser);
        g_bus.registerNamespace("playback",   browser);
        g_bus.registerNamespace("fullscreen", browser);
        g_bus.registerNamespace("osd",        browser);
        g_bus.registerNamespace("app",        browser);
        g_bus.registerNamespace("input",      browser);
        player_router::install();
    });
    client_->setContextMenuBuilder(&app_menu::build);
    client_->setContextMenuDispatcher(&app_menu::dispatch);

    installBusHandlers();
}

// =====================================================================
// MessageBus handler registration (playback / fullscreen / osd / app)
// =====================================================================
//
// Player handlers live in player_router; the rest live here because they
// reuse existing C++ logic that's tied to WebBrowser-adjacent globals
// (g_media_session, g_platform, Settings, paths, g_titlebar_color).
//
// Registered in parallel with the legacy `handleMessage` arms; old per-method
// `jmpNative.*` calls keep working until the JS migration step deletes them.

void WebBrowser::installBusHandlers() {
    // -------- playback (observed player state from JS) --------
    g_bus.on("playback.metadata", [](CefRefPtr<CefDictionaryValue> p) {
        if (!p->HasKey("json") || p->GetType("json") != VTYPE_STRING) return;
        MediaMetadata meta = parseMetadataJson(p->GetString("json").ToString());
        g_media_type = meta.media_type;
        update_idle_inhibit();
        if (g_media_session) g_media_session->setMetadata(meta);
    });
    g_bus.on("playback.artwork", [](CefRefPtr<CefDictionaryValue> p) {
        if (!g_media_session) return;
        if (p->HasKey("uri") && p->GetType("uri") == VTYPE_STRING)
            g_media_session->setArtwork(p->GetString("uri").ToString());
    });
    g_bus.on("playback.queueChange", [](CefRefPtr<CefDictionaryValue> p) {
        if (!g_media_session) return;
        if (p->HasKey("canNext"))
            g_media_session->setCanGoNext(p->GetBool("canNext"));
        if (p->HasKey("canPrev"))
            g_media_session->setCanGoPrevious(p->GetBool("canPrev"));
    });
    g_bus.on("playback.state", [](CefRefPtr<CefDictionaryValue> p) {
        if (!g_media_session) return;
        if (!p->HasKey("state") || p->GetType("state") != VTYPE_STRING) return;
        std::string state = p->GetString("state").ToString();
        if (state == "Playing")      g_media_session->setPlaybackState(PlaybackState::Playing);
        else if (state == "Paused")  g_media_session->setPlaybackState(PlaybackState::Paused);
        else                         g_media_session->setPlaybackState(PlaybackState::Stopped);
    });
    g_bus.on("playback.position", [](CefRefPtr<CefDictionaryValue> p) {
        if (!g_media_session) return;
        int posMs = 0;
        if (p->HasKey("positionMs")) {
            auto t = p->GetType("positionMs");
            if (t == VTYPE_INT)         posMs = p->GetInt("positionMs");
            else if (t == VTYPE_DOUBLE) posMs = static_cast<int>(std::lround(p->GetDouble("positionMs")));
        }
        g_media_session->emitSeeked(static_cast<int64_t>(posMs) * 1000);
    });

    // -------- fullscreen --------
    g_bus.on("fullscreen.toggle", [](CefRefPtr<CefDictionaryValue>) {
        g_platform.toggle_fullscreen();
    });

    // -------- osd --------
    g_bus.on("osd.active", [this](CefRefPtr<CefDictionaryValue> p) {
        bool active = p->HasKey("active") && p->GetBool("active");
        if (active) {
            was_fullscreen_before_osd_ = mpv::fullscreen();
        } else {
            if (!was_fullscreen_before_osd_)
                g_platform.set_fullscreen(false);
        }
    });
    g_bus.on("osd.cursorVisible", [](CefRefPtr<CefDictionaryValue> p) {
        bool visible = p->HasKey("visible") && p->GetBool("visible");
        g_platform.set_cursor(visible ? CT_POINTER : CT_NONE);
    });

    // -------- app --------
    g_bus.on("app.exit", [](CefRefPtr<CefDictionaryValue>) {
        initiate_shutdown();
    });
    g_bus.on("app.openConfigDir", [](CefRefPtr<CefDictionaryValue>) {
        LOG_INFO(LOG_CEF, "Opening mpv home directory");
        paths::openMpvHome();
    });
    g_bus.on("app.saveServerUrl", [](CefRefPtr<CefDictionaryValue> p) {
        if (!p->HasKey("url") || p->GetType("url") != VTYPE_STRING) return;
        Settings::instance().setServerUrl(p->GetString("url").ToString());
        Settings::instance().saveAsync();
    });
    g_bus.on("app.setSettingValue", [](CefRefPtr<CefDictionaryValue> p) {
        std::string section = p->HasKey("section") ? p->GetString("section").ToString() : "";
        std::string key     = p->HasKey("key")     ? p->GetString("key").ToString()     : "";
        std::string value   = p->HasKey("value")   ? p->GetString("value").ToString()   : "";
        applySettingValue(section, key, value);
    });
    g_bus.on("app.themeColor", [](CefRefPtr<CefDictionaryValue> p) {
        if (!p->HasKey("color") || p->GetType("color") != VTYPE_STRING) return;
        std::string color = p->GetString("color").ToString();
        LOG_DEBUG(LOG_CEF, "themeColor IPC: {}", color.c_str());
        if (g_titlebar_color) g_titlebar_color->onThemeColor(color);
    });
}

