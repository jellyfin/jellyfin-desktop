#ifndef DISPLAYMANAGERKDE_H_
#define DISPLAYMANAGERKDE_H_

#include <memory>

#include "display/DisplayManager.h"

struct wl_display;
struct wl_event_queue;
struct wl_registry;
class KdeOutputDeviceRegistry;
class KdeOutputManagement;

// Display mode switching for KDE Plasma (KWin) Wayland sessions, using the
// kde_output_device_v2 and kde_output_management_v2 protocols.
class DisplayManagerKDE : public DisplayManager
{
  Q_OBJECT
public:
  explicit DisplayManagerKDE(QObject* parent);
  ~DisplayManagerKDE() override;

  bool initialize() override;
  bool setDisplayMode(int display, int mode) override;
  int getCurrentDisplayMode(int display) override;
  int getMainDisplay() override;
  int getDisplayFromPoint(int x, int y) override;
  int getDisplayFromWindow(QWindow* window) override;

  // Called from the wl_registry listener.
  void bindGlobal(uint32_t name, const char* interface, uint32_t version);

private:
  // Private queue, so dispatching never runs Qt's own Wayland event handlers.
  wl_display* m_display = nullptr;
  wl_event_queue* m_queue = nullptr;
  wl_registry* m_wlRegistry = nullptr;
  std::unique_ptr<KdeOutputDeviceRegistry> m_registry;
  std::unique_ptr<KdeOutputManagement> m_management;
};

#endif /* DISPLAYMANAGERKDE_H_ */
