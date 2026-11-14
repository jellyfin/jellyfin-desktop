#include "MpvD3D11Item.h"

#include <MpvController>

#include <QDebug>
#include <QMetaObject>
#include <QThread>
#include <QColor>
#include <QQuickWindow>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <d3d11.h>

#if __has_include(<mpv/render_d3d11.h>)
#include <mpv/render_d3d11.h>
#else
// The shinchiro libmpv dev package (20261018) is built from a commit that has
// the D3D11 render API (render.h declares the params) but does not ship this
// header. Same definitions as mpv's include/mpv/render_d3d11.h.
typedef struct mpv_d3d11_init_params {
  void* device; // ID3D11Device*
} mpv_d3d11_init_params;

typedef struct mpv_d3d11_fbo {
  void* tex; // ID3D11Texture2D*
} mpv_d3d11_fbo;
#endif

#if MPV_CLIENT_API_VERSION < MPV_MAKE_VERSION(2, 6)
#error "The Direct3D 11 video renderer needs libmpv with client API 2.6 or newer (mpv 0.42)."
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
void MpvD3D11RenderState::freeContext()
{
  if (ctx)
  {
    mpv_render_context_free(ctx);
    ctx = nullptr;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static void onMpvRedraw(void* ctx)
{
  // Called from mpv threads. mpv_render_context_free() guarantees no further
  // calls once it returns, and the context is always freed before the item
  // is destroyed, so the item pointer is valid here.
  QMetaObject::invokeMethod(static_cast<QQuickItem*>(ctx), &QQuickItem::update, Qt::QueuedConnection);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class MpvD3D11Renderer : public QQuickRhiItemRenderer
{
public:
  explicit MpvD3D11Renderer(std::shared_ptr<MpvD3D11RenderState> state)
    : m_state(std::move(state))
  {
  }

  ~MpvD3D11Renderer() override
  {
    // Runs on the render thread when the scene graph goes away (window
    // closed or recreated). The context belongs to this device, free it now.
    QMutexLocker lock(&m_state->mutex);
    m_state->freeContext();
  }

protected:
  void initialize(QRhiCommandBuffer*) override
  {
    // Called again on every color buffer resize. The mpv context is created
    // lazily in render(), inside QRhi's external-content block, because mpv
    // touches the shared immediate context while creating its resources.
  }

  void synchronize(QQuickRhiItem*) override
  {
  }

  void render(QRhiCommandBuffer* cb) override
  {
    QMutexLocker lock(&m_state->mutex);
    QRhiTexture* tex = colorTexture();
    if (!m_state->mpv || !tex)
      return;

    QRhi* r = rhi();
    if (r->backend() != QRhi::D3D11)
    {
      qCritical() << "MpvD3D11Item: the scene graph is not using Direct3D 11 (backend"
                  << r->backendName() << ") - no video output.";
      return;
    }
    auto* handles = static_cast<const QRhiD3D11NativeHandles*>(r->nativeHandles());
    if (!handles || !handles->dev || !handles->context)
    {
      qCritical() << "MpvD3D11Item: no ID3D11Device from Qt - no video output.";
      return;
    }

    // mpv uses the device's immediate context directly and does not restore
    // any state. ExternalContent + beginExternal() makes QRhi submit what it
    // recorded so far; endExternal() makes it re-set all of its state.
    cb->beginPass(renderTarget(), Qt::black, {1.0f, 0}, nullptr, QRhiCommandBuffer::ExternalContent);
    cb->beginExternal();

    // render_d3d11.h: stages mpv does not set must be at ClearState() defaults.
    static_cast<ID3D11DeviceContext*>(handles->context)->ClearState();

    if (!m_state->ctx && !m_state->createFailed)
      createContext(handles->dev);

    if (m_state->ctx)
    {
      mpv_d3d11_fbo fbo{reinterpret_cast<void*>(static_cast<quintptr>(tex->nativeTexture().object))};
      mpv_render_param params[] = {
        {MPV_RENDER_PARAM_D3D11_FBO, &fbo},
        {MPV_RENDER_PARAM_INVALID, nullptr}};
      mpv_render_context_render(m_state->ctx, params);
    }

    cb->endExternal();
    cb->endPass();
  }

private:
  // Called from render() with the mutex held, inside the external block.
  void createContext(void* device)
  {
    mpv_d3d11_init_params init{device};
    mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_D3D11)},
      {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &init},
      {MPV_RENDER_PARAM_INVALID, nullptr}};

    int err = mpv_render_context_create(&m_state->ctx, m_state->mpv, params);
    if (err < 0)
    {
      // No video output, but the application (UI, audio) keeps working.
      m_state->ctx = nullptr;
      m_state->createFailed = true;
      qCritical() << "MpvD3D11Item: failed to create the mpv D3D11 render context:" << mpv_error_string(err);
    }
    else
    {
      mpv_render_context_set_update_callback(m_state->ctx, onMpvRedraw, m_state->item);
      qInfo() << "MpvD3D11Item: mpv renders through Direct3D 11";
    }

    QMetaObject::invokeMethod(m_state->item, "ready", Qt::QueuedConnection);
  }

  std::shared_ptr<MpvD3D11RenderState> m_state;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
MpvD3D11Item::MpvD3D11Item(QQuickItem* parent)
  : QQuickRhiItem(parent)
  , m_state(std::make_shared<MpvD3D11RenderState>())
{
  // Opaque, single-sampled, non-sRGB RGBA8: what mpv's D3D11 FBO accepts.
  setColorBufferFormat(TextureFormat::RGBA8);
  setSampleCount(1);
  setAlphaBlending(false);

  m_workerThread = new QThread;
  m_controller = new MpvController;
  m_workerThread->start();
  m_controller->moveToThread(m_workerThread);
  QMetaObject::invokeMethod(m_controller, &MpvController::init, Qt::BlockingQueuedConnection);

  m_state->mpv = m_controller->mpv();
  m_state->item = this;

  connect(m_workerThread, &QThread::finished, m_controller, &MpvController::deleteLater);
  connect(this, &MpvD3D11Item::observeProperty, m_controller, &MpvController::observeProperty, Qt::QueuedConnection);
  connect(this, &MpvD3D11Item::setProperty, m_controller, &MpvController::setProperty, Qt::QueuedConnection);
  connect(this, &MpvD3D11Item::command, m_controller, &MpvController::command, Qt::QueuedConnection);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
MpvD3D11Item::~MpvD3D11Item()
{
  mpv_handle* mpv = nullptr;
  {
    // The render context must be gone before mpv_terminate_destroy(). If the
    // scene graph is still alive, the renderer may be using it right now: the
    // mutex waits for that frame to finish.
    QMutexLocker lock(&m_state->mutex);
    m_state->freeContext();
    mpv = m_state->mpv;
    m_state->mpv = nullptr;
    m_state->item = nullptr;
  }

  if (mpv)
    mpv_set_wakeup_callback(mpv, nullptr, nullptr);

  m_workerThread->quit();
  m_workerThread->wait();
  m_workerThread->deleteLater();

  if (mpv)
    mpv_terminate_destroy(mpv);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QQuickRhiItemRenderer* MpvD3D11Item::createRenderer()
{
  // Called during scene graph sync (GUI thread blocked). Tearing down the
  // scene graph would free the mpv render context and lose the video output,
  // so keep it across hide/minimize, like MpvQt does.
  if (QQuickWindow* w = window())
  {
    w->setPersistentSceneGraph(true);
    w->setPersistentGraphics(true);
  }
  return new MpvD3D11Renderer(m_state);
}
