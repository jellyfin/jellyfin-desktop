#include <QtTest/QtTest>
#include "ui/WindowManager.h"
#include "settings/SettingsComponent.h"
#include "settings/SettingsSection.h"

class TestWindowManager : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase();
  void testFullscreenExit_data();
  void testFullscreenExit();
  void testFullscreenExitBeforeMaximizedPlayback();

private:
  SettingsSection* m_main = nullptr;
};

void TestWindowManager::initTestCase()
{
  m_main = new SettingsSection(SETTINGS_SECTION_MAIN, PLATFORM_ANY, -1,
                               &SettingsComponent::Get());
  m_main->registerSetting(new SettingsValue("fullscreen", false, PLATFORM_ANY, m_main));
  m_main->registerSetting(new SettingsValue("forceAlwaysFS", false, PLATFORM_ANY, m_main));
  SettingsComponent::Get().registerSection(m_main);
}

void TestWindowManager::testFullscreenExit_data()
{
  QTest::addColumn<QWindow::Visibility>("restoredVisibility");
  QTest::newRow("windowed") << QWindow::Windowed;
  QTest::newRow("maximized") << QWindow::Maximized;
}

void TestWindowManager::testFullscreenExit()
{
  QFETCH(QWindow::Visibility, restoredVisibility);
  m_main->setValueNoSave("fullscreen", false);
  WindowManager manager;
  QSignalSpy switched(&manager, &WindowManager::fullScreenSwitched);
  QSignalSpy updated(m_main, &SettingsSection::valuesUpdated);

  QVERIFY(QMetaObject::invokeMethod(&manager, "onVisibilityChanged", Qt::DirectConnection,
                                   Q_ARG(QWindow::Visibility, QWindow::FullScreen)));
  QCOMPARE(m_main->value("fullscreen").toBool(), true);

  QVERIFY(QMetaObject::invokeMethod(&manager, "onVisibilityChanged", Qt::DirectConnection,
                                   Q_ARG(QWindow::Visibility, restoredVisibility)));
  QCOMPARE(m_main->value("fullscreen").toBool(), false);
  QCOMPARE(switched.count(), 2);
  QCOMPARE(updated.count(), 2);

  // Repeated visibility notifications must not announce another fullscreen change.
  QVERIFY(QMetaObject::invokeMethod(&manager, "onVisibilityChanged", Qt::DirectConnection,
                                   Q_ARG(QWindow::Visibility, restoredVisibility)));
  QCOMPARE(switched.count(), 2);
  QCOMPARE(updated.count(), 2);
}

void TestWindowManager::testFullscreenExitBeforeMaximizedPlayback()
{
  m_main->setValueNoSave("fullscreen", false);
  WindowManager manager;
  const QWindow::Visibility sequence[] = {
    QWindow::FullScreen, QWindow::Windowed, QWindow::Maximized,
    QWindow::FullScreen, QWindow::Maximized
  };

  for (auto visibility : sequence)
  {
    QVERIFY(QMetaObject::invokeMethod(&manager, "onVisibilityChanged", Qt::DirectConnection,
                                     Q_ARG(QWindow::Visibility, visibility)));
    QCOMPARE(m_main->value("fullscreen").toBool(), visibility == QWindow::FullScreen);
  }
}

QTEST_MAIN(TestWindowManager)
#include "test_windowmanager.moc"
