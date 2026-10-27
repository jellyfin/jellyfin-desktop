#include "MprisComponent.h"
#include "MprisRootAdaptor.h"
#include "MprisPlayerAdaptor.h"
#include "player/PlayerComponent.h"
#include "player/AlbumArtProvider.h"
#include "input/InputComponent.h"
#include "system/SystemComponent.h"
#include "settings/SettingsComponent.h"
#include "core/Globals.h"
#include "core/ProfileManager.h"
#include "ui/WindowManager.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusError>
#include <QApplication>
#include <QDebug>
#include <QFileInfo>

#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"

MprisComponent::MprisComponent(QObject* parent)
  : ComponentBase(parent)
  , m_enabled(false)
  , m_player(nullptr)
  , m_playbackStatus("Stopped")
  , m_position(0)
  , m_duration(0)
  , m_positionTimer(nullptr)
  , m_canGoNext(false)
  , m_canGoPrevious(false)
  , m_shuffle(false)
  , m_loopStatus("None")
  , m_rate(1.0)
  , m_volume(1.0)
  , m_seekPending(false)
  , m_expectedPosition(0)
  , m_isNavigating(false)
  , m_playerState(PlayerComponent::State::finished)
{
}

MprisComponent::~MprisComponent()
{
  if (m_enabled)
  {
    disconnectPlayerSignals();
    QDBusConnection::sessionBus().unregisterService(m_serviceName);
    QDBusConnection::sessionBus().unregisterObject(MPRIS_OBJECT_PATH);
  }
}

bool MprisComponent::componentInitialize()
{
  if (!QDBusConnection::sessionBus().isConnected())
  {
    qDebug() << "D-Bus session bus not available, MPRIS disabled";
    return true;
  }

  // Generate profile-specific service name using cache dir basename (profile ID)
  QString profileCacheDir = ProfileManager::activeProfile().cacheDir();
  m_serviceName = QString("org.mpris.MediaPlayer2.JellyfinDesktop.profile_%1").arg(QFileInfo(profileCacheDir).fileName());

  qDebug() << "Attempting to register MPRIS service:" << m_serviceName;
  if (!QDBusConnection::sessionBus().registerService(m_serviceName))
  {
    qWarning() << "Failed to register MPRIS service:"
               << QDBusConnection::sessionBus().lastError().message();
    return true;
  }
  qDebug() << "MPRIS service registered successfully";

  m_rootAdaptor = std::make_unique<MprisRootAdaptor>(this);
  m_playerAdaptor = std::make_unique<MprisPlayerAdaptor>(this);

  if (!QDBusConnection::sessionBus().registerObject(MPRIS_OBJECT_PATH, this))
  {
    qWarning() << "Failed to register MPRIS object:"
               << QDBusConnection::sessionBus().lastError().message();
    QDBusConnection::sessionBus().unregisterService(m_serviceName);
    return true;
  }

  m_enabled = true;
  qDebug() << "MPRIS interface registered successfully";

  return true;
}

