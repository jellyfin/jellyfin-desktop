#ifndef MPVD3D11ITEM_H
#define MPVD3D11ITEM_H

// Windows replacement for MpvQt's MpvAbstractItem, which is OpenGL-only.
// mpv renders with the libmpv D3D11 render API (mpv 0.42) on the scene
// graph's own ID3D11Device, so d3d11va frames stay on the GPU (zero-copy)
// and D3D11 video filters such as d3d11vpp work.
// Same signals and mpvController() as MpvAbstractItem.

#include <QtQuick/QQuickRhiItem>
#include <QMutex>
#include <QStringList>
#include <QVariant>
#include <memory>

#include <mpv/client.h>
#include <mpv/render.h>

class MpvController;
class QThread;

// Shared between the item (GUI thread) and its renderer (render thread).
struct MpvD3D11RenderState
{
  QMutex mutex;
  mpv_handle* mpv = nullptr;
  mpv_render_context* ctx = nullptr;
  bool createFailed = false; // do not retry every frame
  QQuickItem* item = nullptr; // target of mpv's update callback

  // Must be called with the mutex held.
  void freeContext();
};

class MpvD3D11Item : public QQuickRhiItem
{
  Q_OBJECT

public:
  explicit MpvD3D11Item(QQuickItem* parent = nullptr);
  ~MpvD3D11Item() override;

Q_SIGNALS:
  void ready();
  void observeProperty(const QString& property, mpv_format format, uint64_t id = 0);
  void setProperty(const QString& property, const QVariant& value);
  void command(const QStringList& params);

protected:
  QQuickRhiItemRenderer* createRenderer() override;
  MpvController* mpvController() { return m_controller; }

private:
  QThread* m_workerThread = nullptr;
  MpvController* m_controller = nullptr;
  std::shared_ptr<MpvD3D11RenderState> m_state;
};

#endif // MPVD3D11ITEM_H
