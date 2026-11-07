#ifndef POWERCOMPONENTX11_H
#define POWERCOMPONENTX11_H

#include <QTimer>
#include <QProcess>

#include "PowerComponent.h"

class PowerComponentX11 : public PowerComponent
{
  Q_OBJECT

public:
  PowerComponentX11();

protected:
  void doDisableScreensaver() override;
  void doEnableScreensaver() override;

private slots:
  void onTimer();
  void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
  void onProcessError(QProcess::ProcessError error);

private:
  bool m_broken = false;
  QTimer* m_timer = 0;
  QProcess* m_process = 0;
};

#endif // POWERCOMPONENTX11_H
