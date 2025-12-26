#ifndef PLATFORMINFO_H
#define PLATFORMINFO_H

#include <QObject>
#include <QGuiApplication>

class PlatformInfo : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isWaylandMpv READ isWaylandMpv CONSTANT)
    Q_PROPERTY(bool isWayland READ isWayland CONSTANT)

public:
    explicit PlatformInfo(QObject *parent = nullptr);

    Q_INVOKABLE bool isWaylandMpv() const;
    Q_INVOKABLE bool isWayland() const;
};

#endif // PLATFORMINFO_H
