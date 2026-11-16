#include <QtTest/QtTest>
#include <QQuickWindow>
#include <MpvController>
#include "../src/player/MpvD3D11Item.h"

class TestMpvD3D11 : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase();
  void testRenderingAndResize();
  void testRecreateWhilePlaying();
  void testRecreateRendererWhilePlaying_data();
  void testRecreateRendererWhilePlaying();
  void testMoveBetweenWindows();
};

static bool hasTestPattern(const QImage& image)
{
  if (image.isNull())
    return false;

  const QColor top = image.pixelColor(image.width() / 2, image.height() / 4);
  const QColor bottom = image.pixelColor(image.width() / 2, image.height() * 3 / 4);
  return top.red() > 200 && top.green() < 40 && top.blue() < 40
      && bottom.blue() > 200 && bottom.red() < 40 && bottom.green() < 40;
}

static int playTestPattern(MpvD3D11Item& item, bool withAudio = false)
{
  QByteArray source = "av://lavfi:color=c=red:s=64x64:r=30,drawbox=x=0:y=32:w=64:h=32:color=blue:t=fill";
  if (withAudio)
  {
    const int result = mpv_set_property_string(item.mpvController()->mpv(), "ao", "null");
    if (result < 0)
      return result;
    source += "[out0];anullsrc=r=48000:cl=stereo[out1]";
  }
  const char* command[]{
    "loadfile",
    source.constData(),
    nullptr
  };
  return mpv_command_async(item.mpvController()->mpv(), 0, command);
}

void TestMpvD3D11::initTestCase()
{
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
}

void TestMpvD3D11::testRenderingAndResize()
{
  QQuickWindow window;
  window.resize(320, 180);
  MpvD3D11Item item(window.contentItem());
  item.setSize(window.size());
  QSignalSpy ready(&item, &MpvD3D11Item::ready);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));
  QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 10000);
  QCOMPARE(playTestPattern(item), 0);
  QTRY_VERIFY_WITH_TIMEOUT(hasTestPattern(window.grabWindow()), 10000);

  window.resize(480, 270);
  item.setSize(window.size());
  QTRY_VERIFY_WITH_TIMEOUT(hasTestPattern(window.grabWindow()), 10000);
  QCOMPARE(ready.count(), 1);
}

void TestMpvD3D11::testRecreateWhilePlaying()
{
  QQuickWindow window;
  window.resize(320, 180);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  for (int i = 0; i < 2; ++i)
  {
    auto item = std::make_unique<MpvD3D11Item>(window.contentItem());
    item->setSize(window.size());
    QSignalSpy ready(item.get(), &MpvD3D11Item::ready);
    window.grabWindow();
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 10000);
    QCOMPARE(playTestPattern(*item), 0);
    QTRY_VERIFY_WITH_TIMEOUT(hasTestPattern(window.grabWindow()), 10000);
    item.reset();
    window.grabWindow();
  }
}

void TestMpvD3D11::testRecreateRendererWhilePlaying_data()
{
  QTest::addColumn<bool>("paused");
  QTest::addColumn<bool>("withAudio");
  QTest::newRow("playing-video-only") << false << false;
  QTest::newRow("paused-video-only") << true << false;
  QTest::newRow("playing-audio-video") << false << true;
}

void TestMpvD3D11::testRecreateRendererWhilePlaying()
{
  QFETCH(bool, paused);
  QFETCH(bool, withAudio);
  QQuickWindow window;
  window.setPersistentSceneGraph(false);
  window.setPersistentGraphics(false);
  window.resize(320, 180);
  MpvD3D11Item item(window.contentItem());
  item.setSize(window.size());
  auto* controller = item.mpvController();
  QSignalSpy ready(&item, &MpvD3D11Item::ready);
  int invalidated = 0;
  QObject invalidationReceiver;
  connect(&window, &QQuickWindow::sceneGraphInvalidated, &invalidationReceiver,
          [&invalidated]() { ++invalidated; }, Qt::QueuedConnection);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));
  QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
  QCOMPARE(playTestPattern(item, withAudio), 0);
  QTRY_VERIFY_WITH_TIMEOUT(hasTestPattern(window.grabWindow()), 10000);
  int pause = paused;
  QCOMPARE(mpv_set_property(controller->mpv(), "pause", MPV_FORMAT_FLAG, &pause), 0);

  for (int i = 1; i <= 2; ++i)
  {
    window.hide();
    window.releaseResources();
    QTRY_COMPARE_WITH_TIMEOUT(invalidated, i, 10000);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), i + 1, 10000);
    QCOMPARE(item.mpvController(), controller);
    QTRY_VERIFY_WITH_TIMEOUT(hasTestPattern(window.grabWindow()), 10000);
    pause = !paused;
    QCOMPARE(mpv_get_property(controller->mpv(), "pause", MPV_FORMAT_FLAG, &pause), 0);
    QCOMPARE(pause, int(paused));
  }
}

void TestMpvD3D11::testMoveBetweenWindows()
{
  QQuickWindow first;
  QQuickWindow second;
  first.resize(320, 180);
  second.resize(320, 180);
  MpvD3D11Item item(first.contentItem());
  item.setSize(first.size());
  QSignalSpy ready(&item, &MpvD3D11Item::ready);
  first.show();
  QVERIFY(QTest::qWaitForWindowExposed(&first));
  QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
  QCOMPARE(playTestPattern(item), 0);
  QTRY_VERIFY_WITH_TIMEOUT(hasTestPattern(first.grabWindow()), 10000);

  for (int i = 0; i < 2; ++i)
  {
    auto& target = i == 0 ? second : first;
    target.show();
    QVERIFY(QTest::qWaitForWindowExposed(&target));
    item.setParentItem(target.contentItem());
    target.grabWindow();
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), i + 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(hasTestPattern(target.grabWindow()), 10000);
  }
}

QTEST_MAIN(TestMpvD3D11)
#include "test_mpvd3d11.moc"
