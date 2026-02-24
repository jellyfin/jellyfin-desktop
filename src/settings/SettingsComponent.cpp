#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include "SettingsComponent.h"
#include "SettingsSection.h"
#include "Paths.h"
#include "core/ProfileManager.h"
#include "utils/Utils.h"
#include "AudioSettingsController.h"
#include "Names.h"

#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include "input/InputComponent.h"
#include "system/SystemComponent.h"
#include "Version.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
SettingsComponent::SettingsComponent(QObject *parent) : ComponentBase(parent), m_settingsVersion(-1)
{
  connect(this, &SettingsComponent::sectionValueUpdate, this, [this](const QString& section, const QVariantMap& values)
  {
    if (section == SETTINGS_SECTION_MAIN && values.contains("enableWindowsTrayIcon"))
      emit windowsTrayIconChanged();
  });
}

/////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::componentPostInitialize()
{
  InputComponent::Get().registerHostCommand("cycle_setting", this, "cycleSettingCommand");
  InputComponent::Get().registerHostCommand("set_setting", this, "setSettingCommand");
}

/////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::cycleSettingCommand(const QString& args)
{
  QString settingName = args;
  QStringList sub = settingName.split(".");
  if (sub.size() != 2)
  {
    qCritical() << "Setting must be in the form section.name but got:" << settingName;
    return;
  }
  QString sectionID = sub[0];
  QString valueName = sub[1];
  SettingsSection* section = getSection(sectionID);
  if (!section)
  {
    qCritical() << "Section" << sectionID << "is unknown";
    return;
  }
  QVariantList values = section->possibleValues(valueName);
  // If no possible values are defined, check the type of the default value.
  // In the case it's a boolean simply negate the current value to cycle through.
  // Otherwise log an error message, that it's not possible to cycle through the value.
  if (values.empty())
  {
    if (section->defaultValue(valueName).typeId()== QMetaType::Bool)
    {
      QVariant currentValue = section->value(valueName);
      auto nextValue = !currentValue.toBool();
      setValue(sectionID, valueName, nextValue);
      qDebug() << "Setting" << settingName << "to " << (nextValue ? "Enabled" : "Disabled");
      emit SystemComponent::Get().settingsMessage(valueName, nextValue ? "Enabled" : "Disabled");
      return;
    }
    else
    {
      qCritical() << "Setting" << settingName << "is unknown or is not cycleable.";
      return;
    }
  }
  QVariant currentValue = section->value(valueName);
  int nextValueIndex = 0;
  for (int n = 0; n < values.size(); n++)
  {
    if (currentValue == values[n].toMap()["value"])
    {
      nextValueIndex = n + 1;
      break;
    }
  }
  if (nextValueIndex >= values.size())
    nextValueIndex = 0;
  auto nextSetting = values[nextValueIndex].toMap();
  auto nextValue = nextSetting["value"];
  qDebug() << "Setting" << settingName << "to" << nextValue;
  setValue(sectionID, valueName, nextValue);
  emit SystemComponent::Get().settingsMessage(valueName, nextSetting["title"].toString());
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::setSettingCommand(const QString& args)
{
  qsizetype spaceIndex = args.indexOf(" ");
  if (spaceIndex < 0)
  {
    qCritical() << "No value provided to settings set command.";
    return;
  }
  QString settingName = args.mid(0, spaceIndex);
  QString settingValue = args.mid(spaceIndex + 1);
  qsizetype subIndex = settingName.indexOf(".");
  if (subIndex < 0 || subIndex == args.size() - 1)
  {
    qCritical() << "Setting must be in the form section.name but got:" << settingName;
    return;
  }
  QString sectionID = settingName.mid(0, subIndex);
  QString valueName = settingName.mid(subIndex + 1);
  SettingsSection* section = getSection(sectionID);
  if (!section)
  {
    qCritical() << "Section" << sectionID << "is unknown";
    return;
  }
  QString jsonString = "{\"value\": " + settingValue + "}";
  QJsonParseError err;
  QVariant value = QJsonDocument::fromJson(jsonString.toUtf8(), &err).object()["value"].toVariant();
  printf("val: '%s'\n", settingValue.toUtf8().data());
  if (!value.isValid())
  {
    qCritical() << "Invalid settings value:" << settingValue << "(if it's a string, make sure to quote it)";
    return;
  }
  qDebug() << "Setting" << settingName << "to" << value;
  setValue(sectionID, valueName, value);
  emit SystemComponent::Get().settingsMessage(valueName, value.toString());
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::updatePossibleValues(const QString &sectionID, const QString &key, const QVariantList &possibleValues)
{
  SettingsSection* section = getSection(sectionID);
  if (!section)
  {
    qCritical() << "Section" << sectionID << "is unknown";
    return;
  }
  section->updatePossibleValues(key, possibleValues);
  qDebug() << "Updated possible values for:" << key << "to" << possibleValues;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QVariant SettingsComponent::allValues(const QString& section)
{
  if (section.isEmpty())
  {
    QVariantMap all;
    for(const QString& sname : m_sections.keys())
      all[sname] = m_sections[sname]->allValues();
    return all;
  }
  else if (m_sections.contains(section))
  {
    return m_sections[section]->allValues();
  }
  else
  {
    // look for the value in the webclient section
    return m_sections[SETTINGS_SECTION_WEBCLIENT]->value(section);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QVariant SettingsComponent::orderedSections()
{
  QJsonArray desc;

  for(SettingsSection* section : m_sections.values())
  {
    if (!section->isHidden())
      desc.push_back(QJsonValue::fromVariant(section->sectionOrder()));
  }

  return desc.toVariantList();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static void writeFile(const QString& filename, const QByteArray& data)
{
  QSaveFile file(filename);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
  {
    qCritical() << "Could not open" << filename << "for writing";
    return;
  }
  file.write(data);
  if (!file.commit())
  {
    qCritical() << "Could not write" << filename;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static QJsonObject loadJson(const QString& filename)
{
  // Checking existence before opening is technically a race condition, but
  // it looks like Qt doesn't let us distinguish errors on opening.
  if (!QFile(filename).exists())
    return QJsonObject();

  QJsonParseError err;
  QJsonDocument json = Utils::OpenJsonDocument(filename, &err);
  if (json.isNull())
  {
    qCritical() << "Could not open" << filename << "due to" << err.errorString();
  }
  return json.object();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static void writeJson(const QString& filename, const QJsonObject& data, bool pretty = true)
{
  QJsonDocument json(data);
  writeFile(filename, json.toJson(pretty ? QJsonDocument::Indented : QJsonDocument::Compact));
}

/////////////////////////////////////////////////////////////////////////////////////////
QVariant SettingsComponent::readPreinitValue(const QString& sectionID, const QString& key)
{
  QString path = ProfileManager::activeProfile().dataDir("jellyfin-desktop.conf");
  if (path.isEmpty())
    return QVariant();
  QJsonObject json = loadJson(path);
  return json["sections"].toObject()[sectionID].toObject()[key].toVariant();
}

/////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::load()
{
  QString profileDir = ProfileManager::activeProfile().dataDir();
  if (profileDir.isEmpty())
  {
    qInfo() << "No active profile, skipping settings load (using defaults)";
    return;
  }

  loadConf(ProfileManager::activeProfile().dataDir("jellyfin-desktop.conf"), false);
  loadConf(ProfileManager::activeProfile().dataDir("storage.json"), true);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::loadConf(const QString& path, bool storage)
{
  QJsonObject json = loadJson(path);

  int version = json["version"].toInt(0);

  if (version != m_settingsVersion)
  {
    // version 0 means file doesn't exist or is empty - skip backup/warning
    if (version != 0)
    {
      QString backup = path + ".broken";
      QFile::remove(backup);
      QFile::rename(path, backup);
      qCritical() << "Config version is" << version << "but" << m_settingsVersion << "expected. Moving old config to" << backup;
    }
    // Overwrite/create it with the defaults.
    if (storage)
      saveStorage();
    else
      saveSettings();
    return;
  }

  QJsonObject jsonSections = json["sections"].toObject();

  for(const QString& section : jsonSections.keys())
  {
    QJsonObject jsonSection = jsonSections[section].toObject();

    SettingsSection* sec = getSection(section);
    if (!sec && storage)
    {
      sec = new SettingsSection(section, PLATFORM_ANY, -1, this);
      sec->setHidden(true);
      sec->setStorage(true);
      m_sections.insert(section, sec);
    }
    else if (!sec)
    {
      qCritical() << "Trying to load section:" << section << "from config file, but we don't want that.";
      continue;
    }

    for(const QString& setting : jsonSection.keys())
      sec->setValue(setting, jsonSection.value(setting).toVariant());
  }

#ifdef DISABLE_UPDATE_CHECK
    getSection(SETTINGS_SECTION_MAIN)->setValueHidden("checkForUpdates", "true");
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::saveSettings()
{
  QString path = ProfileManager::activeProfile().dataDir("jellyfin-desktop.conf");
  if (path.isEmpty())
  {
    qWarning() << "No active profile, cannot save settings";
    return;
  }

  QVariantMap sections;

  for(SettingsSection* section : m_sections.values())
  {
    if (!section->isStorage())
      sections.insert(section->sectionName(), section->allValues());
  }

  QJsonObject json;
  json.insert("sections", QJsonValue::fromVariant(sections));
  json.insert("version", m_settingsVersion);
  writeJson(path, json);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::saveStorage()
{
  QString path = ProfileManager::activeProfile().dataDir("storage.json");
  if (path.isEmpty())
  {
    qWarning() << "No active profile, cannot save storage";
    return;
  }

  QVariantMap storage;

  for(SettingsSection* section : m_sections.values())
  {
    if (section->isStorage())
      storage.insert(section->sectionName(), section->allValues());
  }

  QJsonObject storagejson;
  storagejson.insert("sections", QJsonValue::fromVariant(storage));
  storagejson.insert("version", m_settingsVersion);
  writeJson(path, storagejson, false);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::saveSection(SettingsSection* section)
{
  if (section && section->isStorage())
    saveStorage();
  else
    saveSettings();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QVariant SettingsComponent::value(const QString& sectionID, const QString &key)
{
  SettingsSection* section = getSection(sectionID);
  if (!section)
  {
    qCritical() << "Section" << sectionID << "is unknown";
    return QVariant();
  }
  return section->value(key);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::setValue(const QString& sectionID, const QString &key, const QVariant &value)
{
  SettingsSection* section = getSection(sectionID);
  if (!section)
  {
    qCritical() << "Section" << sectionID << "is unknown";
    return;
  }
  section->setValue(key, value);
  saveSection(section);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::setValues(const QVariantMap& options)
{
  Q_ASSERT(options.contains("key"));
  Q_ASSERT(options.contains("value"));

  QString key = options["key"].toString();
  QVariant values = options["value"];

  if (values.typeId() == QMetaType::QVariantMap || values.isNull())
  {
    SettingsSection* section = getSection(key);
    if (!section)
    {
      // let's create this section since it's most likely created by the webclient
      section = new SettingsSection(key, PLATFORM_ANY, -1, this);
      section->setHidden(true);
      section->setStorage(true);
      m_sections.insert(key, section);
    }

    if (values.isNull())
      section->resetValues();
    else
      section->setValues(values);

    saveSection(section);
  }
  else if (values.typeId() == QMetaType::QString)
  {
    setValue(SETTINGS_SECTION_WEBCLIENT, key, values);
  }
  else
  {
    qWarning() << "the values sent was not a map, string or empty value. it will be ignored";

    // return so we don't call save()
    return;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::removeValue(const QString &sectionOrKey)
{
  SettingsSection* section = getSection(sectionOrKey);

  if (section)
  {
    // we want to remove a full section
    // dont remove the section, but remove all keys
    section->resetValues();
    saveSection(section);
  }
  else
  {
    // we want to remove a root key from webclient
    // which is stored in webclient section
    section = m_sections[SETTINGS_SECTION_WEBCLIENT];
    section->resetValue(sectionOrKey);
    saveSection(section);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::resetToDefault(const QString &sectionID)
{
  SettingsSection* section = getSection(sectionID);

  if (section)
  {
    section->resetValues();
    saveSection(section);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::resetToDefaultAll()
{
  for(SettingsSection *section : m_sections)
  {
    section->resetValues();
    saveSection(section);
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
struct SectionOrderIndex
{
  inline bool operator ()(SettingsSection* a, SettingsSection* b)
  {
    return a->orderIndex() < b->orderIndex();
  }
};

///////////////////////////////////////////////////////////////////////////////////////////////////
QVariantList SettingsComponent::settingDescriptions()
{
  QJsonArray desc;

  QList<SettingsSection*> sectionList = m_sections.values();
  std::sort(sectionList.begin(), sectionList.end(), SectionOrderIndex());

  for(SettingsSection* section : sectionList)
  {
    if (!section->isHidden())
      desc.push_back(QJsonValue::fromVariant(section->descriptions()));
  }

  return desc.toVariantList();
}

/////////////////////////////////////////////////////////////////////////////////////////
bool SettingsComponent::loadDescription()
{
  QJsonParseError err;
  auto doc = Utils::OpenJsonDocument(":/settings/settings_description.json", &err);
  if (doc.isNull())
  {
    qCritical() << "Failed to read settings description:" << err.errorString();
    throw FatalException("Failed to read settings description!");
  }

  if (!doc.isArray())
  {
    qCritical() << "The object needs to be an array";
    return false;
  }

  for(auto val : doc.array())
  {
    if (!val.isObject())
    {
      qCritical() << "Hoped to find sections in the root array, but they where not JSON objects";
      return false;
    }

    QJsonObject section = val.toObject();
    if (!section.contains("section"))
    {
      qCritical() << "A section needs to contain the section keyword.";
      return false;
    }

    if (section["section"] == "__meta__")
      m_settingsVersion = section.value("version").toInt();
    else
      parseSection(section);
  }

  return true;
}

/////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::parseSection(const QJsonObject& sectionObject)
{
  QString sectionName = sectionObject.value("section").toString();
  if (!sectionObject.contains("values") || !sectionObject.value("values").isArray())
  {
    qCritical() << "section object:" << sectionName << "did not contain a values array";
    return;
  }

  int platformMask = platformMaskFromObject(sectionObject);
  int sectionOrder = sectionObject.value("order").toInt(-1);

  auto section = new SettingsSection(sectionName, static_cast<quint8>(platformMask), sectionOrder, this);
  section->setHidden(sectionObject.value("hidden").toBool(false));
  section->setStorage(sectionObject.value("storage").toBool(false));

  auto values = sectionObject.value("values").toArray();
  int order = 0;
  for(auto val : values)
  {
    if (!val.isObject())
      continue;

    QJsonObject valobj = val.toObject();
    if (!valobj.contains("value") || !valobj.contains("default") || valobj.value("value").isNull())
      continue;

    QJsonValue defaults = valobj.value("default");
    QVariant defaultval = defaults.toVariant();
    if (defaults.isArray())
    {
      defaultval = QVariant();
      // Whichever default matches the current platform first is used.
      for(auto v : defaults.toArray())
      {
        auto vobj = v.toObject();
        int defPlatformMask = platformMaskFromObject(vobj);
        if ((defPlatformMask & Utils::CurrentPlatform()) == Utils::CurrentPlatform())
        {
          defaultval = vobj.value("value").toVariant();
          break;
        }
      }
    }

    int vPlatformMask = platformMaskFromObject(valobj);
    SettingsValue* setting = new SettingsValue(valobj.value("value").toString(), defaultval, static_cast<quint8>(vPlatformMask), this);

    if (valobj.contains("display_name"))
      setting->setDisplayName(valobj.value("display_name").toString());

    if (valobj.contains("help"))
      setting->setHelp(valobj.value("help").toString());

    setting->setHasDescription(true);
    setting->setHidden(valobj.value("hidden").toBool(false));
    setting->setIndexOrder(order ++);

    if (valobj.contains("input_type"))
      setting->setInputType(valobj.value("input_type").toString());

    if (valobj.contains("possible_values") && valobj.value("possible_values").isArray())
    {
      auto list = valobj.value("possible_values").toArray();
      for(auto v : list)
      {
        int platform = PLATFORM_ANY;

        auto vl = v.toArray();
        if (vl.size() < 2)
          continue;

        if (vl.size() == 3 && vl.at(2).isObject())
        {
          // platform options
          QJsonObject options = vl.at(2).toObject();
          platform = platformMaskFromObject(options);
        }

        if ((platform & Utils::CurrentPlatform()) == Utils::CurrentPlatform())
          setting->addPossibleValue(vl.at(1).toString(), vl.at(0).toVariant());
      }
    }

    section->registerSetting(setting);
  }

  m_sections.insert(sectionName, section);
}

/////////////////////////////////////////////////////////////////////////////////////////
int SettingsComponent::platformMaskFromObject(const QJsonObject& object)
{
  int platformMask = PLATFORM_ANY;

  // only include the listed platforms
  if (object.contains("platforms"))
  {
    // start by resetting it to no platforms
    platformMask = PLATFORM_UNKNOWN;

    QJsonValue platforms = object.value("platforms");

    // platforms can be both array or a single string
    if (platforms.isArray())
    {
      for(auto pl : platforms.toArray())
      {
        if (!pl.isString())
          continue;

        platformMask |= platformFromString(pl.toString());
      }
    }
    else
    {
      platformMask = platformFromString(platforms.toString());
    }
  }
  else if (object.contains("platforms_excluded"))
  {
    QJsonValue val = object.value("platforms_excluded");
    if (val.isArray())
    {
      for(auto pl : val.toArray())
      {
        if (!pl.isString())
          continue;

        platformMask &= ~platformFromString(pl.toString());
      }
    }
    else
    {
      platformMask &= ~platformFromString(val.toString());
    }
  }

  return platformMask;
}

/////////////////////////////////////////////////////////////////////////////////////////
Platform SettingsComponent::platformFromString(const QString& platformString)
{
  if (platformString == "osx")
    return PLATFORM_OSX;
  else if (platformString == "windows")
    return PLATFORM_WINDOWS;
  else if (platformString == "linux")
    return PLATFORM_LINUX;
  else if (platformString == "freebsd")
    return PLATFORM_FREEBSD;
  else if (platformString == "oe")
    return PLATFORM_OE;
  else if (platformString == "oe_rpi")
    return PLATFORM_OE_RPI;
  else if (platformString == "oe_x86")
    return PLATFORM_OE_X86;
  else if (platformString == "any")
    return PLATFORM_ANY;

  return PLATFORM_UNKNOWN;
}

/////////////////////////////////////////////////////////////////////////////////////////
bool SettingsComponent::componentInitialize()
{
  if (!loadDescription())
    return false;

  load();

  // add our AudioSettingsController that will inspect audio settings and react.
  // then run the signal the first time to make sure that we set the proper visibility
  // on the items from the start.
  //
  auto  ctrl = new AudioSettingsController(this);
  QVariantMap val;
  val.insert("devicetype", value(SETTINGS_SECTION_AUDIO, "devicetype"));
  ctrl->valuesUpdated(val);
  connect(ctrl, &AudioSettingsController::settingsUpdated, this, &SettingsComponent::groupUpdate);

  return true;
}


/////////////////////////////////////////////////////////////////////////////////////////
bool SettingsComponent::resetAndSaveOldConfiguration()
{
  QString path = ProfileManager::activeProfile().dataDir("jellyfin-desktop.conf");
  if (path.isEmpty())
    return false;
  QFile settingsFile(path);
  return settingsFile.rename(ProfileManager::activeProfile().dataDir("jellyfin-desktop.conf.old"));
}

/////////////////////////////////////////////////////////////////////////////////////////
QString SettingsComponent::getWebClientUrl(bool desktop)
{
  (void)desktop;
  QString url;

  url = SettingsComponent::Get().value(SETTINGS_SECTION_PATH, "startupurl_desktop").toString();

  if (url == "bundled")
  {
    // Use qrc:// scheme for bundled web client
    url = "qrc:///web-client/extension/find-webclient.html";
  }

  qDebug() << "Using web-client URL: " << url;

  return url;
}

/////////////////////////////////////////////////////////////////////////////////////////
QString SettingsComponent::getExtensionPath()
{
  QString url;

  url = SettingsComponent::Get().value(SETTINGS_SECTION_PATH, "startupurl_extension").toString();

  if (url == "bundled")
  {
    auto path = Paths::webExtensionPath("extension");
    url = path;
  }

  return url;
}

/////////////////////////////////////////////////////////////////////////////////////////
QString SettingsComponent::getClientName()
{
  QString name;
  name = SettingsComponent::Get().value(SETTINGS_SECTION_SYSTEM, "systemname").toString();

  if (name.compare("JellyfinDesktop") == 0) {
    name = Utils::ComputerName();
  }

  return name;
}

/////////////////////////////////////////////////////////////////////////////////////////
bool SettingsComponent::ignoreSSLErrors()
{
  return m_cliIgnoreSSLErrors || SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "ignoreSSLErrors").toBool();
}

/////////////////////////////////////////////////////////////////////////////////////////
bool SettingsComponent::autodetectCertBundle()
{
  return SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "autodetectCertBundle").toBool();
}

/////////////////////////////////////////////////////////////////////////////////////////
bool SettingsComponent::allowBrowserZoom()
{
  return SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "allowBrowserZoom").toBool();
}

/////////////////////////////////////////////////////////////////////////////////////////
bool SettingsComponent::enableWindowsTrayIcon()
{
  return SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "enableWindowsTrayIcon").toBool();
}

/////////////////////////////////////////////////////////////////////////////////////////
QString SettingsComponent::detectCertBundlePath()
{
  static QString cachedPath;
  static bool cached = false;

  if (cached) {
    return cachedPath;
  }

#if !defined(Q_OS_WIN) && !defined(Q_OS_MAC)
  QList<QString> certPaths;
  certPaths << "/etc/ssl/certs/ca-certificates.crt"
            << "/etc/pki/tls/certs/ca-bundle.crt"
            << "/usr/share/ssl/certs/ca-bundle.crt"
            << "/usr/local/share/certs/ca-root-nss.crt"
            << "/etc/ssl/cert.pem"
            << "/usr/share/curl/curl-ca-bundle.crt"
            << "/usr/local/share/curl/curl-ca-bundle.crt"
            << "/var/lib/ca-certificates/ca-bundle.pem";

  for (const auto& path : certPaths)
  {
    if (QFile::exists(path) && QFileInfo(path).isReadable()) {
      cachedPath = path;
      cached = true;
      return cachedPath;
    }
  }
#endif

  cached = true;
  return cachedPath; // Returns empty QString if not found
}

/////////////////////////////////////////////////////////////////////////////////////////
void SettingsComponent::setCommandLineValues(const QStringList& values)
{
  qDebug() << values;

  for (const QString& value : values)
  {
    if (value == "fullscreen")
      setValue(SETTINGS_SECTION_MAIN, "fullscreen", true);
    else if (value == "windowed")
      setValue(SETTINGS_SECTION_MAIN, "fullscreen", false);
    else if (value == "desktop")
      setValue(SETTINGS_SECTION_MAIN, "layout", "desktop");
    else if (value == "tv")
      setValue(SETTINGS_SECTION_MAIN, "layout", "tv");
    else if (value == "ignore-certificate-errors")
      m_cliIgnoreSSLErrors = true;
  }
}

