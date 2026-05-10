#ifdef _WIN32

// WinRT headers must come before windows.h
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>

#include <systemmediatransportcontrolsinterop.h>
#include <wincrypt.h>

#include "windows_sink.h"

#include "../../../common.h"
#include "../../../browser/browsers.h"
#include "../../../browser/web_browser.h"
#include "../../../logging.h"

#include <condition_variable>
#include <mutex>

using namespace winrt;
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Foundation;

struct WindowsSink::WinRTState {
    SystemMediaTransportControls smtc{nullptr};
    SystemMediaTransportControlsDisplayUpdater updater{nullptr};
    SystemMediaTransportControls::ButtonPressed_revoker button_revoker;
    SystemMediaTransportControls::PlaybackPositionChangeRequested_revoker seek_revoker;
    RandomAccessStreamReference cached_thumbnail{nullptr};
};

WindowsSink::WindowsSink(HWND hwnd) : hwnd_(hwnd) {}

WindowsSink::~WindowsSink() {
    stop();
}

void WindowsSink::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&WindowsSink::threadFunc, this);
}

void WindowsSink::stop() {
    if (!running_.exchange(false)) return;
    wake().signal();
    if (thread_.joinable()) thread_.join();
}

void WindowsSink::initSmtc() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& e) {
        if (e.code() != HRESULT(0x80010106))  // RPC_E_CHANGED_MODE
            throw;
    }

    if (!hwnd_) {
        LOG_ERROR(LOG_MEDIA, "[SMTC] NULL HWND provided");
        return;
    }

    try {
        auto interop = winrt::get_activation_factory<
            SystemMediaTransportControls,
            ISystemMediaTransportControlsInterop>();

        SystemMediaTransportControls smtc{nullptr};
        winrt::check_hresult(interop->GetForWindow(
            hwnd_,
            winrt::guid_of<SystemMediaTransportControls>(),
            winrt::put_abi(smtc)));

        smtc.IsEnabled(true);
        smtc.IsPlayEnabled(true);
        smtc.IsPauseEnabled(true);
        smtc.IsStopEnabled(true);
        smtc.IsNextEnabled(false);
        smtc.IsPreviousEnabled(false);

        state_ = std::make_unique<WinRTState>();
        state_->smtc = std::move(smtc);
        state_->updater = state_->smtc.DisplayUpdater();

        state_->button_revoker = state_->smtc.ButtonPressed(winrt::auto_revoke,
            [this](SystemMediaTransportControls const&,
                   SystemMediaTransportControlsButtonPressedEventArgs const& args) {
                onButtonPressed(static_cast<int>(args.Button()));
            });

        state_->seek_revoker = state_->smtc.PlaybackPositionChangeRequested(winrt::auto_revoke,
            [](SystemMediaTransportControls const&,
               PlaybackPositionChangeRequestedEventArgs const& args) {
                auto pos_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    args.RequestedPlaybackPosition()).count();
                int ms = static_cast<int>(pos_us / 1000);
                if (g_web_browser)
                    g_web_browser->execJs("if(window._nativeSeek) window._nativeSeek(" + std::to_string(ms) + ");");
            });

        LOG_INFO(LOG_MEDIA, "[SMTC] Initialized");
    } catch (const winrt::hresult_error& e) {
        LOG_ERROR(LOG_MEDIA, "[SMTC] Init failed: {}", winrt::to_string(e.message()));
    }
}

void WindowsSink::teardownSmtc() {
    if (!state_) return;
    try {
        state_->button_revoker.revoke();
        state_->seek_revoker.revoke();
        state_->updater.ClearAll();
        state_->updater.Update();
        state_->smtc.IsEnabled(false);
    } catch (const winrt::hresult_error&) {}
    state_.reset();
}

