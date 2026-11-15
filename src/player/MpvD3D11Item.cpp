#include "MpvD3D11Item.h"

#include <MpvController>
#include <QDebug>
#include <QThread>
#include <d3d11.h>
#include <mpv/render_d3d11.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <mutex>

struct MpvD3D11State
{
    explicit MpvD3D11State(MpvD3D11Item* videoItem) : item(videoItem)
    {
        thread = new QThread;
        controller = new MpvController;
        controller->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, controller, &QObject::deleteLater);
        thread->start();
        QMetaObject::invokeMethod(controller, &MpvController::init, Qt::BlockingQueuedConnection);
        handle = controller->mpv();
    }

    ~MpvD3D11State()
    {
        mpv_set_wakeup_callback(handle, nullptr, nullptr);
        thread->quit();
        thread->wait();
        thread->deleteLater();
        mpv_terminate_destroy(handle);
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
        releaseContext();
    }

private:
    void releaseContext()
    {
        if (m_context)
        {
            mpv_render_context_set_update_callback(m_context, nullptr, nullptr);
            mpv_render_context_free(m_context);
            m_context = nullptr;
        }
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
        cb->beginPass(renderTarget(), Qt::black, {1.0f, 0}, nullptr,
                      QRhiCommandBuffer::ExternalContent);
        cb->beginExternal();
        const auto* native = static_cast<const QRhiD3D11NativeHandles*>(rhi()->nativeHandles());
        static_cast<ID3D11DeviceContext*>(native->context)->ClearState();

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
                qFatal("Failed to initialize mpv Direct3D 11 context: %s", mpv_error_string(result));

            m_device = native->dev;
            mpv_render_context_set_update_callback(m_context, &MpvD3D11State::requestUpdate, m_state.get());
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
        cb->endExternal();
        cb->endPass();
        if (result < 0)
            qFatal("Failed to render mpv Direct3D 11 frame: %s", mpv_error_string(result));
    }

    // The render node can outlive its QQuickItem during scene graph teardown.
    std::shared_ptr<MpvD3D11State> m_state;
    mpv_render_context* m_context = nullptr;
    void* m_device = nullptr;
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
