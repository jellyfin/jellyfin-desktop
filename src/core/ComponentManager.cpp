#include <QObject>
#include <QtQml>
#include <qqmlwebchannel.h>
#include <QDebug>

#include "ComponentManager.h"

#include "power/PowerComponent.h"
#include "input/InputComponent.h"
#include "player/PlayerComponent.h"
#include "display/DisplayComponent.h"
#include "system/SystemComponent.h"
#include "system/CertificateComponent.h"
#include "settings/SettingsComponent.h"
#include "taskbar/TaskbarComponent.h"
#include "ui/WindowManager.h"
#ifdef LINUX_DBUS
#include "mpris/MprisComponent.h"
#endif

#ifdef KONVERGO_OPENELEC
#include "system/openelec/OESystemComponent.h"
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
ComponentManager::ComponentManager() : QObject(nullptr)
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void ComponentManager::registerComponent(ComponentBase* comp)
{
  if (m_components.contains(comp->componentName()))
  {
    qCritical() << "Component" << comp->componentName() << "already registered!";
    return;
  }
  
  if (comp->componentInitialize())
  {
    qInfo() << "Component:" << comp->componentName() << "inited";
    m_components[comp->componentName()] = comp;

    // define component as property for qml
    m_qmlProperyMap.insert(comp->componentName(), QVariant::fromValue(comp));
  }
  else
  {
    qCritical() << "Failed to init component:" << comp->componentName();
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void ComponentManager::initialize()
{
  // then settings, since all other components
  // might have some settings
  //
  registerComponent(&SettingsComponent::Get());
  registerComponent(&InputComponent::Get());
  registerComponent(&SystemComponent::Get());
  registerComponent(&CertificateComponent::Get());
  registerComponent(&DisplayComponent::Get());
  registerComponent(&PlayerComponent::Get());
  registerComponent(&PowerComponent::Get());
  registerComponent(&TaskbarComponent::Get());
  registerComponent(&WindowManager::Get());
#ifdef LINUX_DBUS
  registerComponent(&MprisComponent::Get());
#endif

#ifdef KONVERGO_OPENELEC
  registerComponent(&OESystemComponent::Get());
#endif

  for(ComponentBase* component : m_components.values())
    component->componentPostInitialize();
}

/////////////////////////////////////////////////////////////////////////////////////////
void ComponentManager::setWebChannel(QWebChannel* webChannel)
{
  for(ComponentBase* comp : m_components.values())
  {
    if (comp->componentExport())
    {
      qDebug() << "Adding component:" << comp->componentName() << "to webchannel";
      webChannel->registerObject(comp->componentName(), comp);
    }
  }
}
