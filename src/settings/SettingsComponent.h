#ifndef SETTINGSCOMPONENT_H
#define SETTINGSCOMPONENT_H

#include <QObject>
#include "utils/Utils.h"
#include "ComponentManager.h"
#include "SettingsValue.h"

#define SETTINGS_SECTION_AUDIO "audio"
#define SETTINGS_SECTION_VIDEO "video"
#define SETTINGS_SECTION_MAIN "main"
#define SETTINGS_SECTION_SYSTEM "system"
#define SETTINGS_SECTION_STATE "state"
#define SETTINGS_SECTION_PATH "path"
#define SETTINGS_SECTION_WEBCLIENT "webclient"
#define SETTINGS_SECTION_SUBTITLES "subtitles"
#define SETTINGS_SECTION_OVERRIDES "overrides"
#define SETTINGS_SECTION_CEC "cec"
#define SETTINGS_SECTION_APPLEREMOTE "appleremote"
#define SETTINGS_SECTION_MPRIS "mpris"
#define SETTINGS_SECTION_OTHER "other"

#define AUDIO_DEVICE_TYPE_BASIC "basic"
#define AUDIO_DEVICE_TYPE_SPDIF "spdif"
#define AUDIO_DEVICE_TYPE_HDMI "hdmi"


class SettingsSection;

///////////////////////////////////////////////////////////////////////////////////////////////////
class SettingsComponent : public ComponentBase
{
  Q_OBJECT
  Q_PROPERTY(bool windowsTrayIcon READ enableWindowsTrayIcon NOTIFY windowsTrayIconChanged)
  DEFINE_SINGLETON(SettingsComponent);

public:
  bool componentInitialize() override;
  void componentPostInitialize() override;

  const char* componentName() override { return "settings"; }
  bool componentExport() override { return true; }

  SettingsSection* getSection(const QString& sectionID)
  {
    return m_sections.value(sectionID, nullptr);
  }

  void setCommandLineValues(const QStringList& values);

  // JS interface
  Q_INVOKABLE void setValue(const QString& sectionID, const QString& key, const QVariant& value);
  Q_INVOKABLE void setValues(const QVariantMap& options);
  Q_INVOKABLE QVariant value(const QString& sectionID, const QString& key);
  Q_INVOKABLE QVariant orderedSections();
  Q_INVOKABLE QVariant allValues(const QString& section = "");
  // Note: the naming "remove" is a lie - it will remove the affected keys only if they are not
  //       declared in settings_descriptions.json. Also, sections are never removed, even if they
  //       remain empty.
  Q_INVOKABLE void removeValue(const QString& sectionOrKey);
  Q_INVOKABLE void resetToDefaultAll();
  Q_INVOKABLE void resetToDefault(const QString& sectionID);
  Q_INVOKABLE QVariantList settingDescriptions();
  Q_INVOKABLE QString getWebClientUrl(bool desktop);
  Q_INVOKABLE QString getExtensionPath();
  Q_INVOKABLE QString getClientName();
  Q_INVOKABLE bool ignoreSSLErrors();
  Q_INVOKABLE bool autodetectCertBundle();
  Q_INVOKABLE QString detectCertBundlePath();
  Q_INVOKABLE bool allowBrowserZoom();
  bool enableWindowsTrayIcon();

  // host commands
  Q_SLOT Q_INVOKABLE void cycleSettingCommand(const QString& args);
  Q_SLOT Q_INVOKABLE void setSettingCommand(const QString& args);

  void updatePossibleValues(const QString& sectionID, const QString& key, const QVariantList& possibleValues);

  void saveSettings();
  void saveStorage();
  void load();

  // Fired when a section's description is updated.
  Q_SIGNAL void groupUpdate(const QString& section, const QVariant& description);

  // Fired when a subset of a section's values are updated. The values parameter will
  // contain the names of the changed values as keys, and the new settings values as
  // map values. Settings which are part of the section, but did not change, are not
  // part of the map.
  Q_SIGNAL void sectionValueUpdate(const QString& section, const QVariantMap& values);

  // Windows system tray icon enabled/disabled.
  Q_SIGNAL void windowsTrayIconChanged();

  // A hack to load a value from the config file at very early init time, before
  // the SettingsComponent is created.
  //
  static QVariant readPreinitValue(const QString& sectionID, const QString& key);

  // Moves the current settings file to plexmediaplayer.conf.old to make way for new
  // configuration.
  //
  static bool resetAndSaveOldConfiguration();

private:
  explicit SettingsComponent(QObject *parent = nullptr);
  bool loadDescription();
  void parseSection(const QJsonObject& sectionObject);
  int platformMaskFromObject(const QJsonObject& object);
  Platform platformFromString(const QString& platformString);
  void saveSection(SettingsSection* section);

  QMap<QString, SettingsSection*> m_sections;

  int m_settingsVersion;
  int m_sectionIndex;
  bool m_cliIgnoreSSLErrors = false;

  void loadConf(const QString& path, bool storage);
};

#endif // SETTINGSCOMPONENT_H