void MprisComponent::componentPostInitialize()
{
  if (!m_enabled)
    return;

  m_player = &PlayerComponent::Get();
  connectPlayerSignals();

  // Connect to PlayerComponent media integration signals
  connect(m_player, &PlayerComponent::shuffleChanged, this, &MprisComponent::notifyShuffleChange);
  connect(m_player, &PlayerComponent::repeatChanged, this, &MprisComponent::notifyRepeatChange);
  connect(m_player, &PlayerComponent::fullscreenChanged, this, &MprisComponent::notifyFullscreenChange);
  connect(m_player, &PlayerComponent::rateChanged, this, &MprisComponent::notifyRateChange);
  connect(m_player, &PlayerComponent::queueChanged, this, &MprisComponent::notifyQueueChange);
  connect(m_player, &PlayerComponent::playbackStopped, this, &MprisComponent::notifyPlaybackStop);
  connect(m_player, &PlayerComponent::durationChanged, this, &MprisComponent::notifyDurationChange);
  connect(m_player, &PlayerComponent::playbackStateChanged, this, &MprisComponent::notifyPlaybackState);
  connect(m_player, &PlayerComponent::positionChanged, this, &MprisComponent::notifyPosition);
  connect(m_player, &PlayerComponent::seekPerformed, this, &MprisComponent::notifySeek);
  connect(m_player, &PlayerComponent::metadataChanged, this, &MprisComponent::notifyMetadata);
  connect(m_player, &PlayerComponent::volumeChanged, this, &MprisComponent::notifyVolumeChange);

  // Connect to AlbumArtProvider signals
  if (m_player->albumArtProvider())
  {
    connect(m_player->albumArtProvider(), &AlbumArtProvider::artworkReady,
            this, &MprisComponent::onAlbumArtReady);
    connect(m_player->albumArtProvider(), &AlbumArtProvider::artworkUnavailable,
            this, &MprisComponent::onAlbumArtUnavailable);
  }

  m_positionTimer = new QTimer(this);
  m_positionTimer->setInterval(500);
  connect(m_positionTimer, &QTimer::timeout, [this]() {
    if (m_playbackStatus == "Playing")
    {
    }
  });
}

void MprisComponent::connectPlayerSignals()
{
  if (!m_player)
    return;
}

void MprisComponent::disconnectPlayerSignals()
{
  if (!m_player)
    return;

  disconnect(m_player, nullptr, this, nullptr);
}

QString MprisComponent::playbackStatus() const
{
  return m_playbackStatus;
}

double MprisComponent::volume() const
{
  return m_volume;
}

qint64 MprisComponent::position() const
{
  return m_position;
}

QString MprisComponent::loopStatus() const
{
  return m_loopStatus;
}

double MprisComponent::rate() const
{
  return m_rate;
}

bool MprisComponent::shuffle() const
{
  return (m_currentMediaType == "Audio") ? m_shuffle : false;
}

bool MprisComponent::canGoNext() const
{
  return m_canGoNext && m_playbackStatus != "Stopped";
}

bool MprisComponent::canGoPrevious() const
{
  return m_canGoPrevious && m_playbackStatus != "Stopped";
}

bool MprisComponent::canPlay() const
{
  return m_playbackStatus != "Stopped";
}

bool MprisComponent::canPause() const
{
  return m_playbackStatus != "Stopped";
}

bool MprisComponent::canSeek() const
{
  if (m_playbackStatus == "Stopped")
    return false;

  bool result = m_duration > 0;
  qDebug() << "MPRIS: canSeek() called - duration:" << m_duration << "result:" << result;
  return result;
}

bool MprisComponent::canControl() const
{
  if (m_playbackStatus == "Stopped")
    return false;

  return m_playerState == PlayerComponent::State::paused ||
         m_playerState == PlayerComponent::State::playing ||
         m_playerState == PlayerComponent::State::buffering;
}

void MprisComponent::Raise()
{
  QQuickWindow* window = Globals::MainWindow();
  if (window)
  {
    window->raise();
    window->requestActivate();
  }
}

void MprisComponent::Quit()
{
}

bool MprisComponent::fullscreen() const
{
  return WindowManager::Get().isFullScreen();
}

void MprisComponent::setFullscreen(bool value)
{
  bool currentState = WindowManager::Get().isFullScreen();
  if (currentState != value)
  {
    WindowManager::Get().setFullScreen(value);
    qDebug() << "MPRIS: Fullscreen changed to:" << value;
    emitPropertyChange("org.mpris.MediaPlayer2", "Fullscreen", value);
  }
}

void MprisComponent::Next()
{
  qDebug() << "MPRIS: Next track requested";
  InputComponent::Get().sendAction("next");
}

void MprisComponent::Previous()
{
  qDebug() << "MPRIS: Previous track requested";
  InputComponent::Get().sendAction("previous");
}

