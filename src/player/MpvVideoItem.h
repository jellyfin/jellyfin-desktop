#ifndef MPVVIDEOITEM_H
#define MPVVIDEOITEM_H

#include <MpvAbstractItem>

class PlayerComponent;

class MpvVideoItem : public MpvAbstractItem
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
    bool m_initialized;
    bool m_ready;
};

#endif // MPVVIDEOITEM_H
