#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QVariantMap>

#include "PowerComponentDBus.h"

#define DBUS_SERVICE_NAME "org.freedesktop.login1"
#define DBUS_SERVICE_PATH "/org/freedesktop/login1"
#define DBUS_INTERFACE "org.freedesktop.login1.Manager"

#define DBUS_PORTAL_INHIBIT_INTERFACE "org.freedesktop.portal.Inhibit"
#define DBUS_PORTAL_REQUEST_INTERFACE "org.freedesktop.portal.Request"
#define DBUS_PORTAL_SERVICE_NAME "org.freedesktop.portal.Desktop"
#define DBUS_PORTAL_SERVICE_PATH "/org/freedesktop/portal/desktop"

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
  if (!m_inhibitHandle.isEmpty())
  {
    qInfo() << "doDisableScreensaver : already disabled.";
    return;
  }

  if (QDBusConnection::sessionBus().isConnected())
  {
    QDBusMessage msg = QDBusMessage::createMethodCall(DBUS_PORTAL_SERVICE_NAME, DBUS_PORTAL_SERVICE_PATH, DBUS_PORTAL_INHIBIT_INTERFACE, "Inhibit");
    msg << QString() << 8u << QVariantMap{};
    QDBusReply<QDBusObjectPath> reply = QDBusConnection::sessionBus().call(msg);
    if (reply.isValid())
      m_inhibitHandle = reply.value().path();
    else
    {
      qCritical() << "doDisableScreensaver : error while calling portal inhibit:" << reply.error().message();
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
  if (m_inhibitHandle.isEmpty())
  {
    qInfo() << "doEnableScreensaver : already enabled.";
    return;
  }
  if (QDBusConnection::sessionBus().isConnected())
  {
    QDBusMessage msg = QDBusMessage::createMethodCall(DBUS_PORTAL_SERVICE_NAME, m_inhibitHandle, DBUS_PORTAL_REQUEST_INTERFACE, "Close");
    QDBusReply<void> reply = QDBusConnection::sessionBus().call(msg);
    if (reply.isValid())
      m_inhibitHandle.clear();
    else
    {
      qCritical() << "doEnableScreensaver : error while closing portal request:" << reply.error().message();
    }
  }
  else
  {
    qCritical() << "doEnableScreensaver : could not find session bus";
  }
}

