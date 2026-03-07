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
    m_pip(),
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
            if (m_pip.active && !isNavigating)
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
    if (m_pip.active)
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
      // Explicitly restore position — Qt's internal restore geometry may be
      // stale after a PiP→FS transition (window was temporarily off-screen).
      // The interaction between PIP and FS is complicated. It is probably cleaner to disallow PIP <-> FS transitions.
      // However, this requires WebView changes, and carefully enabling/disabling shortcut keys (including video area double click)
      // The current implementation handles all cases well enough
      if (!isWayland() && m_windowedGeometry.isValid())
        m_window->setPosition(m_windowedGeometry.topLeft());
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
  if (m_pip.active)
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
static Qt::CursorShape cursorForEdges(Qt::Edges edges)
{
  bool left = edges & Qt::LeftEdge;
  bool right = edges & Qt::RightEdge;
  bool top = edges & Qt::TopEdge;
  bool bottom = edges & Qt::BottomEdge;

  if ((left && top) || (right && bottom))
    return Qt::SizeFDiagCursor;
  if ((left && bottom) || (right && top))
    return Qt::SizeBDiagCursor;
  if (left || right)
    return Qt::SizeHorCursor;
  return Qt::SizeVerCursor;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
Qt::Edges WindowManager::pipEdgesAt(const QPoint& localPos) const
{
  if (!m_window)
    return {};

  const int grip = 8;
  Qt::Edges edges;

  if (localPos.x() < grip)
    edges |= Qt::LeftEdge;
  if (localPos.x() >= m_window->width() - grip)
    edges |= Qt::RightEdge;
  if (localPos.y() < grip)
    edges |= Qt::TopEdge;
  if (localPos.y() >= m_window->height() - grip)
    edges |= Qt::BottomEdge;

  return edges;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool WindowManager::eventFilter(QObject* watched, QEvent* event)
{
  if (watched == m_window)
  {
    // PiP edge resize: frameless windows lose native resize borders on Windows
    // and some Linux WMs, so we manually detect edges, show resize cursors on
    // hover, and call startSystemResize() on press.  macOS handles this natively.
#ifndef Q_OS_MAC
    if (m_pip.active && event->type() == QEvent::MouseMove && !m_pip.pressEvent && !m_pip.dragging)
    {
      auto* me = static_cast<QMouseEvent*>(event);
      Qt::Edges edges = pipEdgesAt(me->position().toPoint());
      if (edges && !m_pip.resizeCursorSet)
      {
        qApp->setOverrideCursor(QCursor(cursorForEdges(edges)));
        m_pip.resizeCursorSet = true;
      }
      else if (edges && m_pip.resizeCursorSet)
      {
        qApp->changeOverrideCursor(QCursor(cursorForEdges(edges)));
      }
      else if (!edges && m_pip.resizeCursorSet)
      {
        qApp->restoreOverrideCursor();
        m_pip.resizeCursorSet = false;
      }
    }
#endif

    // PiP drag-to-move: click anywhere and drag to reposition the window.
    // Press is consumed to prevent the web UI from toggling play/pause.
    // On move past threshold, startSystemMove() hands off to the OS.
    // On release without movement, we forward the original click.
    if (m_pip.active && !m_pip.forwardingClick && event->type() == QEvent::MouseButtonPress)
    {
      auto* me = static_cast<QMouseEvent*>(event);
      if (me->button() == Qt::LeftButton)
      {
        // Edge press: start system resize instead of drag
#ifndef Q_OS_MAC
        Qt::Edges edges = pipEdgesAt(me->position().toPoint());
        if (edges)
        {
          if (m_pip.resizeCursorSet)
          {
            qApp->restoreOverrideCursor();
            m_pip.resizeCursorSet = false;
          }
          m_window->startSystemResize(edges);
          return true;
        }
#endif

        m_pip.dragging = false;
        m_pip.dragStartCursorPos = QCursor::pos();
        m_pip.pressEvent.reset(me->clone());
        return true;
      }
    }
    else if (m_pip.active && m_pip.pressEvent && event->type() == QEvent::MouseMove)
    {
      QPoint delta = QCursor::pos() - m_pip.dragStartCursorPos;
      if (!m_pip.dragging && delta.manhattanLength() > 3)
      {
        m_pip.dragging = true;
        m_pip.pressEvent.reset();
        m_window->startSystemMove();
      }
      return true;
    }
    else if (m_pip.active && !m_pip.forwardingClick && event->type() == QEvent::MouseButtonRelease)
    {
      auto* me = static_cast<QMouseEvent*>(event);
      if (me->button() == Qt::LeftButton)
      {
        bool wasDragging = m_pip.dragging;
        m_pip.dragging = false;
        m_pip.dragStartCursorPos = QPoint();
        if (wasDragging)
          return true;

        // Not a drag — forward the original press + this release
        if (m_pip.pressEvent)
        {
          m_pip.forwardingClick = true;
          QCoreApplication::sendEvent(m_window, m_pip.pressEvent.get());
          QCoreApplication::sendEvent(m_window, me);
          m_pip.forwardingClick = false;
          m_pip.pressEvent.reset();
          return true;
        }
      }
    }

    if (event->type() == QEvent::Enter)
    {
      m_cursorInsideWindow = true;
#ifdef Q_OS_MAC
      // Re-hide cursor if it should be hidden
      if (!m_cursorVisible)
        OSXUtils::SetCursorVisible(false);
#endif
    }
    else if (event->type() == QEvent::Leave)
    {
      m_cursorInsideWindow = false;
#ifndef Q_OS_MAC
      if (m_pip.resizeCursorSet)
      {
        qApp->restoreOverrideCursor();
        m_pip.resizeCursorSet = false;
      }
#endif
#ifdef Q_OS_MAC
      // Always show cursor when leaving window
      OSXUtils::SetCursorVisible(true);
#endif
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
  // Skip during PiP mode to avoid corrupting m_previousVisibility with transitional states
  if (!m_pip.active && visibility != QWindow::FullScreen && visibility != QWindow::Hidden)
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
QString WindowManager::pipWidthKey() const { return configKeyPrefix() + "PiP-Width"; }
QString WindowManager::pipXKey() const { return configKeyPrefix() + "PiP-XPosition"; }
QString WindowManager::pipYKey() const { return configKeyPrefix() + "PiP-YPosition"; }

///////////////////////////////////////////////////////////////////////////////////////////////////
QRect WindowManager::loadPipGeometry(double aspectRatio)
{
  int w = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, pipWidthKey()).toInt();
  int x = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, pipXKey()).toInt();
  int y = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, pipYKey()).toInt();

  // Use saved width but recompute height from current video aspect ratio
  if (w > 0 && aspectRatio > 0)
  {
    int h = qRound(w / aspectRatio);

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

  if (m_pip.active)
  {
    section->setValue(pipWidthKey(), m_window->width());
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

  if (m_pip.active)
  {
    section->setValue(pipXKey(), m_window->x());
    section->setValue(pipYKey(), m_window->y());
    return;
  }

  // Skip if maximized or fullscreen
  if (m_window->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))
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
void WindowManager::enforcePipAspectRatio()
{
  if (!m_pip.active || !m_window || m_pip.enforcingAspect || m_pip.aspectRatio <= 0)
    return;

  m_pip.enforcingAspect = true;
  int newHeight = qRound(m_window->width() / m_pip.aspectRatio);
  if (newHeight != m_window->height())
    m_window->resize(m_window->width(), newHeight);
  m_pip.enforcingAspect = false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::setPiPMode(bool enable)
{
  if (!m_window || enable == m_pip.active)
    return;

  if (enable)
    enterPiP();
  else
    exitPiP();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::togglePiP()
{
  setPiPMode(!m_pip.active);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::enterPiP()
{
  if (SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "forceAlwaysFS").toBool() || isWayland())
    return;

  m_pip.aspectRatio = PlayerComponent::Get().videoAspectRatio();
  if (m_pip.aspectRatio <= 0) // No video in current view
    return;

  m_pip.prePipGeometry = m_windowedGeometry;
  m_pip.prePipVisibility = m_window->visibility();
  m_pip.prePipFlags = m_window->flags();

  // Must be set before setFullScreen(false) so onVisibilityChanged
  // skips updating m_previousVisibility during the transition.
  m_pip.active = true;

  if (isFullScreen())
    setFullScreen(false);

  setCursorVisibility(true);
  m_window->setMinimumSize(QSize(160, 90));

  QRect pipRect = loadPipGeometry(m_pip.aspectRatio);
  if (!pipRect.isValid())
  {
    int pipWidth = PIP_SIZE.width();
    int pipHeight = qRound(pipWidth / m_pip.aspectRatio);
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

  m_window->setFlags(Qt::FramelessWindowHint);
  setAlwaysOnTop(true);

  connect(m_window, &QQuickWindow::widthChanged, this, &WindowManager::enforcePipAspectRatio);
  emit pipModeChanged(true);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WindowManager::exitPiP()
{
  disconnect(m_window, &QQuickWindow::widthChanged, this, &WindowManager::enforcePipAspectRatio);

#ifndef Q_OS_MAC
  if (m_pip.resizeCursorSet)
  {
    qApp->restoreOverrideCursor();
    m_pip.resizeCursorSet = false;
  }
#endif

  // Save restore values before reset
  auto restoreFlags = m_pip.prePipFlags;
  auto restoreGeometry = m_pip.prePipGeometry;
  auto restoreVisibility = m_pip.prePipVisibility;

  m_pip.reset();
  emit pipModeChanged(false);

  // setFlags() recreates the native window on macOS, which breaks Chromium's
  // mouse tracking. To force real native Enter/Leave events, we place the
  // window off-screen after setFlags(), then defer the real geometry restore
  // to the next event loop tick. This gives macOS time to process the
  // off-screen position, so that when the window moves under the cursor,
  // a real native Enter event is generated.
  m_window->setFlags(restoreFlags);
  m_window->setMinimumSize(WINDOWW_MIN_SIZE);
  m_window->setGeometry(QRect(-10000, -10000, restoreGeometry.width(), restoreGeometry.height()));

  QTimer::singleShot(0, this, [this, restoreGeometry, restoreVisibility]() {
    // If the window is already fullscreen (e.g. PIP → FS transition called
    // showFullScreen() before this timer fired), skip the deferred restore
    // so we don't override the fullscreen geometry with the windowed one.
    if (isFullScreen())
    {
      m_windowedGeometry = restoreGeometry;
      return;
    }

    m_window->setGeometry(restoreGeometry);

    if (restoreVisibility == QWindow::Maximized)
      m_window->showMaximized();
    else if (restoreVisibility == QWindow::FullScreen)
      m_window->showFullScreen();

    m_cursorInsideWindow = m_window->geometry().contains(QCursor::pos());
  });
}
