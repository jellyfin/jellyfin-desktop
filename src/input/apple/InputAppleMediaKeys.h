//
// Created by Tobias Hieta on 21/08/15.
//

#ifndef KONVERGO_INPUTAPPLEMEDIAKEYS_H
#define KONVERGO_INPUTAPPLEMEDIAKEYS_H

#include "input/InputComponent.h"
#include "player/PlayerComponent.h"

class InputAppleMediaKeys : public InputBase
{
  Q_OBJECT
public:
  explicit InputAppleMediaKeys(QObject* parent = nullptr) : InputBase(parent) { }
  bool initInput() override;
  const char* inputName() override { return "AppleMediaKeys"; }

private slots:
  void handleStateChanged(PlayerComponent::State newState);
  void handlePositionUpdate(quint64 position);
  void handleUpdateDuration(qint64 duration);
  void handleUpdateMetaData(const QVariantMap& meta);
  void handleAlbumArtReady(const QByteArray& imageData);
  void handleAlbumArtUnavailable();

private:
  void* m_delegate;

  typedef void (*SetNowPlayingVisibilityFunc)(void* origin, int visibility);
  typedef void* (*GetLocalOriginFunc)(void);
  typedef void (*SetCanBeNowPlayingApplicationFunc)(int);
  SetNowPlayingVisibilityFunc SetNowPlayingVisibility;
  GetLocalOriginFunc GetLocalOrigin;
  SetCanBeNowPlayingApplicationFunc SetCanBeNowPlayingApplication;

  void ensureNowPlayingVisibility();

  bool m_pendingUpdate;
  quint64 m_currentTime;
  PlayerComponent::State m_currentState = PlayerComponent::State::finished;
};

#endif //KONVERGO_INPUTAPPLEMEDIAKEYS_H
