#ifdef USE_WAYLAND_SUBSURFACE

#include "WaylandMpvPlayer.h"
#include "WaylandVulkanContext.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <qpa/qplatformnativeinterface.h>

#include <wayland-client.h>
#include <mpv/client.h>
#include <mpv/render_vk.h>

#include "QtHelper.h"

#include <QDebug>

#include <clocale>
#include <chrono>

static const struct wl_registry_listener s_registryListener = {
    .global = WaylandMpvPlayer::registryGlobal,
    .global_remove = WaylandMpvPlayer::registryGlobalRemove,
};

WaylandMpvPlayer::WaylandMpvPlayer(QObject *parent)
    : QObject(parent)
    , m_vulkan(new WaylandVulkanContext())
{
}

WaylandMpvPlayer::~WaylandMpvPlayer()
{
    detach();
    delete m_vulkan;
}

void WaylandMpvPlayer::registryGlobal(void *data, struct wl_registry *registry,
                                       uint32_t name, const char *interface, uint32_t)
{
    auto *self = static_cast<WaylandMpvPlayer*>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        self->m_wlCompositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        self->m_wlSubcompositor = static_cast<wl_subcompositor*>(
            wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    }
}

void WaylandMpvPlayer::registryGlobalRemove(void *, struct wl_registry *, uint32_t)
{
}

bool WaylandMpvPlayer::initializeWayland()
{
    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    m_wlDisplay = static_cast<wl_display*>(native->nativeResourceForIntegration("wl_display"));
    if (!m_wlDisplay) {
        qCritical() << "Failed to get Wayland display from Qt";
        return false;
    }

    struct wl_registry *registry = wl_display_get_registry(m_wlDisplay);
    wl_registry_add_listener(registry, &s_registryListener, this);
    wl_display_roundtrip(m_wlDisplay);

    if (!m_wlCompositor || !m_wlSubcompositor) {
        qCritical() << "Missing Wayland globals";
        return false;
    }

    return true;
}

bool WaylandMpvPlayer::createSubsurface(QQuickWindow *window)
{
    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    wl_surface *qtSurface = static_cast<wl_surface*>(
        native->nativeResourceForWindow("surface", window));
    if (!qtSurface) {
        qCritical() << "Failed to get Qt's wl_surface";
        return false;
    }

    m_mpvSurface = wl_compositor_create_surface(m_wlCompositor);
    m_mpvSubsurface = wl_subcompositor_get_subsurface(m_wlSubcompositor, m_mpvSurface, qtSurface);

    wl_subsurface_set_position(m_mpvSubsurface, 0, 0);
    wl_subsurface_place_below(m_mpvSubsurface, qtSurface);
    wl_subsurface_set_desync(m_mpvSubsurface);

    wl_surface_commit(m_mpvSurface);
    wl_display_roundtrip(m_wlDisplay);

    qInfo() << "Created mpv subsurface below Qt";
    return true;
}

