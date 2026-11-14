#ifndef MPVVIDEOITEM_H
#define MPVVIDEOITEM_H

#ifdef _WIN32
// Direct3D 11 renderer: needed for d3d11va zero-copy and RTX/Intel VSR.
#include "MpvD3D11Item.h"
using MpvItemBase = MpvD3D11Item;
#else
#include <MpvAbstractItem>
using MpvItemBase = MpvAbstractItem;
#endif

class PlayerComponent;

class MpvVideoItem : public MpvItemBase
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
