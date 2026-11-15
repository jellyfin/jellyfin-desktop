#ifndef MPVD3D11ITEM_H
#define MPVD3D11ITEM_H

#include <QQuickRhiItem>
#include <memory>

class MpvController;
struct MpvD3D11State;

class MpvD3D11Item : public QQuickRhiItem
{
    Q_OBJECT

public:
    explicit MpvD3D11Item(QQuickItem* parent = nullptr);
    ~MpvD3D11Item() override;

    MpvController* mpvController() const;

signals:
    void ready();
    void setProperty(const QString& property, const QVariant& value);

protected:
    QQuickRhiItemRenderer* createRenderer() override;

private:
    std::shared_ptr<MpvD3D11State> m_state;
};

#endif
