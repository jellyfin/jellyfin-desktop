#ifdef _WIN32

// WinRT headers must come before windows.h
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>

#include <systemmediatransportcontrolsinterop.h>
#include <wincrypt.h>

#include "player/windows/media_session_windows.h"
#include "logging.h"

using namespace winrt;
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Foundation;

struct WindowsMediaBackend::WinRTState {
    SystemMediaTransportControls smtc{nullptr};
    SystemMediaTransportControlsDisplayUpdater updater{nullptr};
    SystemMediaTransportControls::ButtonPressed_revoker button_revoker;
    SystemMediaTransportControls::PlaybackPositionChangeRequested_revoker seek_revoker;
    RandomAccessStreamReference cached_thumbnail{nullptr};
};

WindowsMediaBackend::WindowsMediaBackend(MediaSession* session, HWND hwnd)
    : session_(session) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& e) {
        if (e.code() != HRESULT(0x80010106))  // RPC_E_CHANGED_MODE
            throw;
    }

    if (!hwnd) {
        LOG_ERROR(LOG_MEDIA, "[SMTC] NULL HWND provided");
        return;
    }

    try {
        auto interop = winrt::get_activation_factory<
            SystemMediaTransportControls,
            ISystemMediaTransportControlsInterop>();

        SystemMediaTransportControls smtc{nullptr};
        winrt::check_hresult(interop->GetForWindow(
            hwnd,
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

        // auto_revoke ensures in-flight callbacks complete before revoker destruction,
        // preventing use-after-free if a callback is executing during ~WindowsMediaBackend
        state_->button_revoker = state_->smtc.ButtonPressed(winrt::auto_revoke,
            [this](SystemMediaTransportControls const&,
                   SystemMediaTransportControlsButtonPressedEventArgs const& args) {
                onButtonPressed(static_cast<int>(args.Button()));
            });

        state_->seek_revoker = state_->smtc.PlaybackPositionChangeRequested(winrt::auto_revoke,
            [this](SystemMediaTransportControls const&,
                   PlaybackPositionChangeRequestedEventArgs const& args) {
                if (session_->onSeek) {
                    auto pos_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        args.RequestedPlaybackPosition()).count();
                    session_->onSeek(pos_us);
                }
            });

        LOG_INFO(LOG_MEDIA, "[SMTC] Initialized");
    } catch (const winrt::hresult_error& e) {
        LOG_ERROR(LOG_MEDIA, "[SMTC] Init failed: %ls", e.message().c_str());
    }
}

WindowsMediaBackend::~WindowsMediaBackend() {
    if (!state_) return;

    try {
        // Revokers run first (unique_ptr destroys WinRTState), blocking until
        // any in-flight callbacks complete. Then we clean up display state.
        state_->button_revoker.revoke();
        state_->seek_revoker.revoke();
        state_->updater.ClearAll();
        state_->updater.Update();
        state_->smtc.IsEnabled(false);
    } catch (const winrt::hresult_error&) {}
}

void WindowsMediaBackend::onButtonPressed(int button) {
    using B = SystemMediaTransportControlsButton;
    switch (static_cast<B>(button)) {
        case B::Play:
            if (session_->onPlay) session_->onPlay();
            break;
        case B::Pause:
            if (session_->onPause) session_->onPause();
            break;
        case B::Stop:
            if (session_->onStop) session_->onStop();
            break;
        case B::Next:
            if (session_->onNext) session_->onNext();
            break;
        case B::Previous:
            if (session_->onPrevious) session_->onPrevious();
            break;
        default:
            break;
    }
}

void WindowsMediaBackend::setMetadata(const MediaMetadata& meta) {
    metadata_ = meta;
    if (playback_state_ != PlaybackState::Stopped)
        updateDisplayProperties();
}

void WindowsMediaBackend::setArtwork(const std::string& dataUri) {
    if (!state_ || dataUri.empty()) return;

    // Parse data URI: data:<mime>;base64,<payload>
    size_t comma = dataUri.find(',');
    if (comma == std::string::npos) return;
    const char* b64_ptr = dataUri.c_str() + comma + 1;
    DWORD b64_len = static_cast<DWORD>(dataUri.size() - comma - 1);

    // Base64 decode
    DWORD decoded_len = 0;
    if (!CryptStringToBinaryA(b64_ptr, b64_len,
                               CRYPT_STRING_BASE64, nullptr, &decoded_len,
                               nullptr, nullptr)) {
        return;
    }

    std::vector<uint8_t> buf(decoded_len);
    if (!CryptStringToBinaryA(b64_ptr, b64_len,
                               CRYPT_STRING_BASE64, buf.data(), &decoded_len,
                               nullptr, nullptr)) {
        return;
    }
    buf.resize(decoded_len);

    try {
        InMemoryRandomAccessStream stream;
        DataWriter writer(stream);
        writer.WriteBytes(winrt::array_view<const uint8_t>(buf));
        writer.StoreAsync().get();  // Safe: called from MTA (MediaSessionThread)
        writer.DetachStream();

        stream.Seek(0);
        auto ref = RandomAccessStreamReference::CreateFromStream(stream);
        state_->cached_thumbnail = ref;
        state_->updater.Thumbnail(ref);
        state_->updater.Update();
    } catch (const winrt::hresult_error&) {}
}

void WindowsMediaBackend::setPlaybackState(PlaybackState state) {
    playback_state_ = state;
    if (!state_) return;

    switch (state) {
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
}

void WindowsMediaBackend::setPosition(int64_t position_us) {
    position_us_ = position_us;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_position_update_).count();

    if (pending_update_ || elapsed >= 1000) {
        updateTimeline();
        last_position_update_ = now;
        pending_update_ = false;
    }
}

void WindowsMediaBackend::setVolume(double) {
    // SMTC does not expose a volume property
}

void WindowsMediaBackend::setCanGoNext(bool can) {
    if (state_) state_->smtc.IsNextEnabled(can);
}

void WindowsMediaBackend::setCanGoPrevious(bool can) {
    if (state_) state_->smtc.IsPreviousEnabled(can);
}

void WindowsMediaBackend::setRate(double) {
    // SMTC has no explicit playback rate property
}

void WindowsMediaBackend::emitSeeked(int64_t position_us) {
    position_us_ = position_us;
    updateTimeline();
    last_position_update_ = std::chrono::steady_clock::now();
    pending_update_ = false;
}

void WindowsMediaBackend::updateDisplayProperties() {
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

void WindowsMediaBackend::updateTimeline() {
    if (!state_ || metadata_.duration_us <= 0) return;

    try {
        SystemMediaTransportControlsTimelineProperties tl;

        // TimeSpan uses 100-nanosecond ticks
        auto to_ticks = [](int64_t us) -> TimeSpan {
            return TimeSpan{us * 10};
        };

        tl.StartTime(TimeSpan{0});
        tl.EndTime(to_ticks(metadata_.duration_us));
        tl.Position(to_ticks(position_us_));
        tl.MinSeekTime(TimeSpan{0});
        tl.MaxSeekTime(to_ticks(metadata_.duration_us));

        state_->smtc.UpdateTimelineProperties(tl);
    } catch (const winrt::hresult_error&) {}
}

std::unique_ptr<MediaSessionBackend> createWindowsMediaBackend(
    MediaSession* session, HWND hwnd) {
    return std::make_unique<WindowsMediaBackend>(session, hwnd);
}

#endif // _WIN32
