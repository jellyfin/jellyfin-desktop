#ifndef POWERCOMPONENTDBUS_H
#define POWERCOMPONENTDBUS_H

#include <QString>

#include "PowerComponent.h"

class PowerComponentDBus : public PowerComponent
{
  public:
    PowerComponentDBus() : PowerComponent(0) {};
    ~PowerComponentDBus() {};

  public Q_SLOTS:

  int getPowerCapabilities() override
  {
    int flags = 0;
    if (isPowerMethodAvailable("CanPowerOff"))
      flags |= CAP_POWER_OFF;
    if (isPowerMethodAvailable("CanReboot"))
      flags |= CAP_REBOOT;
    if (isPowerMethodAvailable("CanSuspend"))
      flags |= CAP_SUSPEND;
    return flags;
  }

  bool PowerOff() override;
  bool Reboot() override;
  bool Suspend() override { return callPowerMethod("Suspend"); }

  private:
    bool callPowerMethod(QString method);
    bool isPowerMethodAvailable(QString method);

    QString m_inhibitHandle;

  protected:
    void doDisableScreensaver() override;
    void doEnableScreensaver() override;
};

#endif // POWERCOMPONENTDBUS_H
