#include <QtTest/QtTest>
#include "player/PlayerComponent.h"
#include "ui/WindowManager.h"

class TestCursorIdle : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase();
  void testVideoPlaybackStateTransitions();
  void testAudioPlaybackNotConsideredVideo();
  void testCursorVisibilityBasic();
};

void TestCursorIdle::initTestCase()
{
}

void TestCursorIdle::testVideoPlaybackStateTransitions()
{
  PlayerComponent& player = PlayerComponent::Get();

  QSignalSpy spy(&player, &PlayerComponent::videoPlayingChanged);
  QVERIFY(spy.isValid());

  // Initially stopped
  player.notifyPlaybackStop(false);
  QCOMPARE(player.isVideoPlaying(), false);
  spy.clear();

  // Video metadata received, playback starts
  QVariantMap videoMeta;
  videoMeta["MediaType"] = "Video";
  player.notifyMetadata(videoMeta);
  player.notifyPlaybackState("Playing");

  QCOMPARE(player.isVideoPlaying(), true);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.takeFirst().at(0).toBool(), true);

  // Video paused
  player.notifyPlaybackState("Paused");
  QCOMPARE(player.isVideoPlaying(), false);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.takeFirst().at(0).toBool(), false);

  // Video resumed
  player.notifyPlaybackState("Playing");
  QCOMPARE(player.isVideoPlaying(), true);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.takeFirst().at(0).toBool(), true);

  // Video stopped
  player.notifyPlaybackStop(false);
  QCOMPARE(player.isVideoPlaying(), false);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.takeFirst().at(0).toBool(), false);
}

void TestCursorIdle::testAudioPlaybackNotConsideredVideo()
{
  PlayerComponent& player = PlayerComponent::Get();

  QSignalSpy spy(&player, &PlayerComponent::videoPlayingChanged);
  QVERIFY(spy.isValid());

  player.notifyPlaybackStop(false);
  QCOMPARE(player.isVideoPlaying(), false);
  spy.clear();

  // Audio metadata received, playback starts
  QVariantMap audioMeta;
  audioMeta["MediaType"] = "Audio";
  player.notifyMetadata(audioMeta);
  player.notifyPlaybackState("Playing");

  // Audio playback should not be considered video playing
  QCOMPARE(player.isVideoPlaying(), false);
  QCOMPARE(spy.count(), 0);

  player.notifyPlaybackStop(false);
  QCOMPARE(player.isVideoPlaying(), false);
}

void TestCursorIdle::testCursorVisibilityBasic()
{
  WindowManager& win = WindowManager::Get();

  // Test explicit visibility setting
  win.setCursorVisibility(false);
  win.setCursorVisibility(true);

  // Test resetCursorIdleTimer callable
  win.resetCursorIdleTimer();
}

QTEST_MAIN(TestCursorIdle)
#include "test_cursor_idle.moc"