void WindowsSink::threadFunc() {
    initSmtc();

    std::mutex local_mtx;
    std::unique_lock<std::mutex> lock(local_mtx);
    std::condition_variable local_cv;

    while (running_.load(std::memory_order_relaxed)) {
        wake().drain();
        pump();
        // SMTC events are callback-driven; just gate on wake / 100ms cap.
        local_cv.wait_for(lock, std::chrono::milliseconds(100));
    }

    teardownSmtc();
}

void WindowsSink::onButtonPressed(int button) {
    using B = SystemMediaTransportControlsButton;
    switch (static_cast<B>(button)) {
        case B::Play:
            g_mpv.Play(); break;
        case B::Pause:
            g_mpv.Pause(); break;
        case B::Stop:
            g_mpv.Stop(); break;
        case B::Next:
            if (g_web_browser)
                g_web_browser->execJs("if(window._nativeHostInput) window._nativeHostInput(['next']);");
            break;
        case B::Previous:
            if (g_web_browser)
                g_web_browser->execJs("if(window._nativeHostInput) window._nativeHostInput(['previous']);");
            break;
        default: break;
    }
}

static PlaybackState mapEventToState(PlaybackEvent::Kind k) {
    switch (k) {
    case PlaybackEvent::Kind::Started:     return PlaybackState::Playing;
    case PlaybackEvent::Kind::Paused:      return PlaybackState::Paused;
    case PlaybackEvent::Kind::TrackLoaded: return PlaybackState::Paused;
    case PlaybackEvent::Kind::Finished:
    case PlaybackEvent::Kind::Canceled:
    case PlaybackEvent::Kind::Error:       return PlaybackState::Stopped;
    default:                               return PlaybackState::Stopped;
    }
}

void WindowsSink::deliver(const PlaybackEvent& ev) {
    switch (ev.kind) {
    case PlaybackEvent::Kind::MetadataChanged:
        if (!ev.metadata.id.empty() && ev.metadata.id == metadata_.id) break;
        metadata_ = ev.metadata;
        if (playback_state_ != PlaybackState::Stopped)
            updateDisplayProperties();
        break;
    case PlaybackEvent::Kind::ArtworkChanged: {
        if (!state_ || ev.artwork_uri.empty()) break;
        size_t comma = ev.artwork_uri.find(',');
        if (comma == std::string::npos) break;
        const char* b64_ptr = ev.artwork_uri.c_str() + comma + 1;
        DWORD b64_len = static_cast<DWORD>(ev.artwork_uri.size() - comma - 1);

        DWORD decoded_len = 0;
        if (!CryptStringToBinaryA(b64_ptr, b64_len,
                                  CRYPT_STRING_BASE64, nullptr, &decoded_len,
                                  nullptr, nullptr)) break;

        std::vector<uint8_t> buf(decoded_len);
        if (!CryptStringToBinaryA(b64_ptr, b64_len,
                                  CRYPT_STRING_BASE64, buf.data(), &decoded_len,
                                  nullptr, nullptr)) break;
        buf.resize(decoded_len);

        try {
            InMemoryRandomAccessStream stream;
            DataWriter writer(stream);
            writer.WriteBytes(winrt::array_view<const uint8_t>(buf));
            writer.StoreAsync().get();  // Safe: called from MTA (sink thread)
            writer.DetachStream();
            stream.Seek(0);
            auto ref = RandomAccessStreamReference::CreateFromStream(stream);
            state_->cached_thumbnail = ref;
            state_->updater.Thumbnail(ref);
            state_->updater.Update();
        } catch (const winrt::hresult_error&) {}
        break;
    }
    case PlaybackEvent::Kind::QueueCapsChanged:
        if (state_) {
            state_->smtc.IsNextEnabled(ev.can_go_next);
            state_->smtc.IsPreviousEnabled(ev.can_go_prev);
        }
        break;
    case PlaybackEvent::Kind::Started:
    case PlaybackEvent::Kind::Paused:
    case PlaybackEvent::Kind::TrackLoaded:
    case PlaybackEvent::Kind::Finished:
    case PlaybackEvent::Kind::Canceled:
    case PlaybackEvent::Kind::Error: {
        playback_state_ = mapEventToState(ev.kind);
        if (!state_) break;
        switch (playback_state_) {
        case PlaybackState::Playing:
            state_->smtc.PlaybackStatus(MediaPlaybackStatus::Playing);
            updateDisplayProperties();
            break;
        case PlaybackState::Paused:
            state_->smtc.PlaybackStatus(MediaPlaybackStatus::Paused);
            updateTimeline();
            break;
        case PlaybackState::Stopped:
            metadata_ = MediaMetadata{};
            position_us_ = 0;
            state_->cached_thumbnail = nullptr;
            state_->updater.ClearAll();
            state_->updater.Update();
            state_->smtc.PlaybackStatus(MediaPlaybackStatus::Stopped);
            return;
        }
        pending_update_ = true;
        break;
    }
    case PlaybackEvent::Kind::PositionChanged: {
        position_us_ = ev.snapshot.position_us;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_position_update_).count();
        if (pending_update_ || elapsed >= 1000) {
            updateTimeline();
            last_position_update_ = now;
            pending_update_ = false;
        }
        break;
    }
    case PlaybackEvent::Kind::Seeked:
        position_us_ = ev.snapshot.position_us;
        updateTimeline();
        last_position_update_ = std::chrono::steady_clock::now();
        pending_update_ = false;
        break;
    case PlaybackEvent::Kind::RateChanged:
    case PlaybackEvent::Kind::SeekingChanged:
    case PlaybackEvent::Kind::BufferingChanged:
    case PlaybackEvent::Kind::DurationChanged:
    case PlaybackEvent::Kind::MediaTypeChanged:
    case PlaybackEvent::Kind::FullscreenChanged:
    case PlaybackEvent::Kind::OsdDimsChanged:
    case PlaybackEvent::Kind::BufferedRangesChanged:
    case PlaybackEvent::Kind::DisplayHzChanged:
        break;
    }
}

