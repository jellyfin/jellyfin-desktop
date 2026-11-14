#include "DisplayManagerKDE.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <QDebug>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

#include <wayland-client.h>

#include "qwayland-kde-output-device-v2.h"
#include "qwayland-kde-output-management-v2.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
class KdeOutputMode : public QtWayland::kde_output_device_mode_v2
{
public:
  explicit KdeOutputMode(struct ::kde_output_device_mode_v2* object)
    : QtWayland::kde_output_device_mode_v2(object) {}
  ~KdeOutputMode() override { kde_output_device_mode_v2_destroy(object()); }

  QSize m_size;
  int m_refreshMilliHz = 0;
  bool m_removed = false;

protected:
  void kde_output_device_mode_v2_size(int32_t width, int32_t height) override
  {
    m_size = QSize(width, height);
  }
  void kde_output_device_mode_v2_refresh(int32_t refresh) override { m_refreshMilliHz = refresh; }
  void kde_output_device_mode_v2_removed() override { m_removed = true; }
};

///////////////////////////////////////////////////////////////////////////////////////////////////
class KdeOutputDevice : public QtWayland::kde_output_device_v2
{
public:
  explicit KdeOutputDevice(struct ::kde_output_device_v2* object)
    : QtWayland::kde_output_device_v2(object) {}
  ~KdeOutputDevice() override { release(); }

  QString m_name;
  bool m_enabled = false;
  bool m_removed = false;
  std::vector<std::unique_ptr<KdeOutputMode>> m_modes;
  KdeOutputMode* m_currentMode = nullptr;

protected:
  void kde_output_device_v2_mode(struct ::kde_output_device_mode_v2* mode) override
  {
    m_modes.push_back(std::make_unique<KdeOutputMode>(mode));
  }
  void kde_output_device_v2_current_mode(struct ::kde_output_device_mode_v2* mode) override
  {
    auto it = std::find_if(m_modes.begin(), m_modes.end(),
                           [mode](const auto& m) { return m->object() == mode; });
    m_currentMode = it != m_modes.end() ? it->get() : nullptr;
  }
  void kde_output_device_v2_enabled(int32_t enabled) override { m_enabled = enabled; }
  void kde_output_device_v2_name(const QString& name) override { m_name = name; }
  void kde_output_device_v2_removed() override { m_removed = true; }
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Only kde_output_device_registry_v2 (protocol version 21+) is supported. Older KWin versions
// announce each kde_output_device_v2 as a global instead; add that path if needed.
class KdeOutputDeviceRegistry : public QtWayland::kde_output_device_registry_v2
{
public:
  ~KdeOutputDeviceRegistry() override
  {
    m_devices.clear();
    stop();
    kde_output_device_registry_v2_destroy(object());
  }

  std::vector<std::unique_ptr<KdeOutputDevice>> m_devices;

protected:
  void kde_output_device_registry_v2_output(struct ::kde_output_device_v2* output) override
  {
    m_devices.push_back(std::make_unique<KdeOutputDevice>(output));
  }
};

///////////////////////////////////////////////////////////////////////////////////////////////////
class KdeOutputManagement : public QtWayland::kde_output_management_v2
{
public:
  ~KdeOutputManagement() override { kde_output_management_v2_destroy(object()); }
};

///////////////////////////////////////////////////////////////////////////////////////////////////
class KdeOutputConfiguration : public QtWayland::kde_output_configuration_v2
{
public:
  enum class State { Pending, Applied, Failed };

  explicit KdeOutputConfiguration(struct ::kde_output_configuration_v2* object)
    : QtWayland::kde_output_configuration_v2(object) {}
  ~KdeOutputConfiguration() override { destroy(); }

  State m_state = State::Pending;

protected:
  void kde_output_configuration_v2_applied() override { m_state = State::Applied; }
  void kde_output_configuration_v2_failed() override { m_state = State::Failed; }
};

///////////////////////////////////////////////////////////////////////////////////////////////////
static const wl_registry_listener s_registryListener = {
  [](void* data, wl_registry*, uint32_t name, const char* interface, uint32_t version) {
    static_cast<DisplayManagerKDE*>(data)->bindGlobal(name, interface, version);
  },
  [](void*, wl_registry*, uint32_t) {},
};

///////////////////////////////////////////////////////////////////////////////////////////////////
DisplayManagerKDE::DisplayManagerKDE(QObject* parent) : DisplayManager(parent)
{
  auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
  m_display = app ? app->display() : nullptr;
  if (!m_display)
    return;

  // Objects created from a registry on our queue inherit that queue.
  m_queue = wl_display_create_queue(m_display);
  auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(m_display));
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), m_queue);
  m_wlRegistry = wl_display_get_registry(wrapper);
  wl_proxy_wrapper_destroy(wrapper);

  wl_registry_add_listener(m_wlRegistry, &s_registryListener, this);
  wl_display_roundtrip_queue(m_display, m_queue);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