void MprisComponent::Pause()
{
  qDebug() << "MPRIS: Pause requested";
  InputComponent::Get().sendAction("pause");
}

void MprisComponent::PlayPause()
{
  qDebug() << "MPRIS: PlayPause requested";
  InputComponent::Get().sendAction("play_pause");
}

void MprisComponent::Stop()
{
  qDebug() << "MPRIS: Stop requested";
  InputComponent::Get().sendAction("stop");
}

void MprisComponent::Play()
{
  qDebug() << "MPRIS: Play requested";
  InputComponent::Get().sendAction("play");
}

void MprisComponent::Seek(qint64 offset)
{
  if (!canSeek())
    return;

  qint64 newPos = m_position + offset;
  if (newPos < 0)
    newPos = 0;
  if (newPos > m_duration)
    newPos = m_duration;

  m_seekPending = true;
  m_expectedPosition = newPos;

  qDebug() << "MPRIS: Seek requested to" << (newPos / 1000) << "ms";
  InputComponent::Get().seekTo(newPos / 1000);
}

void MprisComponent::SetPosition(const QDBusObjectPath& trackId, qint64 position)
{
  if (!canSeek())
    return;

  if (trackId.path() != m_currentTrackId)
    return;

  if (position >= 0 && position <= m_duration)
  {
    m_seekPending = true;
    m_expectedPosition = position;

    qDebug() << "MPRIS: SetPosition requested to" << (position / 1000) << "ms";
    InputComponent::Get().seekTo(position / 1000);
  }
}

void MprisComponent::OpenUri(const QString& uri)
{
  (void)uri;
}

void MprisComponent::setVolume(double volume)
{
  m_volume = qBound(0.0, volume, 1.0);

  int vol = static_cast<int>(m_volume * 100);
  InputComponent::Get().setVolume(vol);
}

void MprisComponent::setLoopStatus(const QString& value)
{
  if (value == "None" || value == "Track" || value == "Playlist")
  {
    QString action;
    if (value == "None")
      action = "repeatnone";
    else if (value == "Track")
      action = "repeatone";
    else if (value == "Playlist")
      action = "repeatall";

    InputComponent::Get().sendAction(action);

    qDebug() << "MPRIS: Loop status change requested:" << value << "via action:" << action;
  }
}

void MprisComponent::setRate(double value)
{
  value = qBound(0.25, value, 2.0);

  qDebug() << "MPRIS: Playback rate change requested:" << value;
  InputComponent::Get().setRate(value);
}

void MprisComponent::setShuffle(bool value)
{
  if (m_currentMediaType != "Audio")
    return;

  QString action = value ? "shuffle" : "sorted";
  InputComponent::Get().sendAction(action);

  qDebug() << "MPRIS: Shuffle change requested:" << value << "via action:" << action;
}

