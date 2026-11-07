#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>

#include "PowerComponentDBus.h"

#define DBUS_SERVICE_NAME "org.freedesktop.login1"
#define DBUS_SERVICE_PATH "/org/freedesktop/login1"
#define DBUS_INTERFACE "org.freedesktop.login1.Manager"

#define DBUS_SCREENSAVER_SERVICE_NAME "org.freedesktop.ScreenSaver"
#define DBUS_SCREENSAVER_SERVICE_PATH "/org/freedesktop/ScreenSaver"
#define DBUS_SCREENSAVER_INTERFACE "org.freedesktop.ScreenSaver"

/////////////////////////////////////////////////////////////////////////////////////////
bool PowerComponentDBus::PowerOff()
{
  return callPowerMethod("PowerOff");
}

/////////////////////////////////////////////////////////////////////////////////////////
bool PowerComponentDBus::Reboot()
{
  return callPowerMethod("Reboot");
}

/////////////////////////////////////////////////////////////////////////////////////////
bool PowerComponentDBus::callPowerMethod(QString method)
{
  if (QDBusConnection::systemBus().isConnected())
  {
    QDBusInterface iface(DBUS_SERVICE_NAME, DBUS_SERVICE_PATH, DBUS_INTERFACE,
                         QDBusConnection::systemBus());

    if (iface.isValid())
    {
      QDBusReply<bool> reply = iface.call(method, true);

      if (reply.isValid())
      {
        return true;
      }
      else
      {
        qCritical() << "callPowerMethod : Error while calling" << method << ":"
                     << reply.error().message();
        return false;
      }
    }
    else
    {
      qCritical() << "callPowerMethod : failed to retrieve interface.";
    }
  }
  else
  {
    qCritical() << "callPowerMethod : could not find system bus";
  }

  return false;
}

/////////////////////////////////////////////////////////////////////////////////////////
bool PowerComponentDBus::isPowerMethodAvailable(QString method)
{
  if (QDBusConnection::systemBus().isConnected())
  {
    QDBusInterface iface(DBUS_SERVICE_NAME, DBUS_SERVICE_PATH, DBUS_INTERFACE,
                         QDBusConnection::systemBus());

    if (iface.isValid())
    {
      QDBusReply<QString> reply = iface.call(method);

      if (reply.isValid())
      {
        return (reply.value() == "yes");
      }
      else
      {
        qCritical() << "isPowerMethodAvailable : Error while calling" << method << ":"
                     << reply.error().message();
        return false;
      }
    }
    else
    {
      qCritical() << "isPowerMethodAvailable : failed to retrieve interface.";
    }
  }
  else
  {
    qCritical() << "isPowerMethodAvailable : could not find system bus";
  }

  return false;
}

void PowerComponentDBus::doDisableScreensaver()
{
  if (m_screensaverInhibitCookie)
  {
    qInfo() << "doDisableScreensaver : already disabled.";
    return;
  }
  if (QDBusConnection::sessionBus().isConnected())
  {
    QDBusInterface iface(DBUS_SCREENSAVER_SERVICE_NAME, DBUS_SCREENSAVER_SERVICE_PATH,
                         DBUS_SCREENSAVER_INTERFACE, QDBusConnection::sessionBus());
    if (iface.isValid())
    {
      QDBusReply<unsigned int> reply = iface.call("Inhibit", "jellyfin-desktop", "playing");

      if (reply.isValid())
      {
        m_screensaverInhibitCookie = reply.value();
      }
      else
      {
        qCritical() << "doDisableScreensaver : Error while calling UnInhibit:"
                     << reply.error().message();
      }
    }
    else
    {
      qCritical() << "doDisableScreensaver : failed to retrieve interface.";
    }
  }
  else
  {
    qCritical() << "doDisableScreensaver : could not find session bus";
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void PowerComponentDBus::doEnableScreensaver()
{
  if (!m_screensaverInhibitCookie)
  {
    qInfo() << "doEnableScreensaver : already enabled.";
    return;
  }
  if (QDBusConnection::sessionBus().isConnected())
  {
    QDBusInterface iface(DBUS_SCREENSAVER_SERVICE_NAME, DBUS_SCREENSAVER_SERVICE_PATH,
                         DBUS_SCREENSAVER_INTERFACE, QDBusConnection::sessionBus());
    if (iface.isValid())
    {
      QDBusReply<void> reply = iface.call("UnInhibit", m_screensaverInhibitCookie);

      if (reply.isValid())
      {
        m_screensaverInhibitCookie = 0;
      }
      else
      {
        qCritical() << "doEnableScreensaver : Error while calling UnInhibit:"
                     << reply.error().message();
      }
    }
    else
    {
      qCritical() << "doEnableScreensaver : failed to retrieve interface.";
    }
  }
  else
  {
    qCritical() << "doEnableScreensaver : could not find session bus";
  }
}