DisplayManagerKDE::~DisplayManagerKDE()
{
  // DisplayComponent is a static singleton, destroyed after QGuiApplication has already freed
  // the wl_display. The proxies are gone with it, so leak our wrappers instead of touching them.
  if (!qGuiApp)
  {
    (void)m_registry.release();
    (void)m_management.release();
    return;
  }

  m_registry.reset();
  m_management.reset();
  if (m_wlRegistry)
    wl_registry_destroy(m_wlRegistry);
  if (m_queue)
    wl_event_queue_destroy(m_queue);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void DisplayManagerKDE::bindGlobal(uint32_t name, const char* interface, uint32_t version)
{
  if (strcmp(interface, kde_output_device_registry_v2_interface.name) == 0 && version >= 21)
  {
    m_registry = std::make_unique<KdeOutputDeviceRegistry>();
    m_registry->init(m_wlRegistry, static_cast<int>(name),
                     std::min<int>(version, kde_output_device_registry_v2_interface.version));
  }
  else if (strcmp(interface, kde_output_management_v2_interface.name) == 0)
  {
    m_management = std::make_unique<KdeOutputManagement>();
    m_management->init(m_wlRegistry, static_cast<int>(name),
                       std::min<int>(version, kde_output_management_v2_interface.version));
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayManagerKDE::initialize()
{
  m_displays.clear();
  if (!m_registry || !m_management)
    return false;

  // First roundtrip announces the outputs, the second one delivers their properties.
  wl_display_roundtrip_queue(m_display, m_queue);
  wl_display_roundtrip_queue(m_display, m_queue);

  for (size_t o = 0; o < m_registry->m_devices.size(); o++)
  {
    const auto& device = m_registry->m_devices[o];
    if (device->m_removed || !device->m_enabled || !device->m_currentMode)
      continue;

    DMDisplayPtr dmDisplay = DMDisplayPtr::create();
    dmDisplay->m_id = m_displays.size();
    dmDisplay->m_name = device->m_name;
    dmDisplay->m_privId = static_cast<int>(o);
    m_displays[dmDisplay->m_id] = dmDisplay;

    for (size_t m = 0; m < device->m_modes.size(); m++)
    {
      const auto& deviceMode = device->m_modes[m];
      if (deviceMode->m_removed)
        continue;

      DMVideoModePtr mode = DMVideoModePtr::create();
      mode->m_id = dmDisplay->m_videoModes.size();
      mode->m_privId = static_cast<int>(m);
      mode->m_width = deviceMode->m_size.width();
      mode->m_height = deviceMode->m_size.height();
      mode->m_refreshRate = static_cast<float>(deviceMode->m_refreshMilliHz / 1000.0);
      mode->m_bitsPerPixel = 0; // not exposed by the protocol
      mode->m_interlaced = false;
      dmDisplay->m_videoModes[mode->m_id] = mode;
    }
  }

  if (m_displays.empty())
    return false;

  return DisplayManager::initialize();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayManagerKDE::setDisplayMode(int display, int mode)
{
  if (!isValidDisplayMode(display, mode))
    return false;

  const DMDisplayPtr& dmDisplay = m_displays[display];
  const auto& device = m_registry->m_devices[dmDisplay->m_privId];
  const auto& deviceMode = device->m_modes[dmDisplay->m_videoModes[mode]->m_privId];

  KdeOutputConfiguration config(m_management->create_configuration());
  config.mode(device->object(), deviceMode->object());
  config.apply();

  QElapsedTimer timer;
  timer.start();
  while (config.m_state == KdeOutputConfiguration::State::Pending && timer.elapsed() < 5000)
  {
    if (wl_display_roundtrip_queue(m_display, m_queue) < 0)
      break;
  }

  QString modeName = dmDisplay->m_videoModes[mode]->getPrettyName();
  if (config.m_state == KdeOutputConfiguration::State::Pending)
    qWarning() << "No answer from KWin when setting mode" << modeName << "on" << dmDisplay->m_name;
  else if (config.m_state == KdeOutputConfiguration::State::Failed)
    qWarning() << "KWin refused mode" << modeName << "on" << dmDisplay->m_name;

  return config.m_state == KdeOutputConfiguration::State::Applied;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManagerKDE::getCurrentDisplayMode(int display)
{
  if (!isValidDisplay(display))
    return -1;

  const DMDisplayPtr& dmDisplay = m_displays[display];
  const auto& device = m_registry->m_devices[dmDisplay->m_privId];

  for (const DMVideoModePtr& mode : dmDisplay->m_videoModes)
  {
    if (device->m_modes[mode->m_privId].get() == device->m_currentMode)
      return mode->m_id;
  }
  return -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManagerKDE::getMainDisplay()
{
  QScreen* primary = QGuiApplication::primaryScreen();
  int display = primary ? findDisplayByName(primary->name()) : -1;
  return display >= 0 ? display : (m_displays.empty() ? -1 : 0);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManagerKDE::getDisplayFromPoint(int, int)
{
  // Wayland does not expose global window positions, see getDisplayFromWindow().
  return getMainDisplay();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManagerKDE::getDisplayFromWindow(QWindow* window)
{
  int display = window && window->screen() ? findDisplayByName(window->screen()->name()) : -1;
  return display >= 0 ? display : getMainDisplay();
}