/////////////////////////////////////////////////////////////////////////////////////////
void MprisComponent::notifyShuffleChange(bool enabled)
{
  qDebug() << "MPRIS: Received shuffle change notification from web client:" << enabled;
  if (m_shuffle != enabled)
  {
    m_shuffle = enabled;
    emitPropertyChange("org.mpris.MediaPlayer2.Player", "Shuffle", shuffle());
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void MprisComponent::notifyRepeatChange(const QString& mode)
{
  qDebug() << "MPRIS: Received repeat change notification from web client:" << mode;

  QString loopStatus;
  if (mode == "RepeatAll")
    loopStatus = "Playlist";
  else if (mode == "RepeatOne")
    loopStatus = "Track";
  else
    loopStatus = "None";

  if (m_loopStatus != loopStatus)
  {
    m_loopStatus = loopStatus;
    emitPropertyChange("org.mpris.MediaPlayer2.Player", "LoopStatus", loopStatus);
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void MprisComponent::notifyFullscreenChange(bool isFullscreen)
{
  qDebug() << "MPRIS: Received fullscreen change notification from web client:" << isFullscreen;
  emitPropertyChange("org.mpris.MediaPlayer2", "Fullscreen", isFullscreen);
}

void MprisComponent::notifyRateChange(double rate)
{
  qDebug() << "MPRIS: Received playback rate change notification from web client:" << rate;
  if (m_rate != rate)
  {
    m_rate = rate;
    emitPropertyChange("org.mpris.MediaPlayer2.Player", "Rate", rate);
  }
}

void MprisComponent::notifyPlaybackStop(bool isNavigating)
{
  qDebug() << "MPRIS: Playback stop notification - isNavigating:" << isNavigating;
  m_isNavigating = isNavigating;

  notifyPlaybackState("Stopped");
}

void MprisComponent::notifyDurationChange(qint64 durationMs)
{
  qDebug() << "MPRIS: Duration change notification from JS:" << durationMs << "ms";

  qint64 durationMicroseconds = durationMs * 1000;

  if (m_duration != durationMicroseconds)
  {
    m_duration = durationMicroseconds;
    m_metadata["mpris:length"] = QVariant::fromValue(m_duration);

    if (m_playbackStatus == "Playing")
    {
      QVariantMap properties;
      properties["Metadata"] = m_metadata;
      properties["CanSeek"] = canSeek();
      emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);
    }
  }
}

void MprisComponent::notifyQueueChange(bool canNext, bool canPrevious)
{
  qDebug() << "MPRIS: Queue change - canNext:" << canNext << "canPrevious:" << canPrevious
           << "status:" << m_playbackStatus << "state:" << static_cast<int>(m_playerState);

  bool changed = false;

  if (m_canGoNext != canNext)
  {
    m_canGoNext = canNext;
    changed = true;
  }

  if (m_canGoPrevious != canPrevious)
  {
    m_canGoPrevious = canPrevious;
    changed = true;
  }

  if (changed)
  {
    QVariantMap properties;
    properties["CanGoNext"] = m_canGoNext;
    properties["CanGoPrevious"] = m_canGoPrevious;

    if (!canNext && !canPrevious && m_playbackStatus == "Stopped")
    {
      qDebug() << "MPRIS: Queue empty and stopped, clearing metadata";
      m_metadata.clear();
      m_currentTrackId.clear();
      m_currentArtDataUri.clear();
      properties["Metadata"] = m_metadata;
      properties["Position"] = static_cast<qint64>(0);
    }

    emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);
  }
}

void MprisComponent::notifyPlaybackState(const QString& state)
{
  qDebug() << "MPRIS: Playback state change from JS:" << state;

  if (state == "Playing")
  {
    m_playbackStatus = "Playing";
    m_playerState = PlayerComponent::State::playing;
    updateNavigationCapabilities();
    bool seekable = canSeek();

    QVariantMap properties;
    properties["PlaybackStatus"] = "Playing";
    properties["CanPlay"] = canPlay();
    properties["CanPause"] = canPause();
    properties["CanSeek"] = seekable;
    properties["CanControl"] = canControl();
    properties["Rate"] = m_rate;
    properties["Metadata"] = m_metadata;
    emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);

    if (m_positionTimer)
      m_positionTimer->start();
  }
  else if (state == "Paused")
  {
    m_playbackStatus = "Paused";
    m_playerState = PlayerComponent::State::paused;
    updateNavigationCapabilities();

    QVariantMap properties;
    properties["PlaybackStatus"] = "Paused";
    properties["CanPlay"] = canPlay();
    properties["CanPause"] = canPause();
    properties["CanControl"] = canControl();
    emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);

    if (m_positionTimer)
      m_positionTimer->stop();
  }
  else if (state == "Stopped")
  {
    if (m_positionTimer)
      m_positionTimer->stop();

    m_position = 0;
    m_duration = 0;

    m_playbackStatus = "Stopped";
    m_playerState = PlayerComponent::State::finished;

    if (!m_isNavigating)
    {
      qDebug() << "MPRIS: Stopped without navigation, clearing metadata";
      m_metadata.clear();
      m_currentTrackId.clear();
      m_currentArtDataUri.clear();
    }
    else
    {
      qDebug() << "MPRIS: Stopped during navigation, keeping metadata for transition";
    }

    m_isNavigating = false;

    updateNavigationCapabilities();
    bool seekable = canSeek();

    QVariantMap properties;
    properties["PlaybackStatus"] = "Stopped";
    properties["CanPlay"] = canPlay();
    properties["CanPause"] = canPause();
    properties["CanSeek"] = seekable;
    properties["CanControl"] = canControl();
    properties["Position"] = static_cast<qint64>(0);
    properties["Metadata"] = m_metadata;
    emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);
  }
}

