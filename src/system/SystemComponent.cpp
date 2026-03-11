#include <QSysInfo>
#include <QProcess>
#include <QMap>
#include <QtNetwork/qnetworkinterface.h>
#include <QGuiApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QSslCertificate>
#include <QSslError>
#include <QDebug>
#include <QRegularExpression>
#include <QPointer>
#include <functional>

#include <QtWebEngineCore/qtwebenginecoreglobal.h>

#include "input/InputComponent.h"
#include "SystemComponent.h"
#include "Version.h"
#include "settings/SettingsComponent.h"
#include "settings/SettingsSection.h"
#include "Paths.h"
#include "core/ProfileManager.h"
#include "Names.h"
#include "utils/Utils.h"
#include "utils/Log.h"


#define KONVERGO_PRODUCTID_DEFAULT  3
#define KONVERGO_PRODUCTID_OPENELEC 4

// Platform types map
QMap<SystemComponent::PlatformType, QString> g_platformTypeNames = { \
  { SystemComponent::platformTypeOsx, "macosx" }, \
  { SystemComponent::platformTypeWindows, "windows" },
  { SystemComponent::platformTypeLinux, "linux" },
  { SystemComponent::platformTypeOpenELEC, "openelec" },
  { SystemComponent::platformTypeFreeBSD, "freebsd" },
  { SystemComponent::platformTypeUnknown, "unknown" },
};

// platform Archictecture map
QMap<SystemComponent::PlatformArch, QString> g_platformArchNames = {
  { SystemComponent::platformArchX86_32, "i386" },
  { SystemComponent::platformArchX86_64, "x86_64" },
  { SystemComponent::platformArchRpi2, "rpi2" },
  { SystemComponent::platformArchUnknown, "unknown" }
};


