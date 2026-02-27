#ifndef PLAYERCOMPONENT_H
#define PLAYERCOMPONENT_H

#include <QObject>
#include <QtCore/qglobal.h>
#include <QVariant>
#include <QSet>
#include <QQuickWindow>
#include <QTimer>
#include <QTextStream>

#include <functional>

#include "ComponentManager.h"
#include "QtHelper.h"

#include <mpv/client.h>

class MpvController;
class AlbumArtProvider;

///////////////////////////////////////////////////////////////////////////////////////////////////
class PlayerComponent : public ComponentBase
{
  Q_OBJECT
  DEFINE_SINGLETON(PlayerComponent);

public:
  const char* componentName() override { return "player"; }
  bool componentExport() override { return true; }
  bool componentInitialize() override;
  void componentPostInitialize() override;
  
  explicit PlayerComponent(QObject* parent = nullptr);
  ~PlayerComponent() override;

  // Deprecated. Corresponds to stop() + queueMedia().
  Q_INVOKABLE bool load(const QString& url, const QVariantMap& options, const QVariantMap& metadata, const QVariant& audioStream = QVariant(), const QVariant& subtitleStream = QVariant());

  // Append a media item to the internal playlist. If nothing is played yet, the
  // newly appended item will start playing immediately.
  // options:
  //  - startMilliseconds: start playback at this time (in ms)
  //  - autoplay: if false, start playback paused; if true, start normally
  Q_INVOKABLE void queueMedia(const QString& url, const QVariantMap& options, const QVariantMap &metadata, const QVariant& audioStream, const QVariant& subtitleStream);

  // This clears all items queued with queueMedia().
  // It explicitly excludes the currently playing item. The main use of this function
  // is updating the next item that should be played (for the purpose of gapless audio).
  // If you want to wipe everything, use stop().
  Q_INVOKABLE void clearQueue();

  Q_INVOKABLE virtual void seekTo(qint64 ms);

  // Stop playback and clear all queued items.
  Q_INVOKABLE virtual void stop();

  // A full reload of the stream is imminent (stop() + load())
  // Used for not resetting display mode with the next stop() call.
  Q_INVOKABLE virtual void streamSwitch();

  Q_INVOKABLE virtual void pause();
  Q_INVOKABLE virtual void play();

  // OS media integration notifications (called from JavaScript)
  Q_INVOKABLE void notifyShuffleChange(bool enabled);
  Q_INVOKABLE void notifyRepeatChange(const QString& mode);
  Q_INVOKABLE void notifyFullscreenChange(bool isFullscreen);
  Q_INVOKABLE void notifyRateChange(double rate);
  Q_INVOKABLE void notifyQueueChange(bool canNext, bool canPrevious);
  Q_INVOKABLE void notifyPlaybackStop(bool isNavigating);
  Q_INVOKABLE void notifyDurationChange(qint64 durationMs);
  Q_INVOKABLE void notifyPlaybackState(const QString& state);
  Q_INVOKABLE void notifyPosition(qint64 positionMs);
  Q_INVOKABLE void notifySeek(qint64 positionMs);
  Q_INVOKABLE void notifyMetadata(const QVariantMap& metadata);
  Q_INVOKABLE void notifyVolumeChange(double volume);

  // 0-100 volume 0=mute and 100=normal
  // Ignored if no audio output active (e.g. when no file is playing).
  Q_INVOKABLE virtual void setVolume(int volume);
  // Returns 0 if no audio output active.
  Q_INVOKABLE virtual int volume();

  // Ignored if no audio output active.
  Q_INVOKABLE virtual void setMuted(bool muted);
  // Returns 0 if no audio output active.
  Q_INVOKABLE virtual bool muted();

  // Returns a QVariant of the following format:
  // QVariantList                   (list of audio device entries)
  //    QVariantMap                 (an audio device entry)
  //      "name" -> QString         (symbolic name/ID of the device)
  //      "description" -> QString  (human readable description intended for display)
  //
  Q_INVOKABLE virtual QVariant getAudioDeviceList();
  // Uses the "name" from the device list.
  Q_INVOKABLE virtual void setAudioDevice(const QString& name);
  
  Q_INVOKABLE virtual void setAudioStream(const QVariant& audioStream);
  Q_INVOKABLE virtual void setSubtitleStream(const QVariant& subtitleStream);

  Q_INVOKABLE virtual void setAudioDelay(qint64 milliseconds);
  Q_INVOKABLE virtual void setSubtitleDelay(qint64 milliseconds);

  // If enabled, hide the web view (whether it's OSD or not), and show video
  // only. If no video is running, render a black background only.
  Q_INVOKABLE virtual void setVideoOnlyMode(bool enable);

  Q_INVOKABLE void userCommand(QString command);

  // Set the region in which video should be rendered. This uses Qt pixel
  // coordinates (x=0,y=0 is the top/left corner).
  // If the aspect ratio mismatches, the video will be letterboxed or pillarboxed.
  // The lower/right pixel border is always excluded.
  // setVideoRectangle(-1, -1, -1 , -1) will revert to the default and
  // automatically use the whole window. (Same if the rectangle is 0-sized.)
  Q_INVOKABLE void setVideoRectangle(int x, int y, int w, int h);

  Q_INVOKABLE void setPlaybackRate(int rate);

  Q_INVOKABLE qint64 getPosition();
  Q_INVOKABLE qint64 getDuration();

