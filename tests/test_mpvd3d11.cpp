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

static int playTestPattern(MpvD3D11Item& item)
{
  const char* command[]{
    "loadfile",
    "av://lavfi:color=c=red:s=64x64:r=30,drawbox=x=0:y=32:w=64:h=32:color=blue:t=fill",
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
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 10000);
    QCOMPARE(playTestPattern(*item), 0);
    QTRY_VERIFY_WITH_TIMEOUT(hasTestPattern(window.grabWindow()), 10000);
    item.reset();
    window.grabWindow();
  }
}

QTEST_MAIN(TestMpvD3D11)
#include "test_mpvd3d11.moc"