void MprisComponent::notifyPosition(qint64 positionMs)
{
  m_position = positionMs * 1000;

  if (m_seekPending && m_playerAdaptor) {
    m_seekPending = false;
    qDebug() << "MPRIS: MPRIS-initiated seek completed, emitting Seeked signal at" << m_position << "us";
    Q_EMIT m_playerAdaptor->Seeked(m_position);
  }
}

void MprisComponent::notifySeek(qint64 positionMs)
{
  m_position = positionMs * 1000;

  if (m_playerAdaptor) {
    qDebug() << "MPRIS: Player-initiated seek, emitting Seeked signal at" << m_position << "us";
    Q_EMIT m_playerAdaptor->Seeked(m_position);
  }

  m_seekPending = false;
}

void MprisComponent::notifyMetadata(const QVariantMap& metadata)
{
  qDebug() << "MPRIS: Metadata update from JS";
  updateMetadata(metadata);
}

void MprisComponent::notifyVolumeChange(double volume)
{
  double newVolume = qBound(0.0, volume, 1.0);
  if (m_volume != newVolume)
  {
    m_volume = newVolume;
    emitPropertyChange("org.mpris.MediaPlayer2.Player", "Volume", m_volume);
  }
}

void MprisComponent::onPlayerPlaying()
{
  qDebug() << "MPRIS: Player playing";

  m_playbackStatus = "Playing";
  updateNavigationCapabilities();
  bool seekable = canSeek();

  QVariantMap properties;
  properties["PlaybackStatus"] = "Playing";
  properties["CanPlay"] = canPlay();
  properties["CanPause"] = canPause();
  properties["CanSeek"] = seekable;
  properties["CanControl"] = canControl();
  properties["Rate"] = m_rate;
  properties["Metadata"] = m_metadata;
  emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);

  if (m_positionTimer)
    m_positionTimer->start();
}

void MprisComponent::onPlayerPaused()
{
  qDebug() << "MPRIS: Player paused";
  updatePlaybackStatus("Paused");
  if (m_positionTimer)
    m_positionTimer->stop();
}

void MprisComponent::onPlayerStopped()
{
  qDebug() << "MPRIS: Player stopped";
  if (m_positionTimer)
    m_positionTimer->stop();

  m_position = 0;
  m_duration = 0;

  m_playbackStatus = "Stopped";
  updateNavigationCapabilities();
  bool seekable = canSeek();

  QVariantMap properties;
  properties["PlaybackStatus"] = "Stopped";
  properties["CanPlay"] = canPlay();
  properties["CanPause"] = canPause();
  properties["CanSeek"] = seekable;
  properties["CanControl"] = canControl();
  properties["Position"] = static_cast<qint64>(0);
  emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);
}

