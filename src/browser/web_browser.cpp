#include "web_browser.h"
#include "about_browser.h"
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

// Helper to read an int from CefListValue that may have been sent as double.
static int getIntArg(CefRefPtr<CefListValue> args, size_t idx) {
    if (args->GetType(idx) == VTYPE_DOUBLE)
        return static_cast<int>(std::lround(args->GetDouble(idx)));
    return args->GetInt(idx);
}

// =====================================================================
// WebBrowser
// =====================================================================

CefRefPtr<CefDictionaryValue> WebBrowser::injectionProfile() {
    static const char* const kFunctions[] = {
        "playerLoad", "playerStop", "playerPause", "playerPlay", "playerSeek",
        "playerSetVolume", "playerSetMuted", "playerSetSpeed",
        "playerSetSubtitle", "playerAddSubtitle", "playerSetAudio",
        "playerSetAudioDelay", "playerSetAspectMode", "playerOsdActive",
        "saveServerUrl",
        "notifyMetadata", "notifyPosition", "notifySeek",
        "notifyPlaybackState", "notifyArtwork", "notifyQueueChange",
        "notifyRateChange",
        "appExit", "setSettingValue", "themeColor",
        "setOsdVisible", "setCursorVisible", "toggleFullscreen",
        "menuItemSelected", "menuDismissed",
    };
    static const char* const kScripts[] = {
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
    client_->setMessageHandler([this](const std::string& name,
                                      CefRefPtr<CefListValue> args,
                                      CefRefPtr<CefBrowser> browser) {
        return handleMessage(name, args, browser);
    });
    client_->setCreatedCallback([](CefRefPtr<CefBrowser> browser) {
        // Main browser takes input only if the overlay isn't currently active.
        if (!g_overlay_browser)
            input::set_active_browser(browser);
    });
}

bool WebBrowser::handleMessage(const std::string& name,
                               CefRefPtr<CefListValue> args,
                               CefRefPtr<CefBrowser> browser) {
    if (!g_mpv.IsValid()) return false;

    if (name == "playerLoad") {
        std::string url = args->GetString(0).ToString();
        int startMs = args->GetSize() > 1 ? getIntArg(args, 1) : 0;
        int audioIdx = getIntArg(args, 2);
        int subIdx = getIntArg(args, 3);
        LOG_INFO(LOG_CEF, "playerLoad: audio={} sub={} start={}ms url={}",
                 audioIdx, subIdx, startMs, url.c_str());
        MpvHandle::LoadOptions opts;
        opts.startSecs = startMs / 1000.0;
        bool isHls = url.find("master.m3u8") != std::string::npos;
        opts.audioTrack = isHls ? MpvHandle::kTrackAuto : audioIdx;
        opts.subTrack = isHls ? MpvHandle::kTrackAuto : subIdx;
        g_mpv.LoadFile(url, opts);
    } else if (name == "playerStop") {
        g_mpv.Stop();
    } else if (name == "playerPause") {
        g_mpv.Pause();
    } else if (name == "playerPlay") {
        g_mpv.Play();
    } else if (name == "playerSeek") {
        double pos = getIntArg(args, 0) / 1000.0;
        g_mpv.SeekAbsolute(pos);
    } else if (name == "playerSetVolume") {
        g_mpv.SetVolume(getIntArg(args, 0));
    } else if (name == "playerSetMuted") {
        g_mpv.SetMuted(args->GetBool(0));
    } else if (name == "playerSetSpeed") {
        g_mpv.SetSpeed(getIntArg(args, 0) / 1000.0);
    } else if (name == "playerSetSubtitle") {
        LOG_INFO(LOG_CEF, "playerSetSubtitle: {}", getIntArg(args, 0));
        g_mpv.SetSubtitleTrack(getIntArg(args, 0));
    } else if (name == "playerAddSubtitle") {
        std::string url = args->GetString(0).ToString();
        LOG_INFO(LOG_CEF, "playerAddSubtitle: {}", url.c_str());
        g_mpv.SubAdd(url);
    } else if (name == "playerSetAudio") {
        g_mpv.SetAudioTrack(getIntArg(args, 0));
    } else if (name == "playerSetAudioDelay") {
        g_mpv.SetAudioDelay(args->GetDouble(0));
    } else if (name == "playerSetAspectMode") {
        g_mpv.SetAspectMode(args->GetString(0).ToString());
    } else if (name == "playerOsdActive") {
        bool active = args->GetBool(0);
        if (active) {
            was_fullscreen_before_osd_ = mpv::fullscreen();
        } else {
            if (!was_fullscreen_before_osd_)
                g_platform.set_fullscreen(false);
        }
    } else if (name == "toggleFullscreen") {
        g_platform.toggle_fullscreen();
    } else if (name == "saveServerUrl") {
        std::string url = args->GetString(0).ToString();
        Settings::instance().setServerUrl(url);
        Settings::instance().saveAsync();
    } else if (name == "setSettingValue") {
        std::string section = args->GetString(0).ToString();
        std::string key = args->GetString(1).ToString();
        std::string value = args->GetString(2).ToString();
        applySettingValue(section, key, value);
    } else if (name == "themeColor") {
        std::string color = args->GetString(0).ToString();
        LOG_DEBUG(LOG_CEF, "themeColor IPC: {}", color.c_str());
        if (g_titlebar_color) g_titlebar_color->onThemeColor(color);
    } else if (name == "notifyMetadata") {
        std::string json = args->GetString(0).ToString();
        MediaMetadata meta = parseMetadataJson(json);
        g_media_type = meta.media_type;
        update_idle_inhibit();
        if (g_media_session)
            g_media_session->setMetadata(meta);
    } else if (name == "notifyArtwork") {
        std::string artworkUri = args->GetString(0).ToString();
        if (g_media_session) g_media_session->setArtwork(artworkUri);
    } else if (name == "notifyQueueChange") {
        bool canNext = args->GetBool(0);
        bool canPrev = args->GetBool(1);
        if (g_media_session) {
            g_media_session->setCanGoNext(canNext);
            g_media_session->setCanGoPrevious(canPrev);
        }
    } else if (name == "notifyPlaybackState") {
        std::string state = args->GetString(0).ToString();
        if (g_media_session) {
            if (state == "Playing") g_media_session->setPlaybackState(PlaybackState::Playing);
            else if (state == "Paused") g_media_session->setPlaybackState(PlaybackState::Paused);
            else g_media_session->setPlaybackState(PlaybackState::Stopped);
        }
    } else if (name == "notifySeek") {
        int posMs = getIntArg(args, 0);
        if (g_media_session)
            g_media_session->emitSeeked(static_cast<int64_t>(posMs) * 1000);
    } else if (name == "setCursorVisible") {
        g_platform.set_cursor(args->GetBool(0) ? CT_POINTER : CT_NONE);
    } else if (name == "appExit") {
        initiate_shutdown();
    } else if (name == "openAbout") {
        AboutBrowser::open();
    } else {
        return false;
    }
    return true;
}