///////////////////////////////////////////////////////////////////////////////////////////////////
SystemComponent::SystemComponent(QObject* parent) : ComponentBase(parent), m_platformType(platformTypeUnknown), m_platformArch(platformArchUnknown), m_doLogMessages(false), m_scale(1), m_connectivityCheckReply(nullptr), m_resolveUrlReply(nullptr)
{
  m_connectivityRetryTimer = new QTimer(this);
  m_connectivityRetryTimer->setSingleShot(true);
  connect(m_connectivityRetryTimer, &QTimer::timeout, [this]() {
    qInfo() << "Retrying connectivity check for" << m_pendingConnectivityUrl;
    checkServerConnectivity(m_pendingConnectivityUrl);
  });

  m_networkManager = new QNetworkAccessManager(this);

// define OS Type
#if defined(Q_OS_MAC)
  m_platformType = platformTypeOsx;
#elif defined(Q_OS_WIN)
  m_platformType = platformTypeWindows;
#elif defined(KONVERGO_OPENELEC)
  m_platformType = platformTypeOpenELEC;
#elif defined(Q_OS_LINUX)
  m_platformType = platformTypeLinux;
#elif defined(Q_OS_FREEBSD)
  m_platformType = platformTypeFreeBSD;
#endif

// define target type
#ifdef TARGET_RPI
  m_platformArch = platformArchRpi2;
#elif defined(Q_PROCESSOR_X86_32)
  m_platformArch = platformArchX86_32;
#elif defined(Q_PROCESSOR_X86_64)
  m_platformArch = platformArchX86_64;
#endif

  if (auto* s = SettingsComponent::Get().getSection(SETTINGS_SECTION_AUDIO))
  {
    connect(s, &SettingsSection::valuesUpdated, [=]()
    {
      emit capabilitiesChanged(getCapabilitiesString());
    });
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
bool SystemComponent::componentInitialize()
{
  QDir().mkpath(ProfileManager::activeProfile().dataDir("scripts"));
  QDir().mkpath(ProfileManager::activeProfile().dataDir("sounds"));

  return true;
}

/////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::crashApp()
{
  *static_cast<volatile int*>(nullptr) = 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::componentPostInitialize()
{
  InputComponent::Get().registerHostCommand("crash!", this, "crashApp");
  InputComponent::Get().registerHostCommand("script", this, "runUserScript");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QString SystemComponent::getPlatformTypeString() const
{
  return g_platformTypeNames[m_platformType];
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QString SystemComponent::getPlatformArchString() const
{
  return g_platformArchNames[m_platformArch];
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QVariantMap SystemComponent::systemInformation() const
{
  QVariantMap info;
  QString build;
  QString dist;
  QString arch;
  int productid = KONVERGO_PRODUCTID_DEFAULT;

#ifdef Q_OS_WIN
  arch = (sizeof(void *) == 8) ? "x86_64" : "i386";
#else
  arch = QSysInfo::currentCpuArchitecture();
#endif

  build = getPlatformTypeString();
  dist = getPlatformTypeString();

#if defined(KONVERGO_OPENELEC)
  productid = KONVERGO_PRODUCTID_OPENELEC;
  dist = "openelec";

  if (m_platformArch == platformArchRpi2)
  {
    build = "rpi2";
  }
  else
  {
    build = "generic";
  }
#endif

  
  info["build"] = build + "-" + arch;
  info["dist"] = dist;
  info["version"] = Version::GetVersionString();
  info["productid"] = productid;
  
 qDebug() << QString(
                "System Information : build(%1)-arch(%2).dist(%3).version(%4).productid(%5)")
                .arg(build)
                .arg(arch)
                .arg(dist)
                .arg(Version::GetVersionString())
                .arg(productid);
 return info;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::exit()
{
  qApp->quit();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::restart()
{
  qApp->quit();
  QProcess::startDetached(qApp->arguments()[0], qApp->arguments());
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::jsLog(int level, QString text)
{
  switch (level) {
    case 0: // Info
      qInfo() << "JS:" << qPrintable(text);
      break;
    case 1: // Warning
      qWarning() << "JS:" << qPrintable(text);
      break;
    case 2: // Error
      qCritical() << "JS:" << qPrintable(text);
      break;
    default:
      qDebug() << "JS: [uncaught level=" << level << "]" << qPrintable(text);
      break;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QString SystemComponent::extractBaseUrl(const QString& url)
{
  QUrl parsedUrl(url);
  QString path = parsedUrl.path();

  // Find last occurrence of "/web" in path only (case-insensitive)
  qsizetype webIndex = path.toLower().lastIndexOf("/web");

  if (webIndex >= 0) {
    // Truncate path at /web
    path = path.left(webIndex);

    // Build URL without fragment/query
    QString result = parsedUrl.scheme() + "://" + parsedUrl.host();
    int port = parsedUrl.port();
    if (port != -1 &&
        !((parsedUrl.scheme() == "https" && port == 443) ||
          (parsedUrl.scheme() == "http" && port == 80))) {
      result += ":" + QString::number(port);
    }
    result += path;
    return result;
  }

  // Fallback to origin (scheme://host:port, excluding default ports and userinfo)
  QString origin = parsedUrl.scheme() + "://" + parsedUrl.host();
  int port = parsedUrl.port();
  // Only add port if non-default (443 for https, 80 for http)
  if (port != -1 &&
      !((parsedUrl.scheme() == "https" && port == 443) ||
        (parsedUrl.scheme() == "http" && port == 80))) {
    origin += ":" + QString::number(port);
  }
  return origin;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QSslConfiguration SystemComponent::getSSLConfiguration()
{
  QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
  // Require TLS 1.2 or newer
  sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
  if (SettingsComponent::Get().ignoreSSLErrors()) {
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
  } else if (SettingsComponent::Get().autodetectCertBundle()) {
    QString certPath = SettingsComponent::Get().detectCertBundlePath();
    if (!certPath.isEmpty()) {
      QList<QSslCertificate> certs = QSslCertificate::fromPath(certPath);
      if (!certs.isEmpty()) {
        sslConfig.setCaCertificates(certs);
      }
    }
  }
  return sslConfig;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::setReplyTimeout(QNetworkReply* reply, int ms)
{
  QPointer<QNetworkReply> replyPtr(reply);
  QTimer::singleShot(ms, this, [replyPtr]() {
    if (replyPtr && !replyPtr->isFinished()) {
      replyPtr->abort();
    }
  });
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::resolveUrl(const QString& url, std::function<void(const QString&)> callback)
{
  // Abort any pending resolve
  if (m_resolveUrlReply) {
    m_resolveUrlReply->abort();
    m_resolveUrlReply->deleteLater();
    m_resolveUrlReply = nullptr;
  }

  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::UserAgentHeader, getUserAgent());
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setSslConfiguration(getSSLConfiguration());

  m_resolveUrlReply = m_networkManager->head(request);

  QNetworkReply* reply = m_resolveUrlReply;
  setReplyTimeout(reply, NETWORK_REQUEST_TIMEOUT_MS);

  if (SettingsComponent::Get().ignoreSSLErrors()) {
    connect(reply, QOverload<const QList<QSslError>&>::of(&QNetworkReply::sslErrors),
            reply, QOverload<>::of(&QNetworkReply::ignoreSslErrors));
  }
  connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
    if (reply->error() == QNetworkReply::OperationCanceledError) {
      reply->deleteLater();
      if (m_resolveUrlReply == reply) {
        m_resolveUrlReply = nullptr;
      }
      return;
    }

    if (reply->error() != QNetworkReply::NoError) {
      qWarning() << "resolveUrl: error:" << reply->errorString();
    }

    callback(reply->url().toString());
    reply->deleteLater();
    if (m_resolveUrlReply == reply) {
      m_resolveUrlReply = nullptr;
    }
  });
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::checkServerConnectivity(QString url)
{
  // Stop any pending retry
  if (m_connectivityRetryTimer->isActive()) {
    m_connectivityRetryTimer->stop();
  }

  if (m_connectivityCheckReply) {
    m_connectivityCheckReply->abort();
    m_connectivityCheckReply->deleteLater();
    m_connectivityCheckReply = nullptr;
  }

  // Store for retry
  m_pendingConnectivityUrl = url;

  resolveUrl(url, [this, url](const QString& fullResolvedUrl) {
    QString baseUrl = extractBaseUrl(fullResolvedUrl);

    QString checkUrl = baseUrl + "/System/Info/Public";

    QNetworkRequest request(checkUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, getUserAgent());
    request.setRawHeader("Cache-Control", "no-cache");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setSslConfiguration(getSSLConfiguration());

    m_connectivityCheckReply = m_networkManager->get(request);

    QNetworkReply* reply = m_connectivityCheckReply;
    setReplyTimeout(reply, NETWORK_REQUEST_TIMEOUT_MS);

    if (SettingsComponent::Get().ignoreSSLErrors()) {
      connect(reply, QOverload<const QList<QSslError>&>::of(&QNetworkReply::sslErrors),
              reply, QOverload<>::of(&QNetworkReply::ignoreSslErrors));
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, url, fullResolvedUrl]() {
      if (reply->error() == QNetworkReply::OperationCanceledError) {
        reply->deleteLater();
        if (m_connectivityCheckReply == reply) {
          m_connectivityCheckReply = nullptr;
        }
        return;
      }

      bool success = false;
      if (reply->error() == QNetworkReply::NoError) {
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject() && doc.object().contains("Id") && !doc.object()["Id"].toString().isEmpty()) {
          success = true;
        } else {
          qWarning() << "checkServerConnectivity: invalid response";
        }
      } else {
        qWarning() << "checkServerConnectivity: error:" << reply->errorString();
      }

      if (success) {
        m_connectivityRetryTimer->stop();
        m_pendingConnectivityUrl.clear();
        emit serverConnectivityResult(url, success, fullResolvedUrl);
      } else {
        m_connectivityRetryTimer->start(CONNECTIVITY_RETRY_INTERVAL_MS);
      }

      reply->deleteLater();
      if (m_connectivityCheckReply == reply) {
        m_connectivityCheckReply = nullptr;
      }
    });
  });
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::cancelServerConnectivity()
{
  if (m_connectivityRetryTimer->isActive()) {
    m_connectivityRetryTimer->stop();
  }

  // Save pointers before abort() since it synchronously triggers finished handlers
  // which may set member variables to nullptr
  if (m_resolveUrlReply) {
    QNetworkReply* reply = m_resolveUrlReply;
    m_resolveUrlReply = nullptr;
    reply->abort();
    reply->deleteLater();
  }

  if (m_connectivityCheckReply) {
    QNetworkReply* reply = m_connectivityCheckReply;
    m_connectivityCheckReply = nullptr;
    reply->abort();
    reply->deleteLater();
  }

  m_pendingConnectivityUrl.clear();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
QString SystemComponent::getUserAgent()
{
  QString kernel = QSysInfo::kernelType();
  kernel[0] = kernel[0].toUpper();
  QString chromeVersion = QString(qWebEngineChromiumVersion()).split('.').first() + ".0.0.0";
  QString userAgent = QString("JellyfinDesktop/%1 (%2; %3) Chrome/%4")
    .arg(Version::GetVersionString())
    .arg(kernel)
    .arg(getPlatformArchString())
    .arg(chromeVersion);
  return userAgent;
}

/////////////////////////////////////////////////////////////////////////////////////////
QString SystemComponent::debugInformation()
{
  QString debugInfo;
  QTextStream stream(&debugInfo);

  stream << "Jellyfin\n";
  stream << "  Version: " << Version::GetVersionString() << " built: " << Version::GetBuildDate() << "\n";
  stream << "  Web Client Version: " << Version::GetWebVersion() << "\n";
  stream << "  Web Client URL: " << SettingsComponent::Get().value(SETTINGS_SECTION_PATH, "startupurl").toString() << "\n";
  stream << "  Platform: " << getPlatformTypeString() << "-" << getPlatformArchString() << "\n";
  stream << "  User-Agent: " << getUserAgent() << "\n";
  stream << "  Qt version: " << qVersion() << QString("(%1)").arg(Version::GetQtDepsVersion()) << "\n";
  stream << "  Depends version: " << Version::GetDependenciesVersion() << "\n";
  stream << "\n";

  stream << "Files\n";
  stream << "  Log file: " << ProfileManager::activeProfile().logDir() + "/" + Names::DataName() + ".log" << "\n";
  stream << "  Config file: " << ProfileManager::activeProfile().dataDir(Names::DataName() + ".conf") << "\n";
  stream << "\n";

  stream << "Network Addresses\n";
  for(const QString& addr : networkAddresses())
  {
    stream << "  " << addr << "\n";
  }
  stream << "\n";

  stream.flush();
  return debugInfo;
}

/////////////////////////////////////////////////////////////////////////////////////////
QStringList SystemComponent::networkAddresses() const
{
  QStringList list;
  for(const QHostAddress& address : QNetworkInterface::allAddresses())
  {
    if (! address.isLoopback() && (address.protocol() == QAbstractSocket::IPv4Protocol ||
                                   address.protocol() == QAbstractSocket::IPv6Protocol))
    {
      auto s = address.toString();
      if (!s.startsWith("fe80::"))
        list << s;
    }
  }

  return list;
}

/////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::openExternalUrl(const QString& url)
{
  QDesktopServices::openUrl(QUrl(url));
}

/////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::runUserScript(QString script)
{
  // We take the path the user supplied and run it through fileInfo and
  // look for the fileName() part, this is to avoid people sharing keymaps
  // that tries to execute things like ../../ etc. Note that this function
  // is still not safe, people can do nasty things with it, so users needs
  // to be careful with their keymaps.
  //
  QFileInfo fi(script);
  QString scriptPath = ProfileManager::activeProfile().dataDir("scripts/" + fi.fileName());

  QFile scriptFile(scriptPath);
  if (scriptFile.exists())
  {
    if (!QFileInfo(scriptFile).isExecutable())
    {
      qWarning() << "Script:" << script << "is not executable";
      return;
    }

    qInfo() << "Running script:" << scriptPath;

    if (QProcess::startDetached(scriptPath, QStringList()))
      qDebug() << "Script started successfully";
    else
      qWarning() << "Error running script:" << scriptPath;
  }
  else
  {
    qWarning() << "Could not find script:" << scriptPath;
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::hello(const QString& version)
{
  qDebug() << QString("Web-client (%1) fully inited.").arg(version);
  m_webClientVersion = version;
}

/////////////////////////////////////////////////////////////////////////////////////////
QString SystemComponent::getNativeShellScript()
{
  static QString cachedScript;
  if (!cachedScript.isEmpty()) {
    return cachedScript;
  }

  auto path = SettingsComponent::Get().getExtensionPath();
  qDebug() << QString("Using path for extension: %1").arg(path);

  QJsonObject clientData;
  clientData.insert("deviceName", QJsonValue::fromVariant(SettingsComponent::Get().getClientName()));
  clientData.insert("version", QJsonValue::fromVariant(Version::GetVersionString()));
  clientData.insert("userAgent", QJsonValue::fromVariant(getUserAgent()));
  clientData.insert("scriptPath", QJsonValue::fromVariant("file:///" + path));
  QString defaultMode = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "layout").toString();

  QFile flatpakOsFile {"/run/host/os-release"};
  if (flatpakOsFile.exists()) {
    qDebug() << "Found flatpak os-release file";
    if (flatpakOsFile.open(QIODevice::ReadOnly)) {
      QString flatpakOsFileString = QTextStream(&flatpakOsFile).readAll();
      if (flatpakOsFileString.contains("NAME=\"SteamOS\"")) {
        qDebug() << "Detected SteamOS";
        defaultMode = "tv";
      }
    }
  }
  clientData.insert("mode", QJsonValue::fromVariant(defaultMode));

  QVariantList settingsDescriptionsList = SettingsComponent::Get().settingDescriptions();
  QVariantMap settingsDescriptions = QVariantMap();
  for (auto setting : settingsDescriptionsList) {
    QVariantMap settingMap = setting.toMap();
    settingsDescriptions.insert(settingMap["key"].toString(), settingMap["settings"]);
  }
  clientData.insert("sections", QJsonValue::fromVariant(SettingsComponent::Get().orderedSections()));
  clientData.insert("settingsDescriptions", QJsonValue::fromVariant(settingsDescriptions));
  clientData.insert("settings", QJsonValue::fromVariant(SettingsComponent::Get().allValues()));

  QString jmpInfoDeclaration = "const jmpInfo = JSON.parse(window.atob(\"" +
                                QJsonDocument(clientData).toJson(QJsonDocument::Compact).toBase64() +
                                "\"));\nwindow.jmpInfo = jmpInfo;\n";

  auto loadScript = [](const QString& scriptPath) -> QString {
    QFile file(scriptPath);
    if (!file.open(QIODevice::ReadOnly)) {
      qCritical() << "Failed to load" << scriptPath << "from qrc";
      return "";
    }
    return QTextStream(&file).readAll();
  };

  QStringList scriptPaths = {
    ":/qtwebchannel/qwebchannel.js",
    ":/web-client/extension/mpvVideoPlayer.js",
    ":/web-client/extension/mpvAudioPlayer.js",
    ":/web-client/extension/inputPlugin.js",
    ":/web-client/extension/updatePlugin.js",
    ":/web-client/extension/connectivityHelper.js",
    ":/web-client/extension/nativeshell.js"
  };

  cachedScript = jmpInfoDeclaration;
  for (const QString& scriptPath : scriptPaths) {
    cachedScript += loadScript(scriptPath) + "\n";
  }

  return cachedScript;
}

/////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::fetchPageForCSPWorkaround(QString url)
{
  qDebug() << "fetchPageForCSPWorkaround:" << url;

  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::UserAgentHeader, getUserAgent());
  request.setRawHeader("Cache-Control", "no-cache");

  // Configure SSL settings
  QSslConfiguration sslConfig = request.sslConfiguration();
  if (SettingsComponent::Get().ignoreSSLErrors()) {
    qDebug() << "fetchPageForCSPWorkaround: ignoring SSL errors";
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
  }
  request.setSslConfiguration(sslConfig);

  QNetworkReply* reply = m_networkManager->get(request);

  // Handle SSL errors if ignoreSSLErrors is enabled
  if (SettingsComponent::Get().ignoreSSLErrors()) {
    connect(reply, QOverload<const QList<QSslError>&>::of(&QNetworkReply::sslErrors),
            this, [reply](const QList<QSslError>& errors) {
      qDebug() << "fetchPageForCSPWorkaround: ignoring SSL errors:" << errors;
      reply->ignoreSslErrors();
    });
  }

  connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
    if (reply->error() == QNetworkReply::NoError) {
      QByteArray rawData = reply->readAll();
      QString html = QString::fromUtf8(rawData);
      QString finalUrl = reply->url().toString();
      bool hadCSP = false;

      // Check for CSP headers
      QList<QByteArray> headerList = reply->rawHeaderList();
      for (const QByteArray& header : headerList) {
        QString headerName = QString::fromUtf8(header);
        if (headerName.toLower() == "content-security-policy" ||
            headerName.toLower() == "content-security-policy-report-only") {
          hadCSP = true;
          QString headerValue = QString::fromUtf8(reply->rawHeader(header));
          qInfo() << "CSP header detected:" << headerName << "=" << headerValue.left(100);
        }
      }

      if (hadCSP) {
        qInfo() << "CSP workaround: applying for" << url;
      } else {
        qDebug() << "No CSP detected for" << url;
      }

      emit pageContentReady(html, finalUrl, hadCSP);
    } else {
      qCritical() << "fetchPageForCSPWorkaround: fetch failed:" << reply->errorString();
    }
    reply->deleteLater();
  });
}

/////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::checkForUpdates()
{
#ifndef DISABLE_UPDATE_CHECK
  if (SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "checkForUpdates").toBool()) {
#if !defined(Q_OS_WIN) && !defined(Q_OS_MAC)
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    QString checkUrl = "https://github.com/jellyfin/jellyfin-desktop/releases/latest";
    QUrl qCheckUrl = QUrl(checkUrl);
    qDebug() << QString("Checking URL for updates: %1").arg(checkUrl);
    QNetworkRequest req(qCheckUrl);
    req.setHeader(QNetworkRequest::UserAgentHeader, getUserAgent());

    connect(manager, &QNetworkAccessManager::finished, this, &SystemComponent::updateInfoHandler);
    manager->get(req);
#else
    emit updateInfoEmitted("SSL_UNAVAILABLE");
#endif
  }
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////
void SystemComponent::updateInfoHandler(QNetworkReply* reply)
{
  if (reply->error() == QNetworkReply::NoError) {
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if(statusCode == 302) {
      QUrl redirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
      emit updateInfoEmitted(redirectUrl.toString());
    }
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
#define BASESTR "protocols=shoutcast,http-video;videoDecoders=h264{profile:high&resolution:2160&level:52};audioDecoders=mp3,aac,dts{bitrate:800000&channels:%1},ac3{bitrate:800000&channels:%2}"

/////////////////////////////////////////////////////////////////////////////////////////
QString SystemComponent::getCapabilitiesString()
{
  auto capstring = QString(BASESTR);
  auto channels = SettingsComponent::Get().value(SETTINGS_SECTION_AUDIO, "channels").toString();
  auto dtsenabled = SettingsComponent::Get().value(SETTINGS_SECTION_AUDIO, "passthrough.dts").toBool();
  auto ac3enabled = SettingsComponent::Get().value(SETTINGS_SECTION_AUDIO, "passthrough.ac3").toBool();

  // Assume that auto means that we want to select multi-channel tracks by default.
  // So really only disable it when 2.0 is selected.
  //
  int ac3channels = 2;
  int dtschannels = 2;

  if (channels != "2.0")
    dtschannels = ac3channels = 8;
  else if (dtsenabled)
    dtschannels = 8;
  else if (ac3enabled)
    ac3channels = 8;

  return capstring.arg(dtschannels).arg(ac3channels);
}