void WindowsSink::updateDisplayProperties() {
    if (!state_ || playback_state_ == PlaybackState::Stopped) return;

    state_->updater.ClearAll();

    if (metadata_.media_type == MediaType::Audio) {
        state_->updater.Type(MediaPlaybackType::Music);
        auto music = state_->updater.MusicProperties();
        music.Title(winrt::to_hstring(metadata_.title));
        music.Artist(winrt::to_hstring(metadata_.artist));
        music.AlbumTitle(winrt::to_hstring(metadata_.album));
        if (metadata_.track_number > 0)
            music.TrackNumber(static_cast<uint32_t>(metadata_.track_number));
    } else {
        state_->updater.Type(MediaPlaybackType::Video);
        auto video = state_->updater.VideoProperties();
        video.Title(winrt::to_hstring(metadata_.title));
        if (!metadata_.artist.empty())
            video.Subtitle(winrt::to_hstring(metadata_.artist));
    }

    if (state_->cached_thumbnail)
        state_->updater.Thumbnail(state_->cached_thumbnail);

    state_->updater.Update();
    updateTimeline();
}

void WindowsSink::updateTimeline() {
    if (!state_ || metadata_.duration_us <= 0) return;
    try {
        SystemMediaTransportControlsTimelineProperties tl;
        auto to_ticks = [](int64_t us) -> TimeSpan { return TimeSpan{us * 10}; };
        tl.StartTime(TimeSpan{0});
        tl.EndTime(to_ticks(metadata_.duration_us));
        tl.Position(to_ticks(position_us_));
        tl.MinSeekTime(TimeSpan{0});
        tl.MaxSeekTime(to_ticks(metadata_.duration_us));
        state_->smtc.UpdateTimelineProperties(tl);
    } catch (const winrt::hresult_error&) {}
}

#endif // _WIN32