bool WaylandMpvPlayer::initializeMpv()
{
    setlocale(LC_NUMERIC, "C");

    m_mpv = mpv_create();
    if (!m_mpv) {
        qCritical() << "Failed to create mpv";
        return false;
    }

    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "terminal", "no");

    // Match MpvQt settings
    mpv_set_option_string(m_mpv, "force-window", "yes");  // Keep window open between files
    mpv_set_option_string(m_mpv, "osd-level", "0");
    mpv_set_option_string(m_mpv, "demuxer-mkv-probe-start-time", "no");
    mpv_set_option_string(m_mpv, "demuxer-lavf-probe-info", "yes");
    mpv_set_option_string(m_mpv, "audio-fallback-to-null", "yes");
    mpv_set_option_string(m_mpv, "ad-lavc-downmix", "no");
    mpv_set_option_string(m_mpv, "cache-seek-min", "5000");
    mpv_set_option_string(m_mpv, "ytdl", "no");

    // HDR settings
    if (m_vulkan->isHdr()) {
        mpv_set_option_string(m_mpv, "target-trc", "pq");
        mpv_set_option_string(m_mpv, "target-prim", "bt.2020");
        mpv_set_option_string(m_mpv, "target-peak", "1000");
    }

    if (mpv_initialize(m_mpv) < 0) {
        qCritical() << "Failed to initialize mpv";
        return false;
    }

    // Request log messages through event loop
    mpv_request_log_messages(m_mpv, "terminal-default");

    // Observe properties for signals (do this once, not on every file load)
    mpv_observe_property(m_mpv, 0, "playback-time", MPV_FORMAT_DOUBLE);  // not time-pos
    mpv_observe_property(m_mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "core-idle", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "cache-buffering-state", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "vo-configured", MPV_FORMAT_FLAG);

    // Create render context
    mpv_vulkan_init_params vkParams{};
    vkParams.instance = m_vulkan->instance();
    vkParams.physical_device = m_vulkan->physicalDevice();
    vkParams.device = m_vulkan->device();
    vkParams.graphics_queue = m_vulkan->queue();
    vkParams.graphics_queue_family = m_vulkan->queueFamily();
    vkParams.get_instance_proc_addr = vkGetInstanceProcAddr;
    vkParams.features = m_vulkan->features();
    vkParams.extensions = m_vulkan->deviceExtensions();
    vkParams.num_extensions = m_vulkan->deviceExtensionCount();

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_VULKAN)},
        {MPV_RENDER_PARAM_BACKEND, const_cast<char*>("gpu-next")},
        {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vkParams},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    int result = mpv_render_context_create(&m_renderCtx, m_mpv, params);
    if (result < 0) {
        qCritical() << "Failed to create mpv render context:" << mpv_error_string(result);
        return false;
    }

    qInfo() << "mpv render context created";
    return true;
}

bool WaylandMpvPlayer::attachToWindow(QQuickWindow *window)
{
    if (m_window)
        detach();

    m_window = window;

    if (!initializeWayland())
        return false;

    QGuiApplication::processEvents();

    if (!createSubsurface(window))
        return false;

    int width = window->width();
    int height = window->height();

    if (!m_vulkan->initialize(m_wlDisplay, m_mpvSurface))
        return false;

    if (!m_vulkan->createSwapchain(width, height))
        return false;

    if (!initializeMpv())
        return false;

    // Connect resize signals
    connect(window, &QWindow::widthChanged, this, &WaylandMpvPlayer::onWindowWidthChanged);
    connect(window, &QWindow::heightChanged, this, &WaylandMpvPlayer::onWindowHeightChanged);

    // Start render thread
    m_running = true;
    m_renderThread = std::thread(&WaylandMpvPlayer::renderLoop, this);

    return true;
}

void WaylandMpvPlayer::detach()
{
    m_running = false;
    if (m_renderThread.joinable())
        m_renderThread.join();

    if (m_renderCtx) {
        mpv_render_context_free(m_renderCtx);
        m_renderCtx = nullptr;
    }

    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }

    // Only cleanup Vulkan/Wayland resources if the app is still running
    // During shutdown, Qt may have already destroyed the Wayland connection
    bool appClosing = !QGuiApplication::instance() || QGuiApplication::closingDown();

    if (!appClosing && m_vulkan) {
        m_vulkan->cleanup();
    }

    if (!appClosing) {
        if (m_mpvSubsurface) {
            wl_subsurface_destroy(m_mpvSubsurface);
            m_mpvSubsurface = nullptr;
        }

        if (m_mpvSurface) {
            wl_surface_destroy(m_mpvSurface);
            m_mpvSurface = nullptr;
        }
    }

    if (m_window) {
        disconnect(m_window, nullptr, this, nullptr);
        m_window = nullptr;
    }
}

