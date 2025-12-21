#include "PlatformInfo.h"

PlatformInfo::PlatformInfo(QObject *parent) : QObject(parent) {}

bool PlatformInfo::isWaylandMpv() const {
#ifdef USE_WAYLAND_SUBSURFACE
    return QGuiApplication::platformName() == "wayland";
#else
    return false;
#endif
}

bool PlatformInfo::isWayland() const {
    return QGuiApplication::platformName() == "wayland";
}
