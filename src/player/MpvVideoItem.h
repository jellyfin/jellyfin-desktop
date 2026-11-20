#ifndef MPVVIDEOITEM_H
#define MPVVIDEOITEM_H

#ifdef USE_MPV_D3D11
#include "MpvD3D11Item.h"
using MpvVideoItemBase = MpvD3D11Item;
#else
#include <MpvAbstractItem>
using MpvVideoItemBase = MpvAbstractItem;
#endif

class PlayerComponent;

class MpvVideoItem : public MpvVideoItemBase
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit MpvVideoItem(QQuickItem *parent = nullptr);
    void setPlayerComponent(PlayerComponent* player);

    MpvController* controller() { return mpvController(); }

private:
    void initialize();

    PlayerComponent* m_player = nullptr;
    bool m_initialized = false;
    bool m_ready = false;
};

#endif // MPVVIDEOITEM_H
