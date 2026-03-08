#ifndef WINDOWMANAGER_H
#define WINDOWMANAGER_H

#include <QCoreApplication>
#include <QMouseEvent>
#include <QObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRect>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <memory>
#include "core/ComponentManager.h"

#define WEBUI_MAX_HEIGHT 1440.0
#define WEBUI_SIZE QSize(1280, 720)
#define WINDOWW_MIN_SIZE QSize(213, 120)
#define PIP_SIZE QSize(400, 225)

class WindowManager : public ComponentBase
{
  Q_OBJECT
  Q_PROPERTY(bool pipMode READ isPiPMode NOTIFY pipModeChanged)

public:
  static WindowManager& Get();

  explicit WindowManager(QObject* parent = nullptr);
  ~WindowManager() override;

  const char* componentName() override { return "window"; }
  bool componentExport() override { return true; }
  bool componentInitialize() override { return true; }
  void componentPostInitialize() override;

  // Called from main.cpp after window created
  void initializeWindow(QQuickWindow* window);

  // Always on top
  Q_INVOKABLE void setAlwaysOnTop(bool enable);
  Q_INVOKABLE bool isAlwaysOnTop() const;
  Q_INVOKABLE void toggleAlwaysOnTop();

  // Platform detection
  Q_INVOKABLE bool isWayland() const;

  // Fullscreen
  Q_INVOKABLE void setFullScreen(bool enable);
  Q_INVOKABLE bool isFullScreen() const;

  // Picture-in-Picture
  Q_INVOKABLE void setPiPMode(bool enable);
  Q_INVOKABLE void togglePiP();
  bool isPiPMode() const { return m_pip.active; }

  // Cursor visibility
  Q_INVOKABLE void setCursorVisibility(bool visible);

  // Event filter for cursor enter/leave tracking
  bool eventFilter(QObject* watched, QEvent* event) override;

  // Window activation
  Q_INVOKABLE void raiseWindow();

public slots:
  void toggleFullscreen();

signals:
  void fullScreenSwitched();
  void pipModeChanged(bool enabled);

private slots:
  void onVisibilityChanged(QWindow::Visibility visibility);
  void onScreenAdded(QScreen* screen);
  void onScreenRemoved(QScreen* screen);
  void onScreenGeometryChanged(const QRect& geometry);
  void onScreenDpiChanged(qreal dpi);
  void updateMainSectionSettings(const QVariantMap& values);
  void updateWindowState(bool saveGeo = true);
  void saveGeometrySlot();
  void onZoomFactorChanged();

private:
  // Geometry (separate size/position)
  void loadGeometry();
  QRect loadGeometryRect();
  void saveWindowSize();
  void saveWindowPosition();
  void saveGeometry();  // Calls both size and position
  bool fitsInScreens(const QRect& rc);

  // Config key helpers (per-screen-configuration)
  QString configKeyPrefix() const;
  QString sizeWidthKey() const;
  QString sizeHeightKey() const;
  QString maximizedKey() const;
  QString positionXKey() const;
  QString positionYKey() const;
  QString screenNameKey() const;
  QString pipWidthKey() const;
  QString pipXKey() const;
  QString pipYKey() const;
  QRect loadPipGeometry(double aspectRatio);

  // Screens
  void updateScreens();
  void updateCurrentScreen();
  QScreen* findCurrentScreen();
  QScreen* loadLastScreen();
  void updateForcedScreen();

  // Settings
  void connectSettings();
  void applySettings();

  void enforceZoom();
  void enforcePipAspectRatio();
  Qt::Edges pipEdgesAt(const QPoint& localPos) const;
  void enterPiP();
  void exitPiP();

  QQuickWindow* m_window;
  QQuickItem* m_webView;
  bool m_enforcingZoom;
  QString m_currentScreenName;
  int m_ignoreFullscreenSettingsChange;
  bool m_cursorVisible;
  bool m_cursorInsideWindow;

  // Window state
  QWindow::Visibility m_previousVisibility;  // State before fullscreen
  QRect m_windowedGeometry;                  // Geometry when in Windowed state
  QTimer* m_geometrySaveTimer;               // Debounced disk sync

  // PiP state
  struct PipState
  {
    bool active = false;
    bool dragging = false;
    bool forwardingClick = false;
    bool enforcingAspect = false;
    bool resizeCursorSet = false;
    double aspectRatio = 0;
    QPoint dragStartCursorPos;
    std::unique_ptr<QMouseEvent> pressEvent;

    // Pre-PiP window state for restore
    QRect prePipGeometry;
    Qt::WindowFlags prePipFlags;
    QWindow::Visibility prePipVisibility = QWindow::Windowed;

    void reset()
    {
      active = false;
      dragging = false;
      forwardingClick = false;
      enforcingAspect = false;
      resizeCursorSet = false;
      aspectRatio = 0;
      dragStartCursorPos = QPoint();
      pressEvent.reset();
    }
  };
  PipState m_pip;

  // initial size tracking to detect if size changed from default
  QSize m_initialSize;
  QSize m_initialScreenSize;
};

#endif // WINDOWMANAGER_H
