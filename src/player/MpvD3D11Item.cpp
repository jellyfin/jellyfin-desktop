#include "MpvD3D11Item.h"

#include <MpvController>
#include <QDebug>
#include <QScopeGuard>
#include <QThread>
#include <d3d11.h>
#include <mpv/render_d3d11.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <mutex>

class MpvD3D11Renderer;

struct MpvD3D11State
{
    enum Request : uint64_t { DetachVideo = 1, AttachVideo };

    explicit MpvD3D11State(MpvD3D11Item* videoItem) : item(videoItem)
    {
        thread = new QThread;
        controller = new MpvController;
        controller->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, controller, &QObject::deleteLater);
        thread->start();
        QMetaObject::invokeMethod(controller, &MpvController::init, Qt::BlockingQueuedConnection);
        handle = controller->mpv();
        // Recovery must not consume events from the player's client queue.
        controlClient = mpv_create_client(handle, "qt-d3d11");
        if (!controlClient)
            qFatal("Failed to create the mpv Direct3D 11 control client");
        // Only recovery replies are needed, including while the window is hidden.
        for (int id = MPV_EVENT_LOG_MESSAGE; id <= MPV_EVENT_HOOK; ++id)
        {
            const auto event = static_cast<mpv_event_id>(id);
            if (event != MPV_EVENT_SET_PROPERTY_REPLY && mpv_event_name(event))
            {
                const int result = mpv_request_event(controlClient, event, 0);
                if (result < 0)
                    qFatal("Failed to configure the mpv recovery client: %s", mpv_error_string(result));
            }
        }
        mpv_set_wakeup_callback(controlClient, &MpvD3D11State::requestUpdate, this);
    }

    ~MpvD3D11State()
    {
        mpv_set_wakeup_callback(handle, nullptr, nullptr);
        mpv_set_wakeup_callback(controlClient, nullptr, nullptr);
        thread->quit();
        thread->wait();
        thread->deleteLater();
        mpv_destroy(controlClient);
        mpv_terminate_destroy(handle);
    }

    void processControlEvents()
    {
        for (;;)
        {
            const auto* event = mpv_wait_event(controlClient, 0);
            if (event->event_id == MPV_EVENT_NONE)
                break;
            if (event->event_id == MPV_EVENT_SET_PROPERTY_REPLY)
            {
                if (event->error < 0)
                    qFatal("Failed to switch mpv video output during renderer recovery: %s",
                           mpv_error_string(event->error));
                if (event->reply_userdata == DetachVideo)
                    videoOutputDetached = true;
            }
        }
    }

    void setVideoOutput(const char* output, Request request)
    {
        const int result = mpv_set_property_async(controlClient, request, "vo", MPV_FORMAT_STRING, &output);
        if (result < 0)
            qFatal("Failed to queue mpv video output recovery: %s", mpv_error_string(result));
    }

    void retireContext(mpv_render_context* context)
    {
        processControlEvents();
        videoOutputDetached = false;
        // Freeing an active context disables its track (and ends video-only files).
        setVideoOutput("null", DetachVideo);
        while (!videoOutputDetached)
        {
            // Both APIs are render-thread safe; keep servicing mpv during VO teardown.
            mpv_render_context_update(context);
            processControlEvents();
            if (!videoOutputDetached)
                QThread::usleep(1000);
        }
        mpv_render_context_free(context);
        restoreVideoOutput = true;
    }

    static void requestUpdate(void* context)
    {
        auto* state = static_cast<MpvD3D11State*>(context);
        std::lock_guard<std::mutex> lock(state->itemMutex);
        if (state->item)
            QMetaObject::invokeMethod(state->item, &QQuickItem::update, Qt::QueuedConnection);
    }

    void notifyReady()
    {
        std::lock_guard<std::mutex> lock(itemMutex);
        if (item)
            QMetaObject::invokeMethod(item, &MpvD3D11Item::ready, Qt::QueuedConnection);
    }

    QThread* thread = nullptr;
    MpvController* controller = nullptr;
    mpv_handle* handle = nullptr;
    mpv_handle* controlClient = nullptr;
    bool videoOutputDetached = false;
    bool restoreVideoOutput = false;
    std::mutex rendererMutex;
    MpvD3D11Renderer* activeRenderer = nullptr;
    std::mutex itemMutex;
    MpvD3D11Item* item = nullptr;
};

class MpvD3D11Renderer : public QQuickRhiItemRenderer
{
public:
    explicit MpvD3D11Renderer(std::shared_ptr<MpvD3D11State> state)
        : m_state(std::move(state))
    {
    }

