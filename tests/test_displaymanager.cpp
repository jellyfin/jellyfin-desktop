#include <QtTest/QtTest>
#include "../src/display/DisplayManager.h"
#include "../src/settings/SettingsComponent.h"
#include "../src/settings/SettingsSection.h"
#include "../src/settings/SettingsValue.h"

// Implements the pure virtuals (and some helpers) to test DisplayManager logic.
class TestableDisplayManager : public DisplayManager
{
public:
  explicit TestableDisplayManager() : DisplayManager(nullptr), m_currentMode(0) {}

  bool setDisplayMode(int, int) override { return true; }
  int getCurrentDisplayMode(int) override { return m_currentMode; }
  int getMainDisplay() override { return 0; }
  int getDisplayFromPoint(int, int) override { return 0; }

  void setCurrentMode(int mode) { m_currentMode = mode; }

  void addMode(int id, int w, int h, int bpp, float hz, bool interlaced)
  {
    auto mode = DMVideoModePtr::create();
    mode->m_id = id;
    mode->m_width = w;
    mode->m_height = h;
    mode->m_bitsPerPixel = bpp;
    mode->m_refreshRate = hz;
    mode->m_interlaced = interlaced;
    mode->m_privId = id;

    if (!m_displays.contains(0))
    {
      auto disp = DMDisplayPtr::create();
      disp->m_id = 0;
      disp->m_name = "TestDisplay";
      disp->m_privId = 0;
      m_displays[0] = disp;
    }
    m_displays[0]->m_videoModes[id] = mode;
  }

private:
  int m_currentMode;
};


class TestDisplayManager : public QObject
{
  Q_OBJECT

private:
  void setAvoid25_30(bool on)
  {
    auto* section = SettingsComponent::Get().getSection(SETTINGS_SECTION_VIDEO);
    section->setValue("refreshrate.avoid_25hz_30hz", on);
  }

private slots:
  void initTestCase();
  void testExactRefreshRateMatch();
  void testPrefers60HzOver30HzForFilm();
  void testCloseRefreshRateMatch();
  void testNoMatchWithDifferentResolution();
  void testInterlacedPreference();
  void testCurrentModeBonusBreaksTie();
  void testAvoid25And30Hz();
  void testNoSuitableModeReturnsNegative();
  void testFindBestMode();
  void testMultipleRefreshRate();
};

// Helper function: Populates a DisplayManager with some typical display modes.
static void setupStandardModes(TestableDisplayManager& mgr)
{
  // id, w, h, bpp, hz, interlaced
  mgr.addMode(0, 1920, 1080, 32, 24.0f, false);
  mgr.addMode(1, 1920, 1080, 32, 30.0f, false);
  mgr.addMode(2, 1920, 1080, 32, 50.0f, false);
  mgr.addMode(3, 1920, 1080, 32, 60.0f, false); // This will be set as currentMode (see below)
  mgr.addMode(4, 1920, 1080, 32, 23.976f, false);
  mgr.addMode(5, 1920, 1080, 32, 59.94f, false);
  mgr.setCurrentMode(3);
}

void TestDisplayManager::initTestCase()
{
  auto* section = new SettingsSection(SETTINGS_SECTION_VIDEO, PLATFORM_ANY, -1,
                                      &SettingsComponent::Get());
  auto* avoidSetting = new SettingsValue("refreshrate.avoid_25hz_30hz",
                                          false, PLATFORM_ANY, section);
  section->registerSetting(avoidSetting);
  SettingsComponent::Get().registerSection(section);
}

void TestDisplayManager::testExactRefreshRateMatch()
{
  TestableDisplayManager mgr;
  setupStandardModes(mgr);
  DMMatchMediaInfo match(24.0f, false);

  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 0); // 24 hz mode
}

void TestDisplayManager::testPrefers60HzOver30HzForFilm()
{
  // Prefer 60 hz for 30 fps content.
  TestableDisplayManager mgr;
  setupStandardModes(mgr);
  DMMatchMediaInfo match(30.0f, false);

  setAvoid25_30(true);
  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 3);
  setAvoid25_30(false);
}

void TestDisplayManager::testCloseRefreshRateMatch()
{
  // Prefer 24hz for 23.976 fps content.
  TestableDisplayManager mgr;
  setupStandardModes(mgr);
  DMMatchMediaInfo match(23.976f, false);

  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 4);
}

void TestDisplayManager::testNoMatchWithDifferentResolution()
{
  // Prefer 1080p over 720p, regardless of framerate.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1920, 1080, 32, 60.0f, false);
  mgr.addMode(1, 1280, 720,  32, 24.0f, false);
  mgr.setCurrentMode(0);

  DMMatchMediaInfo match(24.0f, false);
  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 0);
}

void TestDisplayManager::testInterlacedPreference()
{
  // Prefer interlaced mode for interlaced content.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1920, 1080, 32, 50.0f, false);
  mgr.addMode(1, 1920, 1080, 32, 50.0f, true);
  mgr.setCurrentMode(0); // non-interlaced

  DMMatchMediaInfo match(50.0f, true);
  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 1);
}

void TestDisplayManager::testCurrentModeBonusBreaksTie()
{
  // Prefer current mode when multiple identical modes available.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1920, 1080, 32, 60.0f, false);
  mgr.addMode(1, 1920, 1080, 32, 60.0f, false);
  mgr.setCurrentMode(1);

  DMMatchMediaInfo match(60.0f, false);
  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 1);
}

void TestDisplayManager::testAvoid25And30Hz()
{
  // Prefer 50 hz for 25 fps content when avoid25_30 is enabled.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1920, 1080, 32, 25.0f, false);
  mgr.addMode(1, 1920, 1080, 32, 50.0f, false);
  mgr.setCurrentMode(1);

  DMMatchMediaInfo match(25.0f, false);
  setAvoid25_30(true);
  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 1);

  // Disabling avoid25_30 should produce the opposite result.
  setAvoid25_30(false);
  best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 0);
}

void TestDisplayManager::testNoSuitableModeReturnsNegative()
{
  // No preference when resolution doesn't match content.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1920, 1080, 32, 60.0f, false);
  mgr.addMode(1, 1280, 720,  32, 24.0f, false);
  mgr.setCurrentMode(0);
  mgr.m_displays[0]->m_videoModes.remove(0);

  DMMatchMediaInfo match(24.0f, false);
  int best = mgr.findBestMatch(0, match);

  QCOMPARE(best, -1);
}

void TestDisplayManager::testFindBestMode()
{
  // Prefer greatest refresh rate, greatest resolution, and non-interlaced.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1280, 720,  32, 60.0f, false);
  mgr.addMode(1, 1920, 1080, 32, 60.0f, false);
  mgr.addMode(2, 1920, 1080, 32, 30.0f, true);
  mgr.addMode(3, 1920, 1080, 32, 60.0f, true);

  int best = mgr.findBestMode(0);
  QCOMPARE(best, 1);
}

void TestDisplayManager::testMultipleRefreshRate()
{
  // Prefer exact multiple of fps rate.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1920, 1080, 32, 47.952f, false);
  mgr.addMode(1, 1920, 1080, 32, 60.0f,   false);
  mgr.setCurrentMode(1);

  DMMatchMediaInfo match(23.976f, false);
  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 0);
}

QTEST_APPLESS_MAIN(TestDisplayManager)
#include "test_displaymanager.moc"
