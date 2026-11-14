#include "MpvVideoItem.h"
#include "PlayerComponent.h"
#include <MpvController>
#include <QDebug>

MpvVideoItem::MpvVideoItem(QQuickItem *parent)
    : MpvAbstractItem(parent)
{
    connect(this, &MpvAbstractItem::ready, this, [this]() {
        qDebug() << "MpvVideoItem ready() signal fired!";
        m_ready = true;
        initialize();
    });

    qDebug() << "MpvVideoItem constructed";
    // Critical: Set vo=libmpv for Qt integration
    Q_EMIT setProperty("vo", "libmpv");

#ifdef Q_OS_WIN32
    // Force desktop OpenGL and disable advanced features for compatibility with older GPUs
    Q_EMIT setProperty("gpu-api", "opengl");
    Q_EMIT setProperty("opengl-es", "no");
#endif
}

void MpvVideoItem::setPlayerComponent(PlayerComponent* player)
{
    qDebug() << "MpvVideoItem::setPlayerComponent called, mpvController():" << mpvController();
    m_player = player;
    initialize();
}

void MpvVideoItem::initialize() {
    if (m_initialized) {
        qDebug() << "MpvVideoItem is already initialized, ignoring";
        return;
    }

    if (!m_ready) {
        qDebug() << "MpvVideoItem not ready yet, waiting for ready() signal";
        return;
    }

    if (!m_player) {
        qDebug() << "MpvVideoItem has no player, waiting for setPlayerComponent()";
        return;
    }

    m_initialized = true;

    qDebug() << "MpvVideoItem ready, initializing now";
    m_player->setMpvController(mpvController());
    m_player->initializeMpv();
}