void MprisComponent::onPlayerFinished()
{
  updatePlaybackStatus("Stopped");
  if (m_positionTimer)
    m_positionTimer->stop();

  qDebug() << "MPRIS: Playback finished, clearing metadata";
  m_position = 0;
  m_duration = 0;
  m_metadata.clear();
  m_currentTrackId.clear();
  m_canGoNext = false;
  m_canGoPrevious = false;
  m_currentArtDataUri.clear();

  QVariantMap properties;
  properties["Metadata"] = m_metadata;
  properties["Position"] = static_cast<qint64>(m_position);
  properties["CanGoNext"] = false;
  properties["CanGoPrevious"] = false;
  properties["CanControl"] = canControl();
  emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);
}

void MprisComponent::onPlayerStateChanged(PlayerComponent::State newState, PlayerComponent::State oldState)
{
  qDebug() << "MPRIS: State changed from" << static_cast<int>(oldState) << "to" << static_cast<int>(newState);
  m_playerState = newState;

  if (newState == PlayerComponent::State::finished)
  {
    if (!m_canGoNext)
    {
      qDebug() << "MPRIS: Finished with no next item, clearing metadata";
      m_metadata.clear();
      m_currentTrackId.clear();
      m_currentArtDataUri.clear();

      QVariantMap properties;
      properties["Metadata"] = m_metadata;
      emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);
    }
    else
    {
      qDebug() << "MPRIS: Finished but next item available, keeping metadata";
    }
  }
  else if (newState == PlayerComponent::State::canceled)
  {
    if (!m_isNavigating)
    {
      qDebug() << "MPRIS: Canceled (exiting playback), clearing metadata";
      m_metadata.clear();
      m_currentTrackId.clear();
      m_canGoNext = false;
      m_canGoPrevious = false;
      m_currentArtDataUri.clear();

      QVariantMap properties;
      properties["Metadata"] = m_metadata;
      properties["CanGoNext"] = false;
      properties["CanGoPrevious"] = false;
      emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);
    }
    else
    {
      qDebug() << "MPRIS: Canceled during navigation, keeping metadata (will be updated by queueMedia)";
    }

    m_isNavigating = false;
  }
  else if (newState == PlayerComponent::State::error)
  {
    qDebug() << "MPRIS: Clearing metadata due to error state";
    m_metadata.clear();
    m_currentTrackId.clear();
    m_currentArtDataUri.clear();

    QVariantMap properties;
    properties["Metadata"] = m_metadata;
    emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);
  }
}

void MprisComponent::onPlayerPositionUpdate(quint64 position)
{
  m_position = position * 1000;

  if (m_seekPending && m_playerAdaptor) {
    m_seekPending = false;
    Q_EMIT m_playerAdaptor->Seeked(m_position);
  }
}

void MprisComponent::onPlayerDurationChanged(qint64 duration)
{
  qint64 mpvDuration = duration * 1000;

  qDebug() << "MPRIS: MPV duration:" << mpvDuration << "Jellyfin duration:" << m_duration;

  m_duration = mpvDuration;

  m_metadata["mpris:length"] = QVariant::fromValue(m_duration);

  qDebug() << "MPRIS: Duration updated, CanSeek:" << canSeek();

  if (m_playbackStatus == "Playing")
  {
    QVariantMap properties;
    properties["Metadata"] = m_metadata;
    properties["CanSeek"] = canSeek();
    emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);
  }
}

void MprisComponent::onPlayerMetaData(const QVariantMap& metadata)
{
  qDebug() << "MPRIS: Received metadata from PlayerComponent:" << metadata.value("Name").toString();
  updateMetadata(metadata);
}

void MprisComponent::onPlayerVolumeChanged()
{
  emitPropertyChange("org.mpris.MediaPlayer2.Player", "Volume", volume());
}

void MprisComponent::onShuffleModeChanged(bool shuffleEnabled)
{
  if (m_currentMediaType != "Audio")
    return;

  m_shuffle = shuffleEnabled;
  qDebug() << "MPRIS: Shuffle mode changed from web client to:" << shuffleEnabled;
  emitPropertyChange("org.mpris.MediaPlayer2.Player", "Shuffle", shuffle());
}