  Q_INVOKABLE QVariantList getWebPlaylist() const;
  Q_INVOKABLE QString getCurrentWebPlaylistItemId() const;
  Q_INVOKABLE void setWebPlaylist(const QVariantList& playlist, const QString& currentItemId);

  QRect videoRectangle() { return m_videoRectangle; }

  AlbumArtProvider* albumArtProvider() const { return m_albumArtProvider; }

  void setMpvController(MpvController* controller) {
    if (!m_mpv)
      m_mpv = controller;
  }
  void initializeMpv();

  virtual void setWindow(QQuickWindow* window);

  QString videoInformation() const;

  static QStringList AudioCodecsAll() { return { "ac3", "dts", "eac3", "dts-hd", "truehd" }; };
  static QStringList AudioCodecsSPDIF() { return { "ac3", "dts" }; };

  enum class State {
    finished,
    canceled,
    error,
    paused,
    playing,
    buffering,
  };

  enum class MediaType {
    Subtitle,
    Audio,
  };
  
public Q_SLOTS:
  void updateAudioDeviceList();
  void setAudioConfiguration();
  void setSubtitleConfiguration();
  void setVideoConfiguration();
  void setOtherConfiguration();
  void updateAudioConfiguration();
  void updateSubtitleConfiguration();
  void updateVideoConfiguration();
  void updateConfiguration();

private Q_SLOTS:
  void handleMpvEvents();
  void onRestoreDisplay();
  void onRefreshRateChange();
  void updateAudioDevice();

Q_SIGNALS:
  // The following signals correspond to the State enum above.
  void playing();                 // playback is progressing (audio playing, pictures are moving)
  void buffering(float percent);  // temporary state during "playing", or during media loading
  void paused();                  // paused (covers all sub-states)
  void finished();                // playback finished successfully
  void canceled();                // playback was stopped (by user/via API)
  void error(const QString& msg); // playback stopped due to external error
  // To be phased out. Raised on finished() and canceled().
  void stopped();                 // playback finished successfully, or was stopped with stop()
  void stateChanged(State newState, State oldState); // all state changes

  // true if the video (or music) is actually playing
  // false if nothing is loaded, playback is paused, during seeking, or media is being loaded
  void videoPlaybackActive(bool active);
  void windowVisible(bool visible);
  // emitted as soon as the duration of the current file is known
  void updateDuration(qint64 milliseconds);
  void playbackRateChanged(double rate);

  // current position in ms should be triggered 2 times a second
  // when position updates
  void positionUpdate(quint64);

  void onVideoRecangleChanged();

  void onMpvEvents();

  void onMetaData(const QVariantMap &meta, QUrl baseUrl);

  void webPlaylistChanged(const QVariantList& playlist, const QString& currentItemId);

  // OS media integration signals (for MPRIS, SMTC, etc.)
  void shuffleChanged(bool enabled);
  void repeatChanged(const QString& mode);
  void fullscreenChanged(bool isFullscreen);
  void rateChanged(double rate);
  void queueChanged(bool canNext, bool canPrevious);
  void playbackStopped(bool isNavigating);
  void durationChanged(qint64 durationMs);
  void playbackStateChanged(const QString& state);
  void positionChanged(qint64 positionMs);
  void seekPerformed(qint64 positionMs);
  void metadataChanged(const QVariantMap& metadata);
  void volumeChanged(double volume);

  // Buffered ranges from demuxer-cache-state, as list of {start, end} in ticks
  void bufferedRangesUpdated(const QVariantList& ranges);

private:
  // this is the function actually implemented in the backends. the variantmap contains
  // a few known keys:
  // * subtitleStreamIndex
  // * subtitleStreamIdentifier
  // * audioStreamIndex
  // * audioStreamIdentifier
  // * viewOffset
  //
  void loadWithOptions(const QVariantMap& options);
  void setQtQuickWindow(QQuickWindow* window);
  void updatePlaybackState();
  void handleMpvEvent(mpv_event *event);
  // Potentially switch the display refresh rate, and return true if the refresh rate
  // was actually changed.
  bool switchDisplayFrameRate();
  void checkCurrentAudioDevice(const QSet<QString>& old_devs, const QSet<QString>& new_devs);
  void appendAudioFormat(QTextStream& info, const QString& property) const;
  void updateVideoAspectSettings();
  QVariantList findStreamsForURL(const QString &url);
  void reselectStream(const QVariant &streamSelection, MediaType target);

  MpvController* m_mpv = nullptr;

  State m_state;
  bool m_paused;
  bool m_playbackActive;
  bool m_windowVisible;
  bool m_videoPlaybackActive;
  bool m_inPlayback;
  bool m_playbackCanceled;
  QString m_playbackError;
  int m_bufferingPercentage;
  int m_lastBufferingPercentage;
  double m_lastPositionUpdate;
  qint64 m_playbackAudioDelay;
  QQuickWindow* m_window;
  float m_mediaFrameRate;
  QTimer m_restoreDisplayTimer;
  QTimer m_reloadAudioTimer;
  QSet<QString> m_audioDevices;
  bool m_streamSwitchImminent;
  bool m_doAc3Transcoding;
  QStringList m_passthroughCodecs;
  QVariantMap m_serverMediaInfo;
  QVariant m_currentSubtitleStream;
  QVariant m_currentAudioStream;
  QRect m_videoRectangle;

  QVariantList m_webPlaylist;
  QString m_currentWebPlaylistItemId;
  QTimer* m_playlistTimer;
  QVariantList m_queuedItems;

  AlbumArtProvider* m_albumArtProvider;
};

#endif // PLAYERCOMPONENT_H
