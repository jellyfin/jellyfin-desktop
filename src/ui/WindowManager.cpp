#include "WindowManager.h"
#include "core/Globals.h"
#include "settings/SettingsComponent.h"
#include "settings/SettingsSection.h"
#include "player/PlayerComponent.h"
#include "display/DisplayComponent.h"
#include "taskbar/TaskbarComponent.h"
#include "input/InputComponent.h"
#include "utils/Utils.h"

#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QDebug>

///////////////////////////////////////////////////////////////////////////////////////////////////
WindowManager& WindowManager::Get()
{
  static WindowManager instance;
  return instance;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
WindowManager::WindowManager(QObject* parent)
  : ComponentBase(parent),
    m_window(nullptr),
    m_webView(nullptr),
    m_enforcingZoom(false),
    m_ignoreFullscreenSettingsChange(0),
    m_cursorVisible(true),
    m_cursorInsideWindow(true),
    m_previousVisibility(QWindow::Windowed),
    m_geometrySaveTimer(nullptr),
    m_pipMode(false),
    m_pipTogglingTitleBar(false),
    m_pipEnforcingAspect(false),
    m_pipAspectRatio(0),
    m_prePipFlags(),
    m_prePipVisibility(QWindow::Windowed),
    m_initialSize(),
    m_initialScreenSize()
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////
WindowManager::~WindowManager()
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::componentPostInitialize()
{
  // Window not available yet - will be initialized via initializeWindow()
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::initializeWindow(QQuickWindow* window)
{
  m_window = window;

  if (!m_window)
  {
    qCritical() << "WindowManager: Failed to get main window";
    return;
  }

  // Initialize components that need window reference
  PlayerComponent::Get().setWindow(m_window);
  DisplayComponent::Get().setApplicationWindow(m_window);
  TaskbarComponent::Get().setWindow(m_window);

  // Install event filter to track cursor enter/leave
  m_window->installEventFilter(this);

  // Register host commands
  InputComponent::Get().registerHostCommand("fullscreen", this, "toggleFullscreen");
  InputComponent::Get().registerHostCommand("togglePip", this, "togglePiP");

  // Load and apply saved geometry
  loadGeometry();

  // Connect to settings
  connectSettings();

  // Apply initial settings
  applySettings();

  // Setup screen management
  updateScreens();

  // Debounced disk sync timer (30s)
  // Values are written to memory immediately, timer syncs to disk
  m_geometrySaveTimer = new QTimer(this);
  m_geometrySaveTimer->setSingleShot(true);
  m_geometrySaveTimer->setInterval(30000);
  connect(m_geometrySaveTimer, &QTimer::timeout, this, []() {
    // STATE section is storage, so use saveStorage()
    SettingsComponent::Get().saveStorage();
  });

  // Connect to window visibility changes (for fullscreen tracking)
  connect(m_window, &QQuickWindow::visibilityChanged,
          this, &WindowManager::onVisibilityChanged);

  // Separate handlers for size and position
  // Use deferred save to ensure windowState is updated before checking
  // (geometry signals fire before state signals during maximize transition)
  auto scheduleSizeSave = [this]() {
    QTimer::singleShot(0, this, [this]() {
      if (m_window) {
        saveWindowSize();
        m_geometrySaveTimer->start();  // Debounced disk sync
      }
    });
  };

  auto schedulePositionSave = [this]() {
    QTimer::singleShot(0, this, [this]() {
      if (m_window) {
        saveWindowPosition();
        m_geometrySaveTimer->start();  // Debounced disk sync
      }
    });
  };

  // Connect to window state changes (for maximize tracking)
  connect(m_window, &QWindow::windowStateChanged, this, scheduleSizeSave);

  // Size tracking
  connect(m_window, &QQuickWindow::widthChanged, this, scheduleSizeSave);
  connect(m_window, &QQuickWindow::heightChanged, this, scheduleSizeSave);

  // Position tracking only on non-Wayland (Wayland compositor controls positioning)
  if (!isWayland())
  {
    connect(m_window, &QQuickWindow::xChanged, this, schedulePositionSave);
    connect(m_window, &QQuickWindow::yChanged, this, schedulePositionSave);
  }

  // Connect to application shutdown
  connect(qApp, &QGuiApplication::aboutToQuit,
          this, &WindowManager::saveGeometrySlot);

  // Auto-exit PiP when playback stops (user navigated away from player)
  connect(&PlayerComponent::Get(), &PlayerComponent::playbackStopped,
          this, [this](bool isNavigating) {
            if (m_pipMode && !isNavigating)
              setPiPMode(false);
          });

  // Find web view and connect to zoom changes
  m_webView = m_window->findChild<QQuickItem*>("web");
  if (m_webView)
  {
    connect(m_webView, SIGNAL(zoomFactorChanged()), this, SLOT(onZoomFactorChanged()));
    enforceZoom();
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::setAlwaysOnTop(bool enable)
{
  if (!m_window)
    return;

  Qt::WindowFlags flags = m_window->flags();
  Qt::WindowFlags onTopFlags = Qt::WindowStaysOnTopHint;

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
  // X11 needs bypass hint, Wayland doesn't support it
  if (QGuiApplication::platformName() == "xcb")
    onTopFlags |= Qt::X11BypassWindowManagerHint;
#endif

  if (enable)
    m_window->setFlags(flags | onTopFlags);
  else
    m_window->setFlags(flags & ~onTopFlags);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool WindowManager::isAlwaysOnTop() const
{
  if (!m_window)
    return false;

  Qt::WindowFlags checkFlags = Qt::WindowStaysOnTopHint;

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
  if (QGuiApplication::platformName() == "xcb")
    checkFlags |= Qt::X11BypassWindowManagerHint;
#endif

  return (m_window->flags() & checkFlags) == checkFlags;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::toggleAlwaysOnTop()
{
  setAlwaysOnTop(!isAlwaysOnTop());
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool WindowManager::isWayland() const
{
  return QGuiApplication::platformName() == "wayland";
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::setFullScreen(bool enable)
{
  if (!m_window)
    return;

  if (enable)
  {
    if (m_pipMode)
      setPiPMode(false);

    // Use showFullScreen()
    m_window->showFullScreen();
    updateForcedScreen();
  }
  else
  {
    // Exit fullscreen: restore to previous state
    qDebug() << "setFullScreen(false): m_previousVisibility=" << m_previousVisibility
             << "(Maximized=" << QWindow::Maximized << ")";
    if (m_previousVisibility == QWindow::Maximized)
    {
      // show() + showMaximized()
      qDebug() << "setFullScreen(false): show + showMaximized";
      m_window->show();
      m_window->showMaximized();
    }
    else
    {
      qDebug() << "setFullScreen(false): restoring to windowed";
      m_window->showNormal();
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool WindowManager::isFullScreen() const
{
  if (!m_window)
    return false;

  return m_window->visibility() == QWindow::FullScreen;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::toggleFullscreen()
{
  setFullScreen(!isFullScreen());
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::setCursorVisibility(bool visible)
{
  // Always show cursor in PiP mode
  if (m_pipMode)
    visible = true;

  if (visible == m_cursorVisible)
    return;

  m_cursorVisible = visible;

  if (visible)
    qApp->restoreOverrideCursor();
  else
    qApp->setOverrideCursor(QCursor(Qt::BlankCursor));

#ifdef Q_OS_MAC
  // Only apply macOS global cursor hiding when cursor is inside window
  if (m_cursorInsideWindow)
    OSXUtils::SetCursorVisible(visible);
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool WindowManager::eventFilter(QObject* watched, QEvent* event)
{
  if (watched == m_window)
  {
    if (event->type() == QEvent::Enter)
    {
      m_cursorInsideWindow = true;
#ifdef Q_OS_MAC
      // Re-hide cursor if it should be hidden
      if (!m_cursorVisible)
        OSXUtils::SetCursorVisible(false);
#endif
      // Show title bar in PiP mode to allow dragging
      setPipTitleBar(true);
    }
    else if (event->type() == QEvent::Leave)
    {
      m_cursorInsideWindow = false;
#ifdef Q_OS_MAC
      // Always show cursor when leaving window
      OSXUtils::SetCursorVisible(true);
#endif
      // Hide title bar in PiP mode when mouse leaves
      setPipTitleBar(false);
    }
    else if (event->type() == QEvent::Close)
    {
      // In PiP mode, close button exits PiP and pauses playback
      if (m_pipMode)
      {
        event->ignore();
        PlayerComponent::Get().pause();
        setPiPMode(false);
        return true;
      }
    }
  }
  return ComponentBase::eventFilter(watched, event);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::raiseWindow()
{
  if (!m_window)
    return;

  // Restore from minimized state if needed
  if (m_window->windowState() & Qt::WindowMinimized)
    m_window->setWindowState(static_cast<Qt::WindowState>(m_window->windowState() & ~Qt::WindowMinimized));

  // Raise and request activation
  // Note: Wayland blocks requestActivate() for security (prevents focus stealing)
  // On Wayland, window will be raised but user must click to activate
  m_window->show();
  m_window->raise();
  m_window->requestActivate();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::onVisibilityChanged(QWindow::Visibility visibility)
{
  qDebug() << "onVisibilityChanged: visibility=" << visibility
           << "m_previousVisibility=" << m_previousVisibility;

  bool isFS = (visibility == QWindow::FullScreen);
  bool wasFS = (m_previousVisibility == QWindow::FullScreen);

  // Kiosk mode: force back to fullscreen if user tried to exit
  if (!isFS)
  {
    bool forceAlwaysFS = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "forceAlwaysFS").toBool();
    if (forceAlwaysFS)
    {
      setFullScreen(true);
      return;
    }
  }

  // Track previous visibility (only when NOT fullscreen or hidden)
  // Preserve pre-fullscreen state for restore
  if (visibility != QWindow::FullScreen && visibility != QWindow::Hidden)
  {
    qDebug() << "onVisibilityChanged: updating m_previousVisibility from" << m_previousVisibility << "to" << visibility;
    m_previousVisibility = visibility;
  }
  else
  {
    qDebug() << "onVisibilityChanged: NOT updating m_previousVisibility (visibility is FS or Hidden)";
  }

  // Update fullscreen setting (in-memory only) when state changes
  if (isFS != wasFS)
  {
    SettingsSection* section = SettingsComponent::Get().getSection(SETTINGS_SECTION_MAIN);
    if (section)
      section->setValueNoSave("fullscreen", isFS);

    emit fullScreenSwitched();
  }

  updateWindowState(false);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::onScreenAdded(QScreen* screen)
{
  if (!screen)
    return;

  qDebug() << "Screen added:" << screen->name();

  // Connect to new screen's signals
  connect(screen, &QScreen::geometryChanged,
          this, &WindowManager::onScreenGeometryChanged);
  connect(screen, &QScreen::logicalDotsPerInchChanged,
          this, &WindowManager::onScreenDpiChanged);

  updateScreens();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::onScreenRemoved(QScreen* screen)
{
  qDebug() << "Screen removed:" << (screen ? screen->name() : "unknown");

  updateScreens();

  // Check if window was on removed screen
  if (m_window && screen && m_currentScreenName == screen->name())
  {
    qDebug() << "Window was on removed screen, moving to primary";
    QScreen* primary = QGuiApplication::primaryScreen();
    if (primary)
      m_window->setScreen(primary);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::onScreenGeometryChanged(const QRect& geometry)
{
  Q_UNUSED(geometry);
  qDebug() << "Screen geometry changed";
  updateScreens();
  updateWindowState();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::onScreenDpiChanged(qreal dpi)
{
  Q_UNUSED(dpi);
  qDebug() << "Screen DPI changed";
  updateScreens();
  updateWindowState();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::updateMainSectionSettings(const QVariantMap& values)
{
  // Fullscreen
  if (values.contains("fullscreen"))
  {
    bool fs = values["fullscreen"].toBool();
    if (fs != isFullScreen())
      setFullScreen(fs);
  }

  // Always on top
  if (values.contains("alwaysOnTop"))
  {
    bool onTop = values["alwaysOnTop"].toBool();
    if (onTop != isAlwaysOnTop())
      setAlwaysOnTop(onTop);
  }

  // Web mode
  if (values.contains("webMode"))
  {
    QString mode = values["webMode"].toString();
    bool desktopMode = (mode == "desktop");
    m_window->setProperty("webDesktopMode", desktopMode);
  }

  // Forced screen
  if (values.contains("forceFSScreen"))
  {
    if (isFullScreen())
      updateForcedScreen();
  }

  // Always fullscreen
  if (values.contains("forceAlwaysFS"))
  {
    bool alwaysFS = values["forceAlwaysFS"].toBool();
    if (alwaysFS && !isFullScreen())
      setFullScreen(true);
  }

  // Startup URL
  if (values.contains("startupurl"))
  {
    QString url = values["startupurl"].toString();
    if (!url.isEmpty())
      m_window->setProperty("webUrl", url);
  }

  // Browser zoom
  if (values.contains("allowBrowserZoom"))
    enforceZoom();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::updateWindowState(bool saveGeo)
{
  updateCurrentScreen();

  if (saveGeo)
    saveGeometry();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::saveGeometrySlot()
{
  qDebug() << "saveGeometrySlot: called (app shutdown)";

  // Called on app shutdown - stop pending timer and sync immediately
  if (m_geometrySaveTimer && m_geometrySaveTimer->isActive())
  {
    qDebug() << "saveGeometrySlot: stopping pending timer";
    m_geometrySaveTimer->stop();
  }

  saveGeometry();

  qDebug() << "saveGeometrySlot: calling saveStorage()";
  // STATE section is storage, so use saveStorage()
  SettingsComponent::Get().saveStorage();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::loadGeometry()
{
  qDebug() << "loadGeometry: loading...";
  qDebug() << "loadGeometry: sizeWidthKey=" << sizeWidthKey()
           << "sizeHeightKey=" << sizeHeightKey()
           << "maximizedKey=" << maximizedKey();

  QRect rect = loadGeometryRect();
  qDebug() << "loadGeometry: loadGeometryRect returned" << rect;

  // Validate geometry fits in available screens
  if (!fitsInScreens(rect))
  {
    QScreen* primary = QGuiApplication::primaryScreen();
    if (primary)
    {
      QRect screenRect = primary->geometry();
      rect = QRect(
        screenRect.x() + (screenRect.width() - WEBUI_SIZE.width()) / 2,
        screenRect.y() + (screenRect.height() - WEBUI_SIZE.height()) / 2,
        WEBUI_SIZE.width(),
        WEBUI_SIZE.height()
      );
    }
    else
    {
      rect = QRect(0, 0, WEBUI_SIZE.width(), WEBUI_SIZE.height());
    }
  }

  // Apply minimum size
  if (rect.width() < WINDOWW_MIN_SIZE.width())
    rect.setWidth(WINDOWW_MIN_SIZE.width());
  if (rect.height() < WINDOWW_MIN_SIZE.height())
    rect.setHeight(WINDOWW_MIN_SIZE.height());

  // On Wayland, center window (position not restored)
  if (isWayland())
  {
    QScreen* primary = QGuiApplication::primaryScreen();
    if (primary)
    {
      QRect screenRect = primary->geometry();
      rect.moveTo(
        screenRect.x() + (screenRect.width() - rect.width()) / 2,
        screenRect.y() + (screenRect.height() - rect.height()) / 2
      );
    }
  }

  // Restore size first
  m_window->resize(rect.size());
  m_windowedGeometry = rect;

  // Store initial size for tracking if user changed it
  m_initialSize = rect.size();
  QScreen* primary = QGuiApplication::primaryScreen();
  if (primary)
    m_initialScreenSize = primary->geometry().size();

  // Restore to last screen (before position/maximize)
  QScreen* lastScreen = loadLastScreen();
  if (lastScreen)
    m_window->setScreen(lastScreen);

  // Restore position (only on non-Wayland)
  if (!isWayland())
    m_window->setPosition(rect.topLeft());

  // Restore maximized state after size
  bool wasMaximized = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, maximizedKey()).toBool();
  // Fallback to legacy key
  if (!wasMaximized)
    wasMaximized = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, "maximized").toBool();

  if (wasMaximized)
  {
    m_previousVisibility = QWindow::Maximized;
    m_window->setWindowState(Qt::WindowMaximized);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Config key helpers (per-screen-configuration)
QString WindowManager::configKeyPrefix() const
{
  auto screenCount = QGuiApplication::screens().size();
  if (screenCount == 1)
  {
    QScreen* primary = QGuiApplication::primaryScreen();
    if (primary)
    {
      QRect geo = primary->geometry();
      return QString("%1x%2 screen: ").arg(geo.width()).arg(geo.height());
    }
  }
  return QString("%1 screens: ").arg(screenCount);
}

QString WindowManager::sizeWidthKey() const { return configKeyPrefix() + "Width"; }
QString WindowManager::sizeHeightKey() const { return configKeyPrefix() + "Height"; }
QString WindowManager::maximizedKey() const { return configKeyPrefix() + "Window-Maximized"; }
QString WindowManager::positionXKey() const { return configKeyPrefix() + "XPosition"; }
QString WindowManager::positionYKey() const { return configKeyPrefix() + "YPosition"; }
QString WindowManager::screenNameKey() const { return "ScreenName"; }
QString WindowManager::pipWidthKey() const { return "PiP-Width"; }
QString WindowManager::pipHeightKey() const { return "PiP-Height"; }
QString WindowManager::pipXKey() const { return "PiP-XPosition"; }
QString WindowManager::pipYKey() const { return "PiP-YPosition"; }

///////////////////////////////////////////////////////////////////////////////////////////////////
QRect WindowManager::loadPipGeometry(double aspectRatio)
{
  int w = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, pipWidthKey()).toInt();
  int h = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, pipHeightKey()).toInt();
  int x = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, pipXKey()).toInt();
  int y = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, pipYKey()).toInt();

  // Use saved width but recompute height from current video aspect ratio
  if (w > 0 && aspectRatio > 0)
  {
    h = qRound(w / aspectRatio);

    QRect candidate(x, y, w, h);
    if (fitsInScreens(candidate))
      return candidate;
  }

  return QRect();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::saveWindowSize()
{
  if (!m_window)
    return;

  SettingsSection* section = SettingsComponent::Get().getSection(SETTINGS_SECTION_STATE);
  if (!section)
    return;

  if (m_pipMode)
  {
    section->setValue(pipWidthKey(), m_window->width());
    section->setValue(pipHeightKey(), m_window->height());
    return;
  }

  bool isMaximized = m_window->windowState() & Qt::WindowMaximized;
  bool isFullScreen = m_window->windowState() & Qt::WindowFullScreen;

  qDebug() << "saveWindowSize: windowState=" << m_window->windowState()
           << "isMaximized=" << isMaximized << "isFullScreen=" << isFullScreen
           << "currentSize=" << m_window->size()
           << "m_windowedGeometry=" << m_windowedGeometry;

  // Only save size if NOT maximized or fullscreen
  if (!isMaximized && !isFullScreen)
  {
    QSize size = m_window->size();
    m_windowedGeometry.setSize(size);

    // Check if size changed from initial (revert if unchanged)
    QScreen* primary = QGuiApplication::primaryScreen();
    QSize currentScreenSize = primary ? primary->geometry().size() : QSize();
    bool sizeUnchanged = (size == m_initialSize && currentScreenSize == m_initialScreenSize);

    qDebug() << "saveWindowSize: NOT maximized, saving size=" << size
             << "sizeUnchanged=" << sizeUnchanged
             << "m_initialSize=" << m_initialSize;

    if (sizeUnchanged)
    {
      // Revert to default (remove keys)
      qDebug() << "saveWindowSize: reverting to default (size unchanged)";
      section->resetValue(sizeWidthKey());
      section->resetValue(sizeHeightKey());
    }
    else
    {
      // Write to memory (disk sync happens on timer)
      qDebug() << "saveWindowSize: writing" << sizeWidthKey() << "=" << size.width()
               << sizeHeightKey() << "=" << size.height();
      section->setValue(sizeWidthKey(), size.width());
      section->setValue(sizeHeightKey(), size.height());
    }
  }
  else
  {
    qDebug() << "saveWindowSize: IS maximized/fullscreen, NOT saving size, preserving m_windowedGeometry=" << m_windowedGeometry;
  }

  // Revert maximized key when not maximized (if no default exists)
  // Don't change maximized state when in fullscreen (preserve pre-fullscreen state)
  if (!isFullScreen)
  {
    if (!isMaximized)
    {
      qDebug() << "saveWindowSize: resetting maximized key";
      section->resetValue(maximizedKey());
    }
    else
    {
      qDebug() << "saveWindowSize: setting maximized=true, key=" << maximizedKey();
      section->setValue(maximizedKey(), true);
    }
  }
  else
  {
    qDebug() << "saveWindowSize: in fullscreen, not changing maximized key";
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::saveWindowPosition()
{
  if (!m_window)
    return;

  // no-op on Wayland
  if (isWayland())
    return;

  SettingsSection* section = SettingsComponent::Get().getSection(SETTINGS_SECTION_STATE);
  if (!section)
    return;

  if (m_pipMode)
  {
    section->setValue(pipXKey(), m_window->x());
    section->setValue(pipYKey(), m_window->y());
    return;
  }

  // Skip if maximized
  if (m_window->windowState() & Qt::WindowMaximized)
    return;

  m_windowedGeometry.moveTo(m_window->position());

  // Write to memory (disk sync happens on timer)
  section->setValue(positionXKey(), m_window->x());
  section->setValue(positionYKey(), m_window->y());
  section->setValue(screenNameKey(), m_currentScreenName);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::saveGeometry()
{
  saveWindowSize();
  saveWindowPosition();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QRect WindowManager::loadGeometryRect()
{
  // Read per-screen-configuration keys
  int width = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, sizeWidthKey()).toInt();
  int height = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, sizeHeightKey()).toInt();

  qDebug() << "loadGeometryRect: read width=" << width << "height=" << height
           << "from keys" << sizeWidthKey() << sizeHeightKey();

  // Fallback to legacy geometry string if new keys don't exist
  if (width <= 0 || height <= 0)
  {
    QString geoStr = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, "geometry").toString();
    qDebug() << "loadGeometryRect: falling back to legacy geometry=" << geoStr;
    if (!geoStr.isEmpty())
    {
      QStringList parts = geoStr.split(',');
      if (parts.size() == 4)
      {
        return QRect(parts[0].toInt(), parts[1].toInt(), parts[2].toInt(), parts[3].toInt());
      }
    }
    // No saved geometry - return invalid rect to trigger centering
    qDebug() << "loadGeometryRect: no saved geometry, returning invalid rect";
    return QRect();
  }

  int x = 0, y = 0;
  if (!isWayland())
  {
    x = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, positionXKey()).toInt();
    y = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, positionYKey()).toInt();
  }

  qDebug() << "loadGeometryRect: returning" << QRect(x, y, width, height);
  return QRect(x, y, width, height);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool WindowManager::fitsInScreens(const QRect& rc)
{
  for (QScreen* screen : QGuiApplication::screens())
  {
    if (screen->geometry().intersects(rc))
      return true;
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::updateScreens()
{
  // Connect to all screen signals
  for (QScreen* screen : QGuiApplication::screens())
  {
    connect(screen, &QScreen::geometryChanged,
            this, &WindowManager::onScreenGeometryChanged, Qt::UniqueConnection);
    connect(screen, &QScreen::logicalDotsPerInchChanged,
            this, &WindowManager::onScreenDpiChanged, Qt::UniqueConnection);
  }

  // Connect to app screen add/remove
  connect(qApp, &QGuiApplication::screenAdded,
          this, &WindowManager::onScreenAdded, Qt::UniqueConnection);
  connect(qApp, &QGuiApplication::screenRemoved,
          this, &WindowManager::onScreenRemoved, Qt::UniqueConnection);

  updateCurrentScreen();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::updateCurrentScreen()
{
  QScreen* screen = findCurrentScreen();
  if (screen)
  {
    QString newName = screen->name();
    if (newName != m_currentScreenName)
    {
      qDebug() << "Current screen changed:" << m_currentScreenName << "->" << newName;
      m_currentScreenName = newName;
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QScreen* WindowManager::findCurrentScreen()
{
  if (!m_window)
    return QGuiApplication::primaryScreen();

  QRect windowRect = m_window->geometry();
  QScreen* bestScreen = nullptr;
  int maxIntersect = 0;

  for (QScreen* screen : QGuiApplication::screens())
  {
    QRect intersect = screen->geometry().intersected(windowRect);
    int area = intersect.width() * intersect.height();
    if (area > maxIntersect)
    {
      maxIntersect = area;
      bestScreen = screen;
    }
  }

  return bestScreen ? bestScreen : QGuiApplication::primaryScreen();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QScreen* WindowManager::loadLastScreen()
{
  QString screenName = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, "lastusedscreen").toString();

  if (screenName.isEmpty())
    return QGuiApplication::primaryScreen();

  for (QScreen* screen : QGuiApplication::screens())
  {
    if (screen->name() == screenName)
    {
      qDebug() << "Restored to last screen:" << screenName;
      return screen;
    }
  }

  qDebug() << "Last screen not found:" << screenName << "using primary";
  return QGuiApplication::primaryScreen();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::updateForcedScreen()
{
  QString forcedScreen = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "forceFSScreen").toString();

  if (forcedScreen.isEmpty() || !isFullScreen())
    return;

  for (QScreen* screen : QGuiApplication::screens())
  {
    if (screen->name() == forcedScreen)
    {
      qDebug() << "Forcing fullscreen to screen:" << forcedScreen;
      m_window->setScreen(screen);
      m_window->setVisibility(QWindow::FullScreen);
      return;
    }
  }

  qDebug() << "Forced screen not found:" << forcedScreen;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::connectSettings()
{
  connect(SettingsComponent::Get().getSection(SETTINGS_SECTION_MAIN),
          &SettingsSection::valuesUpdated,
          this, &WindowManager::updateMainSectionSettings);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::applySettings()
{
  // Read and apply all settings
  QVariantMap values;
  values["fullscreen"] = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "fullscreen");
  values["alwaysOnTop"] = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "alwaysOnTop");
  values["webMode"] = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "webMode");
  values["forceFSScreen"] = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "forceFSScreen");
  values["forceAlwaysFS"] = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "forceAlwaysFS");
  values["startupurl"] = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "startupurl");

  updateMainSectionSettings(values);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::onZoomFactorChanged()
{
  enforceZoom();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::enforceZoom()
{
  if (!m_webView || m_enforcingZoom)
    return;

  bool allowZoom = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "allowBrowserZoom").toBool();
  if (!allowZoom)
  {
    qreal currentZoom = m_webView->property("zoomFactor").toReal();
    if (currentZoom != 1.0)
    {
      m_enforcingZoom = true;
      m_webView->setProperty("zoomFactor", 1.0);
      m_enforcingZoom = false;
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::setPipTitleBar(bool show)
{
  if (!m_pipMode || m_pipTogglingTitleBar)
    return;

  m_pipTogglingTitleBar = true;
  QRect geo = m_window->geometry();

  if (show)
  {
    // Show title bar with only minimize and close (no maximize — confusing in PiP)
    Qt::WindowFlags flags = (m_window->flags() & ~Qt::FramelessWindowHint)
                          | Qt::CustomizeWindowHint
                          | Qt::WindowTitleHint
                          | Qt::WindowMinimizeButtonHint
                          | Qt::WindowCloseButtonHint;
    m_window->setFlags(flags);
  }
  else
  {
    m_window->setFlags(m_window->flags() | Qt::FramelessWindowHint);
  }

  m_window->setGeometry(geo);
  m_window->show();
  m_pipTogglingTitleBar = false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::enforcePipAspectRatio()
{
  if (!m_pipMode || !m_window || m_pipEnforcingAspect || m_pipAspectRatio <= 0)
    return;

  m_pipEnforcingAspect = true;
  int newHeight = qRound(m_window->width() / m_pipAspectRatio);
  if (newHeight != m_window->height())
    m_window->resize(m_window->width(), newHeight);
  m_pipEnforcingAspect = false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::setPiPMode(bool enable)
{
  if (!m_window || enable == m_pipMode)
    return;

  if (enable)
  {
    m_prePipGeometry = m_window->geometry();
    m_prePipVisibility = m_window->visibility();

    if (isFullScreen())
      setFullScreen(false);

    // Ensure cursor is visible in PiP
    setCursorVisibility(true);

    m_window->setMinimumSize(QSize(160, 90));

    m_pipAspectRatio = PlayerComponent::Get().videoAspectRatio();

    QRect pipRect = loadPipGeometry(m_pipAspectRatio);
    if (!pipRect.isValid())
    {
      int pipWidth = PIP_SIZE.width();
      int pipHeight = (m_pipAspectRatio > 0) ? qRound(pipWidth / m_pipAspectRatio) : PIP_SIZE.height();
      QSize pipSize(pipWidth, pipHeight);

      QScreen* screen = findCurrentScreen();
      if (screen)
      {
        QRect screenGeo = screen->availableGeometry();
        int x = screenGeo.right() - pipSize.width() - 20;
        int y = screenGeo.bottom() - pipSize.height() - 20;
        pipRect = QRect(QPoint(x, y), pipSize);
      }
      else
      {
        pipRect = QRect(m_window->position(), pipSize);
      }
    }
    m_window->setGeometry(pipRect);

    m_prePipFlags = m_window->flags();
    m_window->setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);

    // Enforce aspect ratio on resize
    connect(m_window, &QQuickWindow::widthChanged, this, &WindowManager::enforcePipAspectRatio);

    m_pipMode = true;
    emit pipModeChanged(true);
  }
  else // Exiting PiP mode
  {
    disconnect(m_window, &QQuickWindow::widthChanged, this, &WindowManager::enforcePipAspectRatio);

    m_window->setFlags(m_prePipFlags);

    m_window->setMinimumSize(WINDOWW_MIN_SIZE);

    m_window->setGeometry(m_prePipGeometry);

    if (m_prePipVisibility == QWindow::Maximized)
      m_window->showMaximized();
    else if (m_prePipVisibility == QWindow::FullScreen)
      m_window->showFullScreen();

    m_pipMode = false;
    emit pipModeChanged(false);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::togglePiP()
{
  // Only allow entering PiP when video is playing
  if (!m_pipMode && PlayerComponent::Get().videoAspectRatio() <= 0)
    return;

  setPiPMode(!m_pipMode);
}
