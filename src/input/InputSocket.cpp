//
// Created by Tobias Hieta on 24/08/15.
//

#include <QDebug>
#include "InputSocket.h"
#include "Version.h"

/////////////////////////////////////////////////////////////////////////////////////////
bool InputSocket::initInput()
{
  return m_server->listen();
}

/////////////////////////////////////////////////////////////////////////////////////////
void InputSocket::clientConnected(QLocalSocket* socket)
{
  QVariantMap welcome;
  welcome.insert("version", Version::GetVersionString());
  welcome.insert("builddate", Version::GetBuildDate());

  m_server->sendMessage(welcome, socket);
}

/////////////////////////////////////////////////////////////////////////////////////////
void InputSocket::messageReceived(const QVariant& message)
{
  QVariantMap map = message.toMap();

  if (!map.contains("client") || !map.contains("source") || !map.contains("keycode"))
  {
    qWarning() << "Got packet from client but it was missing the important fields";
    return;
  }

  qDebug() << "Input from client:" << map.value("client").toString() << " - " <<
               map.value("source").toString() << map.value("keycode").toString();

  emit receivedInput(map.value("source").toString(), map.value("keycode").toString(), KeyPressed);
}