void MprisComponent::onRepeatModeChanged(const QString& repeatMode)
{
  if (m_currentMediaType != "Audio")
    return;

  QString mprisLoopStatus;
  if (repeatMode == "RepeatNone")
    mprisLoopStatus = "None";
  else if (repeatMode == "RepeatOne")
    mprisLoopStatus = "Track";
  else if (repeatMode == "RepeatAll")
    mprisLoopStatus = "Playlist";
  else {
    qWarning() << "MPRIS: Unknown repeat mode from web client:" << repeatMode;
    return;
  }

  m_loopStatus = mprisLoopStatus;
  qDebug() << "MPRIS: Repeat mode changed from web client:" << repeatMode << "-> MPRIS:" << mprisLoopStatus;
  emitPropertyChange("org.mpris.MediaPlayer2.Player", "LoopStatus", loopStatus());
}

void MprisComponent::updatePlaybackStatus(const QString& status)
{
  if (m_playbackStatus != status)
  {
    m_playbackStatus = status;

    updateNavigationCapabilities();
    bool seekable = canSeek();
    qDebug() << "MPRIS: Emitting CanSeek on status change:" << seekable << "status:" << status << "duration:" << m_duration;

    QVariantMap properties;
    properties["PlaybackStatus"] = status;
    properties["CanPlay"] = canPlay();
    properties["CanPause"] = canPause();
    properties["CanSeek"] = seekable;
    properties["CanControl"] = canControl();
    properties["Rate"] = m_rate;
    emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);
  }
}

void MprisComponent::updateMetadata(const QVariantMap& jellyfinMeta)
{
  QVariantMap mprisMeta;

  m_currentTrackId = generateTrackId();
  mprisMeta["mpris:trackid"] = QVariant::fromValue(QDBusObjectPath(m_currentTrackId));

  QString mediaType = jellyfinMeta.value("MediaType").toString();
  QString itemType = jellyfinMeta.value("Type").toString();

  m_currentMediaType = mediaType;

  if (jellyfinMeta.contains("Name"))
    mprisMeta["xesam:title"] = jellyfinMeta["Name"];

  if (jellyfinMeta.contains("RunTimeTicks"))
  {
    qint64 runTimeTicks = jellyfinMeta["RunTimeTicks"].toLongLong();
    qint64 durationMicroseconds = runTimeTicks / 10;
    m_duration = durationMicroseconds;
    qDebug() << "MPRIS: Got duration from Jellyfin metadata:" << m_duration << "us";
  }

  if (m_duration > 0)
    mprisMeta["mpris:length"] = QVariant::fromValue(m_duration);

  if (mediaType == "Audio" || itemType == "Audio")
  {
    if (jellyfinMeta.contains("Artists"))
    {
      QStringList artists = jellyfinMeta["Artists"].toStringList();
      if (!artists.isEmpty())
        mprisMeta["xesam:artist"] = QVariant::fromValue(artists);
    }

    if (jellyfinMeta.contains("AlbumArtist"))
      mprisMeta["xesam:albumArtist"] = QVariant::fromValue(QStringList{jellyfinMeta["AlbumArtist"].toString()});

    if (jellyfinMeta.contains("Album"))
      mprisMeta["xesam:album"] = jellyfinMeta["Album"];

    if (jellyfinMeta.contains("IndexNumber"))
      mprisMeta["xesam:trackNumber"] = jellyfinMeta["IndexNumber"];

    if (jellyfinMeta.contains("ProductionYear"))
      mprisMeta["xesam:contentCreated"] = jellyfinMeta["ProductionYear"].toString();
  }
  else if (itemType == "Episode")
  {
    QString title = jellyfinMeta.value("Name").toString();
    QString series = jellyfinMeta.value("SeriesName").toString();
    int episode = jellyfinMeta.value("IndexNumber", 0).toInt();

    mprisMeta["xesam:title"] = title;

    if (!series.isEmpty())
      mprisMeta["xesam:artist"] = QVariant::fromValue(QStringList{series});

    if (jellyfinMeta.contains("SeasonName"))
      mprisMeta["xesam:album"] = jellyfinMeta["SeasonName"].toString();

    if (episode > 0)
      mprisMeta["xesam:trackNumber"] = episode;

    if (jellyfinMeta.contains("ProductionYear"))
      mprisMeta["xesam:contentCreated"] = jellyfinMeta["ProductionYear"].toString();
  }
  else if (itemType == "Movie")
  {
    if (jellyfinMeta.contains("ProductionYear"))
      mprisMeta["xesam:contentCreated"] = jellyfinMeta["ProductionYear"].toString();
  }

  // Album art will be set asynchronously via onAlbumArtReady signal
  // Keep existing data URI if available
  if (!m_currentArtDataUri.isEmpty())
    mprisMeta["mpris:artUrl"] = m_currentArtDataUri;

  mprisMeta["mpris:length"] = QVariant::fromValue(m_duration);

  m_metadata = mprisMeta;

  updateNavigationCapabilities();

  bool seekable = canSeek();
  qDebug() << "MPRIS: Emitting metadata, CanSeek:" << seekable << "duration:" << m_duration;

  QVariantMap properties;
  properties["Metadata"] = m_metadata;

  static bool lastCanSeek = false;
  if (seekable != lastCanSeek)
  {
    properties["CanSeek"] = seekable;
    lastCanSeek = seekable;
  }

  if (m_currentMediaType == "Audio") {
    static bool lastShuffle = false;
    static QString lastLoopStatus = "";

    bool currentShuffle = shuffle();
    QString currentLoopStatus = loopStatus();

    if (currentShuffle != lastShuffle)
    {
      properties["Shuffle"] = currentShuffle;
      lastShuffle = currentShuffle;
    }

    if (currentLoopStatus != lastLoopStatus)
    {
      properties["LoopStatus"] = currentLoopStatus;
      lastLoopStatus = currentLoopStatus;
    }
  }

  emitMultiplePropertyChanges("org.mpris.MediaPlayer2.Player", properties);
}