void WaylandMpvPlayer::onWindowWidthChanged(int)
{
    if (m_window) {
        m_pendingWidth = m_window->width();
        m_pendingHeight = m_window->height();
        m_needsResize = true;
    }
}

void WaylandMpvPlayer::onWindowHeightChanged(int)
{
    if (m_window) {
        m_pendingWidth = m_window->width();
        m_pendingHeight = m_window->height();
        m_needsResize = true;
    }
}

void WaylandMpvPlayer::renderLoop()
{
    while (m_running) {
        // Handle resize
        if (m_needsResize) {
            int w = m_pendingWidth;
            int h = m_pendingHeight;
            if (w > 0 && h > 0) {
                m_vulkan->recreateSwapchain(w, h);
            }
            m_needsResize = false;
        }

        // Process mpv events
        processEvents();

        if (!m_running)
            break;

        // Acquire swapchain image
        VkFence fence;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(m_vulkan->device(), &fenceInfo, nullptr, &fence);

        uint32_t imageIdx;
        VkResult result = vkAcquireNextImageKHR(m_vulkan->device(), m_vulkan->swapchain(),
                                                 1000000000, VK_NULL_HANDLE, fence, &imageIdx);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            vkDestroyFence(m_vulkan->device(), fence, nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        vkWaitForFences(m_vulkan->device(), 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(m_vulkan->device(), fence, nullptr);

        // Render with mpv
        mpv_vulkan_fbo fbo{};
        fbo.image = m_vulkan->swapchainImages()[imageIdx];
        fbo.image_view = m_vulkan->swapchainViews()[imageIdx];
        fbo.width = m_vulkan->width();
        fbo.height = m_vulkan->height();
        fbo.format = m_vulkan->swapchainFormat();
        fbo.current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        fbo.target_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        int flipY = 0;
        mpv_render_param renderParams[] = {
            {MPV_RENDER_PARAM_VULKAN_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };

        mpv_render_context_render(m_renderCtx, renderParams);

        // Present
        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        VkSwapchainKHR swapchain = m_vulkan->swapchain();
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &imageIdx;
        vkQueuePresentKHR(m_vulkan->queue(), &presentInfo);
    }
}

void WaylandMpvPlayer::processEvents()
{
    while (m_mpv) {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;

        switch (event->event_id) {
        case MPV_EVENT_SHUTDOWN:
            m_running = false;
            break;
        case MPV_EVENT_START_FILE:
            QMetaObject::invokeMethod(this, [this]() {
                emit fileStarted();
            }, Qt::QueuedConnection);
            break;
        case MPV_EVENT_END_FILE:
            // Don't set m_running = false here - that stops the render loop
            // and prevents playing the next file
            QMetaObject::invokeMethod(this, [this]() {
                emit endOfFile();
            }, Qt::QueuedConnection);
            break;
        case MPV_EVENT_LOG_MESSAGE: {
            mpv_event_log_message *msg = static_cast<mpv_event_log_message*>(event->data);
            QString text = QString("[%1] %2").arg(msg->prefix, QString(msg->text).trimmed());
            if (strcmp(msg->level, "error") == 0 || strcmp(msg->level, "fatal") == 0)
                qCritical().noquote() << text;
            else if (strcmp(msg->level, "warn") == 0)
                qWarning().noquote() << text;
            else
                qInfo().noquote() << text;
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE: {
            mpv_event_property *prop = static_cast<mpv_event_property*>(event->data);
            if (strcmp(prop->name, "playback-time") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                qint64 pos = static_cast<qint64>(*static_cast<double*>(prop->data) * 1000);
                QMetaObject::invokeMethod(this, [this, pos]() {
                    emit positionChanged(pos);
                }, Qt::QueuedConnection);
            } else if (strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                qint64 dur = static_cast<qint64>(*static_cast<double*>(prop->data) * 1000);
                QMetaObject::invokeMethod(this, [this, dur]() {
                    emit durationChanged(dur);
                }, Qt::QueuedConnection);
            } else if (strcmp(prop->name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG) {
                bool paused = *static_cast<int*>(prop->data);
                QMetaObject::invokeMethod(this, [this, paused]() {
                    emit playbackStateChanged(!paused);
                }, Qt::QueuedConnection);
            } else if (strcmp(prop->name, "core-idle") == 0 && prop->format == MPV_FORMAT_FLAG) {
                bool idle = *static_cast<int*>(prop->data);
                QMetaObject::invokeMethod(this, [this, idle]() {
                    emit coreIdleChanged(idle);
                }, Qt::QueuedConnection);
            } else if (strcmp(prop->name, "cache-buffering-state") == 0 && prop->format == MPV_FORMAT_INT64) {
                int percent = static_cast<int>(*static_cast<int64_t*>(prop->data));
                QMetaObject::invokeMethod(this, [this, percent]() {
                    emit bufferingChanged(percent);
                }, Qt::QueuedConnection);
            } else if (strcmp(prop->name, "vo-configured") == 0 && prop->format == MPV_FORMAT_FLAG) {
                bool visible = *static_cast<int*>(prop->data);
                QMetaObject::invokeMethod(this, [this, visible]() {
                    emit windowVisibleChanged(visible);
                }, Qt::QueuedConnection);
            }
            break;
        }
        default:
            break;
        }
    }
}

void WaylandMpvPlayer::loadFile(const QString &path)
{
    if (!m_mpv)
        return;
    const char *cmd[] = {"loadfile", path.toUtf8().constData(), nullptr};
    mpv_command(m_mpv, cmd);
}

void WaylandMpvPlayer::command(const QVariant &args)
{
    if (!m_mpv)
        return;

    // Use mpv::qt::command which properly handles QVariantMap options
    mpv::qt::command(m_mpv, args);
}

void WaylandMpvPlayer::setProperty(const QString &name, const QVariant &value)
{
    if (!m_mpv)
        return;

    QByteArray nameUtf8 = name.toUtf8();

    if (value.typeId() == QMetaType::Bool) {
        int flag = value.toBool() ? 1 : 0;
        mpv_set_property(m_mpv, nameUtf8.constData(), MPV_FORMAT_FLAG, &flag);
    } else if (value.typeId() == QMetaType::Int || value.typeId() == QMetaType::LongLong) {
        int64_t val = value.toLongLong();
        mpv_set_property(m_mpv, nameUtf8.constData(), MPV_FORMAT_INT64, &val);
    } else if (value.typeId() == QMetaType::Double) {
        double val = value.toDouble();
        mpv_set_property(m_mpv, nameUtf8.constData(), MPV_FORMAT_DOUBLE, &val);
    } else {
        QByteArray valUtf8 = value.toString().toUtf8();
        mpv_set_property_string(m_mpv, nameUtf8.constData(), valUtf8.constData());
    }
}

QVariant WaylandMpvPlayer::getProperty(const QString &name) const
{
    if (!m_mpv)
        return QVariant();

    QByteArray nameUtf8 = name.toUtf8();
    char *result = mpv_get_property_string(m_mpv, nameUtf8.constData());
    if (result) {
        QString str = QString::fromUtf8(result);
        mpv_free(result);
        return str;
    }
    return QVariant();
}

bool WaylandMpvPlayer::isPlaying() const
{
    if (!m_mpv)
        return false;
    int paused = 0;
    mpv_get_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused);
    return !paused;
}

qint64 WaylandMpvPlayer::position() const
{
    if (!m_mpv)
        return 0;
    double pos = 0;
    mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    return static_cast<qint64>(pos * 1000);
}

qint64 WaylandMpvPlayer::duration() const
{
    if (!m_mpv)
        return 0;
    double dur = 0;
    mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
    return static_cast<qint64>(dur * 1000);
}

#endif // USE_WAYLAND_SUBSURFACE