    ~MpvD3D11Renderer() override
    {
        std::lock_guard<std::mutex> lock(m_state->rendererMutex);
        releaseContext();
    }

private:
    void releaseContext()
    {
        if (m_context)
        {
            mpv_render_context_set_update_callback(m_context, nullptr, nullptr);
            bool itemAlive;
            {
                std::lock_guard<std::mutex> lock(m_state->itemMutex);
                itemAlive = m_state->item != nullptr;
            }
            if (itemAlive)
                m_state->retireContext(m_context);
            else
                mpv_render_context_free(m_context);
            m_context = nullptr;
            m_state->activeRenderer = nullptr;
            MpvD3D11State::requestUpdate(m_state.get());
        }
    }

    bool deviceLost(ID3D11Device* device)
    {
        const HRESULT reason = device->GetDeviceRemovedReason();
        if (SUCCEEDED(reason))
            return false;
        if (!m_deviceLost)
            qWarning() << "Direct3D 11 device lost, waiting for Qt Quick recovery:"
                       << QString::number(static_cast<quint32>(reason), 16);
        m_deviceLost = true;
        return true;
    }

    void initialize(QRhiCommandBuffer*) override
    {
        if (rhi()->backend() != QRhi::D3D11)
            qFatal("The mpv Direct3D 11 renderer requires Qt Quick's Direct3D 11 backend");
        if (!colorTexture())
            qFatal("The mpv Direct3D 11 renderer requires a non-multisampled color texture");
    }

    void synchronize(QQuickRhiItem*) override
    {
    }

    void render(QRhiCommandBuffer* cb) override
    {
        std::lock_guard<std::mutex> lock(m_state->rendererMutex);
        m_state->processControlEvents();
        const auto* native = static_cast<const QRhiD3D11NativeHandles*>(rhi()->nativeHandles());
        auto* device = static_cast<ID3D11Device*>(native->dev);
        if (deviceLost(device))
            return;

        cb->beginPass(renderTarget(), Qt::black, {1.0f, 0}, nullptr,
                      QRhiCommandBuffer::ExternalContent);
        cb->beginExternal();
        const auto endPass = qScopeGuard([cb]() {
            cb->endExternal();
            cb->endPass();
        });
        static_cast<ID3D11DeviceContext*>(native->context)->ClearState();

        // A reparented item may reach its new window before the old node is released.
        if (m_state->activeRenderer && m_state->activeRenderer != this)
            return;

        if (!m_context || m_device != native->dev)
        {
            releaseContext();
            mpv_d3d11_init_params init{native->dev};
            mpv_render_param params[]{
                {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_D3D11)},
                {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &init},
                {MPV_RENDER_PARAM_INVALID, nullptr}
            };
            const int result = mpv_render_context_create(&m_context, m_state->handle, params);
            if (result < 0)
            {
                if (deviceLost(device))
                    return;
                qFatal("Failed to initialize mpv Direct3D 11 context: %s", mpv_error_string(result));
            }

            m_device = native->dev;
            m_state->activeRenderer = this;
            m_deviceLost = false;
            mpv_render_context_set_update_callback(m_context, &MpvD3D11State::requestUpdate, m_state.get());
            if (m_state->restoreVideoOutput)
            {
                m_state->setVideoOutput("libmpv", MpvD3D11State::AttachVideo);
                m_state->restoreVideoOutput = false;
            }
            m_state->notifyReady();
            qInfo() << "Initialized mpv Direct3D 11 renderer";
        }

        mpv_render_context_update(m_context);
        mpv_d3d11_fbo target{reinterpret_cast<void*>(colorTexture()->nativeTexture().object)};
        int blockForTargetTime = 0;
        mpv_render_param params[]{
            {MPV_RENDER_PARAM_D3D11_FBO, &target},
            {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForTargetTime},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };
        const int result = mpv_render_context_render(m_context, params);
        if (result < 0)
        {
            if (deviceLost(device))
                return;
            qFatal("Failed to render mpv Direct3D 11 frame: %s", mpv_error_string(result));
        }
    }

    // The render node can outlive its QQuickItem during scene graph teardown.
    std::shared_ptr<MpvD3D11State> m_state;
    mpv_render_context* m_context = nullptr;
    void* m_device = nullptr;
    bool m_deviceLost = false;
};

MpvD3D11Item::MpvD3D11Item(QQuickItem* parent)
    : QQuickRhiItem(parent), m_state(std::make_shared<MpvD3D11State>(this))
{
    connect(this, &MpvD3D11Item::setProperty, m_state->controller, &MpvController::setProperty,
            Qt::QueuedConnection);
}

MpvD3D11Item::~MpvD3D11Item()
{
    std::lock_guard<std::mutex> lock(m_state->itemMutex);
    m_state->item = nullptr;
}

MpvController* MpvD3D11Item::mpvController() const
{
    return m_state->controller;
}

QQuickRhiItemRenderer* MpvD3D11Item::createRenderer()
{
    return new MpvD3D11Renderer(m_state);
}
