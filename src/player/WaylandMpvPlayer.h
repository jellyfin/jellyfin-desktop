#ifndef WAYLANDMPVPLAYER_H
#define WAYLANDMPVPLAYER_H

#ifdef USE_WAYLAND_SUBSURFACE

#include <QObject>
#include <QVariant>
#include <thread>
#include <atomic>

class QQuickWindow;
class WaylandVulkanContext;

struct wl_display;
struct wl_compositor;
struct wl_subcompositor;
struct wl_surface;
struct wl_subsurface;

struct mpv_handle;
struct mpv_render_context;

class WaylandMpvPlayer : public QObject
{
    Q_OBJECT

public:
    explicit WaylandMpvPlayer(QObject *parent = nullptr);
    ~WaylandMpvPlayer();

    bool attachToWindow(QQuickWindow *window);
    void detach();

    void loadFile(const QString &path);
    void command(const QVariant &args);
    void setProperty(const QString &name, const QVariant &value);
    QVariant getProperty(const QString &name) const;

    bool isPlaying() const;
    qint64 position() const;
    qint64 duration() const;

signals:
    void fileStarted();
    void playbackStateChanged(bool playing);
    void positionChanged(qint64 pos);
    void durationChanged(qint64 duration);
    void coreIdleChanged(bool idle);
    void bufferingChanged(int percent);
    void windowVisibleChanged(bool visible);
    void endOfFile();
    void error(const QString &message);

public:
    // Wayland registry callbacks (must be public for struct initialization)
    static void registryGlobal(void *data, struct wl_registry *registry,
                               uint32_t name, const char *interface, uint32_t version);
    static void registryGlobalRemove(void *data, struct wl_registry *registry, uint32_t name);

private slots:
    void onWindowWidthChanged(int width);
    void onWindowHeightChanged(int height);

private:
    bool initializeWayland();
    bool createSubsurface(QQuickWindow *window);
    bool initializeMpv();
    void renderLoop();
    void processEvents();

    QQuickWindow *m_window = nullptr;
    WaylandVulkanContext *m_vulkan = nullptr;

    // Wayland
    wl_display *m_wlDisplay = nullptr;
    wl_compositor *m_wlCompositor = nullptr;
    wl_subcompositor *m_wlSubcompositor = nullptr;
    wl_surface *m_mpvSurface = nullptr;
    wl_subsurface *m_mpvSubsurface = nullptr;

    // mpv
    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_renderCtx = nullptr;

    // Threading
    std::thread m_renderThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_needsResize{false};
    std::atomic<int> m_pendingWidth{0};
    std::atomic<int> m_pendingHeight{0};
};

#endif // USE_WAYLAND_SUBSURFACE
#endif // WAYLANDMPVPLAYER_H