void MprisComponent::emitPropertyChange(const QString& interface,
                                        const QString& property,
                                        const QVariant& value)
{
  QVariantMap changedProps;
  changedProps[property] = value;

  QDBusMessage signal = QDBusMessage::createSignal(
    MPRIS_OBJECT_PATH,
    "org.freedesktop.DBus.Properties",
    "PropertiesChanged"
  );

  signal << interface << changedProps << QStringList();
  QDBusConnection::sessionBus().send(signal);
}

void MprisComponent::emitMultiplePropertyChanges(const QString& interface,
                                                 const QVariantMap& properties)
{
  QDBusMessage signal = QDBusMessage::createSignal(
    MPRIS_OBJECT_PATH,
    "org.freedesktop.DBus.Properties",
    "PropertiesChanged"
  );

  signal << interface << properties << QStringList();
  QDBusConnection::sessionBus().send(signal);
}

QString MprisComponent::generateTrackId() const
{
  static int trackCounter = 0;
  return QString("/org/mpris/MediaPlayer2/Track/%1").arg(++trackCounter);
}

void MprisComponent::onAlbumArtReady(const QByteArray& imageData, const QString& mimeType)
{
  // Create data URI from the downloaded image
  QString dataUri = QString("data:%1;base64,%2")
                      .arg(mimeType)
                      .arg(QString::fromLatin1(imageData.toBase64()));

  m_currentArtDataUri = dataUri;

  if (!m_metadata.isEmpty())
  {
    m_metadata["mpris:artUrl"] = dataUri;
    emitPropertyChange("org.mpris.MediaPlayer2.Player", "Metadata", m_metadata);
  }
}

void MprisComponent::onAlbumArtUnavailable()
{
  // No action needed - metadata remains without artwork
}

void MprisComponent::updateNavigationCapabilities()
{
}
