#ifndef POWERCOMPONENTWIN_H
#define POWERCOMPONENTWIN_H

#include "PowerComponent.h"


class PowerComponentWin : public PowerComponent
{
public:
  PowerComponentWin();

  int getPowerCapabilities() override
  {
    int flags = CAP_SUSPEND;
    if (m_hasPrivileges)
      flags |= CAP_POWER_OFF | CAP_REBOOT;
    return flags;
  }

  bool PowerOff() override;
  bool Reboot() override;
  bool Suspend() override;

protected:
  void doDisableScreensaver() override;
  void doEnableScreensaver() override;

private:
  bool m_hasPrivileges;

};

#endif // POWERCOMPONENTWIN_H
