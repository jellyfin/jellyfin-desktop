#include <QGuiApplication>
#include <QApplication>
#include <memory>
#include <QDateTime>
#include <QFileInfo>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QtQml>
#include <Qt>
#include <QtWebEngineQuick>
#ifdef USE_WAYLAND_SUBSURFACE
#include <QVulkanInstance>
#endif
#include <qtwebenginecoreglobal.h>
#include <QtWebEngineCore/QWebEngineProfile>
#include <QErrorMessage>
#include <QtWebEngineCore/QWebEngineScript>
#include <QCommandLineOption>
#include <QDebug>
#include <QRegularExpression>
#include <QSettings>

#include "shared/Names.h"
#include "system/SystemComponent.h"
#include <QDebug>
#include "Paths.h"
#include "player/CodecsComponent.h"
#include "player/PlayerComponent.h"
#include "player/OpenGLDetect.h"
#include "display/DisplayComponent.h"
#include "Version.h"
#include "settings/SettingsComponent.h"
#include "settings/SettingsSection.h"
#include "ui/EventFilter.h"
#include "ui/WindowManager.h"
#include "Globals.h"
#include "ui/ErrorMessage.h"
#include "UniqueApplication.h"
#include "utils/Log.h"

#ifdef Q_OS_MAC
#include "PFMoveApplication.h"
#endif

#if defined(Q_OS_MAC) || defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
#include "SignalManager.h"
#endif

/////////////////////////////////////////////////////////////////////////////////////////
static void preinitQt()
{
  QCoreApplication::setApplicationName(Names::MainName());
  QCoreApplication::setApplicationVersion(Version::GetVersionString());
  QCoreApplication::setOrganizationDomain("jellyfin.org");

#ifdef Q_OS_WIN32
  // Qt 6 uses desktop OpenGL by default on Windows
  // AA_UseOpenGLES and AA_UseDesktopOpenGL are deprecated and ignored
  // No need to set any attributes
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////
char** appendCommandLineArguments(int argc, char **argv, const QStringList& args)
{
  size_t newSize = static_cast<size_t>(argc + args.length() + 1) * sizeof(char*);
  char** newArgv = static_cast<char**>(calloc(1, newSize));
  memcpy(newArgv, argv, static_cast<size_t>(argc) * sizeof(char*));

  int pos = argc;
  for(const QString& str : args)
    newArgv[pos++] = qstrdup(str.toUtf8().data());

  return newArgv;
}

/////////////////////////////////////////////////////////////////////////////////////////
void ShowLicenseInfo()
{
  QFile licenses(":/misc/licenses.txt");
  if (!licenses.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    fprintf(stderr, "Error: Could not open license file\n");
    return;
  }
  QByteArray contents = licenses.readAll();
  printf("%.*s\n", static_cast<int>(contents.size()), contents.data());
}

/////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char *argv[])
{
  try
  {
    QCommandLineParser parser;
    parser.setApplicationDescription("Jellyfin Desktop");
    parser.addVersionOption();
    parser.addOptions({{{"h", "help"},              "Show this help"},
                       {{"l", "licenses"},          "Show license information"},
                       {"desktop",                  "Start in desktop mode"},
                       {"tv",                       "Start in TV mode"},
                       {"windowed",                 "Start in windowed mode"},
                       {"fullscreen",               "Start in fullscreen"},
                       {"disable-gpu",              "Disable QtWebEngine gpu accel"},
                       {"ignore-certificate-errors", "Ignore certificate errors"}});

    auto scaleOption = QCommandLineOption("scale-factor", "Set to a integer or default auto which controls" \
                                                          "the scale (DPI) of the desktop interface.");
    scaleOption.setValueName("scale");
    scaleOption.setDefaultValue("auto");

    auto platformOption = QCommandLineOption("platform", "Equivalent to QT_QPA_PLATFORM.");
    platformOption.setValueName("platform");
    platformOption.setDefaultValue("default");

    auto devOption = QCommandLineOption("remote-debugging-port", "Port number for devtools.");
    devOption.setValueName("port");

    auto configDirOption = QCommandLineOption("config-dir", "Override config directory path.");
    configDirOption.setValueName("path");

    auto logLevelOption = QCommandLineOption("log-level", "Log level: debug, info, warn, error, fatal (default: error)");
    logLevelOption.setValueName("level");

    auto profileOption = QCommandLineOption("profile", "Use specific profile for this session.");
    profileOption.setValueName("name");

    auto setDefaultProfileOption = QCommandLineOption("set-default-profile", "Set the default profile and exit.");
    setDefaultProfileOption.setValueName("name");

    auto listProfilesOption = QCommandLineOption("list-profiles", "List all profiles and exit.");

    auto deleteProfileOption = QCommandLineOption("delete-profile", "Delete a profile and exit.");
    deleteProfileOption.setValueName("name");

    auto createProfileOption = QCommandLineOption("create-profile", "Create a new profile and exit.");
    createProfileOption.setValueName("name");

    parser.addOption(scaleOption);
    parser.addOption(devOption);
    parser.addOption(platformOption);
    parser.addOption(configDirOption);
    parser.addOption(logLevelOption);
    parser.addOption(profileOption);
    parser.addOption(setDefaultProfileOption);
    parser.addOption(listProfilesOption);
    parser.addOption(deleteProfileOption);
    parser.addOption(createProfileOption);

#ifdef USE_WAYLAND_SUBSURFACE
    // Check if we'll use Wayland Vulkan mode - needs to be early for Qt flags
    static bool isWayland = qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland" ||
                            qEnvironmentVariable("QT_QPA_PLATFORM").contains("wayland");
#endif

    // Qt flags for WebEngine - skip on Wayland Vulkan to avoid conflicts
    QStringList g_qtFlags;
#ifdef USE_WAYLAND_SUBSURFACE
    if (!isWayland) {
      g_qtFlags << "--enable-gpu-rasterization" << "--disable-features=MediaSessionService";
    }
#else
    g_qtFlags << "--enable-gpu-rasterization" << "--disable-features=MediaSessionService";
#endif

    char **newArgv = appendCommandLineArguments(argc, argv, g_qtFlags);
    int newArgc = argc + g_qtFlags.size();

    preinitQt();

#ifdef USE_WAYLAND_SUBSURFACE
    if (isWayland) {
      // Vulkan mode - no OpenGL detection needed
    } else {
      detectOpenGLEarly();
      QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
      QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    }
#else
    detectOpenGLEarly();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#endif

    QStringList arguments;
    for (int i = 0; i < argc; i++)
      arguments << QString::fromLatin1(argv[i]);

    {
      // This is kinda dumb. But in order for the QCommandLineParser
      // to work properly we need to init if before we call process
      // but we don't want to do that for the main application since
      // we need to set the scale factor before we do that. So it becomes
      // a small chicken-or-egg problem, which we "solve" by making
      // this temporary console app.
      //
      QCoreApplication core(newArgc, newArgv);

      // Now parse the command line.
      parser.process(arguments);
    }

    if (parser.isSet("help"))
    {
      // Get Qt's generated help, insert section header before profile options
      QString help = parser.helpText();
      help.replace("  --profile", "\nProfile Options:\n  --profile");
      printf("%s", qPrintable(help));
      return EXIT_SUCCESS;
    }

    if (parser.isSet("licenses"))
    {
      ShowLicenseInfo();
      return EXIT_SUCCESS;
    }

    // Handle config-dir first (affects where profiles.json is located)
    auto configDir = parser.value("config-dir");
    if (!configDir.isEmpty())
    {
      QFileInfo fi(configDir);
      QString absPath = fi.absoluteFilePath();
      QDir parentDir = fi.dir();

      if (!parentDir.exists())
      {
        fprintf(stderr, "Config directory parent does not exist: %s\n", qPrintable(parentDir.absolutePath()));
        return EXIT_FAILURE;
      }

      Paths::setConfigDir(absPath);
      QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, absPath);
    }

    // Helper lambdas for profile operations
    auto profileExists = [](const QString& id) {
      return QDir(Paths::globalDataDir("profiles/" + id)).exists();
    };

    auto getProfilesList = []() {
      QStringList profiles;
      QDir profilesDir(Paths::globalDataDir("profiles"));
      if (profilesDir.exists())
      {
        for (const QString& dir : profilesDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        {
          if (dir.length() == 32 && dir.contains(QRegularExpression("^[0-9a-f]+$")))
            profiles << dir;
        }
      }
      return profiles;
    };

    auto getProfileName = [](const QString& id) {
      QFile meta(Paths::globalDataDir("profiles/" + id + "/profile.json"));
      if (meta.open(QIODevice::ReadOnly))
      {
        QJsonDocument doc = QJsonDocument::fromJson(meta.readAll());
        meta.close();
        return doc.object()["name"].toString();
      }
      return QString();
    };

    auto findProfileByName = [&getProfilesList, &getProfileName](const QString& name) {
      for (const QString& id : getProfilesList())
      {
        if (getProfileName(id) == name)
          return id;
      }
      return QString();
    };

    auto readDefaultProfile = []() {
      QString profilesFile = Paths::globalDataDir("profiles.json");
      QFile file(profilesFile);
      if (file.open(QIODevice::ReadOnly))
      {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        return doc.object()["defaultProfile"].toString();
      }
      return QString();
    };

    auto writeDefaultProfile = [](const QString& id) {
      QString profilesFile = Paths::globalDataDir("profiles.json");
      QJsonObject root;
      root["defaultProfile"] = id;
      QFile out(profilesFile);
      if (out.open(QIODevice::WriteOnly))
      {
        out.write(QJsonDocument(root).toJson());
        out.close();
        return true;
      }
      return false;
    };

    auto createNewProfile = [](const QString& name) {
      QString id = QUuid::createUuid().toString(QUuid::Id128);
      QDir().mkpath(Paths::globalDataDir("profiles/" + id));
      QDir().mkpath(Paths::globalDataDir("profiles/" + id + "/logs"));
      // Always write profile.json with name
      QFile meta(Paths::globalDataDir("profiles/" + id + "/profile.json"));
      if (meta.open(QIODevice::WriteOnly))
      {
        QJsonObject obj;
        obj["name"] = name;
        meta.write(QJsonDocument(obj).toJson());
        meta.close();
      }
      return id;
    };

    // Handle --list-profiles
    if (parser.isSet("list-profiles"))
    {
      QStringList profiles = getProfilesList();
      QString defaultId = readDefaultProfile();
      if (profiles.isEmpty())
      {
        printf("No profiles found.\n");
      }
      else
      {
        for (const QString& id : profiles)
        {
          QString marker = (id == defaultId) ? " *" : "";
          QString name = getProfileName(id);
          if (name.isEmpty())
            printf("%s%s\n", qPrintable(id), qPrintable(marker));
          else
            printf("%s%s\n", qPrintable(name), qPrintable(marker));
        }
      }
      return EXIT_SUCCESS;
    }

    // Handle --create-profile
    if (parser.isSet("create-profile"))
    {
      QString name = parser.value("create-profile");
      if (name.isEmpty())
      {
        fprintf(stderr, "Profile name is required\n");
        return EXIT_FAILURE;
      }
      if (!findProfileByName(name).isEmpty())
      {
        fprintf(stderr, "Profile already exists: %s\n", qPrintable(name));
        return EXIT_FAILURE;
      }
      createNewProfile(name);
      printf("Created profile: %s\n", qPrintable(name));
      return EXIT_SUCCESS;
    }

    // Handle --delete-profile
    if (parser.isSet("delete-profile"))
    {
      QString name = parser.value("delete-profile");
      QString id = findProfileByName(name);
      if (id.isEmpty())
      {
        fprintf(stderr, "Profile not found: %s\n", qPrintable(name));
        return EXIT_FAILURE;
      }
      QString profilePath = Paths::globalDataDir("profiles/" + id);
      QDir dir(profilePath);
      if (!dir.removeRecursively())
      {
        fprintf(stderr, "Failed to delete profile: %s\n", qPrintable(name));
        return EXIT_FAILURE;
      }
      // Update default if we deleted it
      if (readDefaultProfile() == id)
      {
        QStringList remaining = getProfilesList();
        writeDefaultProfile(remaining.isEmpty() ? QString() : remaining.first());
      }
      printf("Deleted profile: %s\n", qPrintable(name));
      return EXIT_SUCCESS;
    }

    // Handle --set-default-profile
    if (parser.isSet("set-default-profile"))
    {
      QString name = parser.value("set-default-profile");
      QString id = findProfileByName(name);
      if (id.isEmpty())
      {
        fprintf(stderr, "Profile not found: %s\n", qPrintable(name));
        return EXIT_FAILURE;
      }
      if (!writeDefaultProfile(id))
      {
        fprintf(stderr, "Failed to write profiles.json\n");
        return EXIT_FAILURE;
      }
      printf("Default profile set to: %s\n", qPrintable(name));
      return EXIT_SUCCESS;
    }

    // Determine which profile to use for this session
    QString profileId;

    // --profile overrides everything
    if (parser.isSet("profile"))
    {
      QString name = parser.value("profile");
      profileId = findProfileByName(name);
      if (profileId.isEmpty())
      {
        fprintf(stderr, "Profile not found: %s\n", qPrintable(name));
        return EXIT_FAILURE;
      }
    }
    else
    {
      // Use default profile
      profileId = readDefaultProfile();
      bool needsWrite = false;

      // Validate default exists
      if (!profileId.isEmpty() && !profileExists(profileId))
      {
        profileId.clear();
        needsWrite = true;
      }

      // Find first profile
      if (profileId.isEmpty())
      {
        QStringList profiles = getProfilesList();
        if (!profiles.isEmpty())
        {
          profileId = profiles.first();
          needsWrite = true;
        }
      }

      // Create new profile
      if (profileId.isEmpty())
      {
        profileId = createNewProfile("Default");
        needsWrite = true;
      }

      if (needsWrite)
        writeDefaultProfile(profileId);
    }

    Paths::setActiveProfileId(profileId);

    // Now everything else - all paths are profile-specific from here on
    QString logLevel = parser.value("log-level");
    if (parser.isSet("log-level") && (logLevel.isEmpty() || Log::ParseLogLevel(logLevel) == -1))
    {
      fprintf(stderr, "Error: invalid log level '%s'. Valid levels: debug, info, warn, error, fatal\n", qPrintable(logLevel));
      return EXIT_FAILURE;
    }

    if (parser.isSet("log-level"))
      Log::SetLogLevel(logLevel);

    Log::Init();

    auto scale = parser.value("scale-factor");
    if (scale.isEmpty() || scale == "auto")
    {
#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
      QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    }
    else if (scale != "none")
    {
      qputenv("QT_SCALE_FACTOR", scale.toUtf8());
    }

    auto platform = parser.value("platform");
    if (!(platform.isEmpty() || platform == "default"))
    {
      qputenv("QT_QPA_PLATFORM", platform.toUtf8());
    }
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && !defined(Q_OS_ANDROID) && QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
    // Force xcb on Qt < 6.5 due to mpvqt wayland requiring >=6.5
    else if (platform.isEmpty() || platform == "default")
    {
      qputenv("QT_QPA_PLATFORM", "xcb");
    }
#endif

    QStringList chromiumFlags;
#ifdef USE_WAYLAND_SUBSURFACE
    if (isWayland) {
      // Disable GPU compositing in Chromium - jellyfin-web triggers Vulkan crash with radeon
      chromiumFlags << "--disable-gpu-compositing";
    } else {
#endif
#ifdef Q_OS_LINUX
      chromiumFlags << "--disable-features=MediaSessionService,HardwareMediaKeyHandling";
#endif
#ifdef USE_WAYLAND_SUBSURFACE
    }
#endif
    // Disable pinch-to-zoom if browser zoom is not allowed
    QVariant allowZoom = SettingsComponent::readPreinitValue(SETTINGS_SECTION_MAIN, "allowBrowserZoom");
    if (allowZoom.isValid() && !allowZoom.toBool())
      chromiumFlags << "--disable-pinch";

    if (parser.isSet("ignore-certificate-errors"))
      chromiumFlags << "--ignore-certificate-errors";

    if (!chromiumFlags.isEmpty())
      qputenv("QTWEBENGINE_CHROMIUM_FLAGS", chromiumFlags.join(" ").toUtf8());

    if (parser.isSet("remote-debugging-port"))
      qputenv("QTWEBENGINE_REMOTE_DEBUGGING", parser.value("remote-debugging-port").toUtf8());

    // Must initialize QtWebEngine before QGuiApplication (per Qt docs)
    QtWebEngineQuick::initialize();

#ifdef USE_WAYLAND_SUBSURFACE
    // Set Vulkan graphics API for Wayland (must be after QtWebEngineQuick::initialize, before QApplication)
    if (isWayland) {
      QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    }
#endif

#ifdef USE_WAYLAND_SUBSURFACE
    // Use QGuiApplication for Vulkan (QApplication's widget stuff conflicts with Vulkan)
    std::unique_ptr<QCoreApplication> appPtr;
    if (isWayland) {
      appPtr.reset(new QGuiApplication(newArgc, newArgv));
    } else {
      appPtr.reset(new QApplication(newArgc, newArgv));
    }
    auto& app = *appPtr;
#else
    QApplication app(newArgc, newArgv);
#endif

#ifdef USE_WAYLAND_SUBSURFACE
    // Create Vulkan instance for Qt (must be after QApplication, before QML engine)
    static QVulkanInstance vulkanInstance;
    if (isWayland) {
      vulkanInstance.setApiVersion(QVersionNumber(1, 2));
      if (!vulkanInstance.create()) {
        qCritical() << "Failed to create Qt Vulkan instance";
        return EXIT_FAILURE;
      }
    }
#endif

#if defined(Q_OS_WIN)
    // Setting window icon on OSX will break user ability to change it
    qobject_cast<QGuiApplication*>(&app)->setWindowIcon(QIcon(":/images/icon.png"));
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
  	// Set window icon on Linux using system icon theme
  	qobject_cast<QGuiApplication*>(&app)->setWindowIcon(QIcon::fromTheme("org.jellyfin.JellyfinDesktop", QIcon(":/images/icon.png")));
    // Set app id for Wayland compositor window icon
    qobject_cast<QGuiApplication*>(&app)->setDesktopFileName("org.jellyfin.JellyfinDesktop");
#endif

    // Configure default WebEngineProfile paths early (profile is already set)
    // Skip early access on Wayland Vulkan - accessing defaultProfile() here triggers
    // Chromium GPU init which crashes. Will be set later after QML engine loads.
#ifdef USE_WAYLAND_SUBSURFACE
    if (!isWayland) {
#endif
      QString webEngineDir = Paths::dataDir("QtWebEngine");
      QWebEngineProfile* defaultProfile = QWebEngineProfile::defaultProfile();
      defaultProfile->setCachePath(webEngineDir);
      defaultProfile->setPersistentStoragePath(webEngineDir);
#ifdef USE_WAYLAND_SUBSURFACE
    }
#endif

#if defined(Q_OS_MAC) && defined(NDEBUG)
    PFMoveToApplicationsFolderIfNecessary();
#endif

    UniqueApplication* uniqueApp = new UniqueApplication(&app);
    if (!uniqueApp->ensureUnique())
    {
      Log::Cleanup();
      return EXIT_SUCCESS;
    }

#ifdef Q_OS_UNIX
    // install signals handlers for proper app closing.
    SignalManager signalManager(qobject_cast<QGuiApplication*>(&app));
    Q_UNUSED(signalManager);
#endif

    detectOpenGLLate();

    // Initialize all the components. This needs to be done
    // early since most everything else relies on it
    //
    ComponentManager::Get().initialize();

    // Rotate temp log to main log file and rotate old logs
    Log::RotateLog();

    qInfo() << "Config directory:" << qPrintable(Paths::dataDir());

    Codecs::preinitCodecs();

    Log::ApplyConfigLogLevel();

    SettingsComponent::Get().setCommandLineValues(parser.optionNames());

    // Set user agent now that SystemComponent is available
    // Skip early access on Wayland Vulkan - will be set in objectCreated callback
#ifdef USE_WAYLAND_SUBSURFACE
    if (!isWayland) {
#endif
      QWebEngineProfile::defaultProfile()->setHttpUserAgent(SystemComponent::Get().getUserAgent());
#ifdef USE_WAYLAND_SUBSURFACE
    }
#endif

    // load QtWebChannel so that we can register our components with it.
    QQmlApplicationEngine *engine = Globals::Engine();

    Globals::SetContextProperty("components", &ComponentManager::Get().getQmlPropertyMap());

    // the only way to detect if QML parsing fails is to hook to this signal and then see
    // if we get a valid object passed to it. Any error messages will be reported on stderr
    // but since no normal user should ever see this it should be fine
    //
    QObject::connect(engine, &QQmlApplicationEngine::objectCreated, [&](QObject* object, const QUrl& url)
    {
      Q_UNUSED(url);

      if (object == nullptr)
        throw FatalException(QObject::tr("Failed to parse application engine script."));

      QQuickWindow* window = Globals::MainWindow();

#ifdef USE_WAYLAND_SUBSURFACE
      // Set Vulkan instance on window when using Vulkan
      if (isWayland && vulkanInstance.isValid()) {
        window->setVulkanInstance(&vulkanInstance);
      }
      // Deferred WebEngineProfile setup for Wayland Vulkan
      if (isWayland) {
        QString webEngineDir = Paths::dataDir("QtWebEngine");
        QWebEngineProfile* defaultProfile = QWebEngineProfile::defaultProfile();
        defaultProfile->setCachePath(webEngineDir);
        defaultProfile->setPersistentStoragePath(webEngineDir);
        defaultProfile->setHttpUserAgent(SystemComponent::Get().getUserAgent());
      }
#endif

      // Set window flags for proper popup handling (e.g., WebEngineView dropdowns)
      window->setFlags(window->flags() | Qt::WindowFullscreenButtonHint);

      // Install event filter for proper event handling
      window->installEventFilter(new EventFilter(window));

      // Install application event filter to catch popup window creation early
      class PopupFixer : public QObject {
        QQuickWindow* m_mainWindow;
      public:
        PopupFixer(QQuickWindow* mainWin) : m_mainWindow(mainWin) {}
        bool eventFilter(QObject* obj, QEvent* event) override {
          auto* win = qobject_cast<QWindow*>(obj);
          if (!win || win == m_mainWindow) {
            return QObject::eventFilter(obj, event);
          }

          // Fix WebEngineView popup flags to accept focus
          if (event->type() == QEvent::Show) {
            Qt::WindowFlags flags = win->flags();

            // Only fix WebEngineView dropdowns (Tool + FramelessWindowHint + WindowStaysOnTopHint)
            // Don't touch other windows (e.g., MPV-related)
            bool isWebEnginePopup = (flags & Qt::Tool) &&
                                     (flags & Qt::FramelessWindowHint) &&
                                     (flags & Qt::WindowStaysOnTopHint);

            if (!isWebEnginePopup) {
              return QObject::eventFilter(obj, event);
            }

            if (win->transientParent() == nullptr) {
              win->setTransientParent(m_mainWindow);
            }

            if (win->modality() != Qt::NonModal) {
              win->setModality(Qt::NonModal);
            }

            // WebEngineView creates popups with Qt::Tool | WindowDoesNotAcceptFocus
            // which prevents interaction. Change to Qt::Popup to accept focus.
            flags &= ~Qt::Tool;
            flags |= Qt::Popup;
            flags &= ~Qt::WindowDoesNotAcceptFocus;
            win->setFlags(flags);
          }

          return QObject::eventFilter(obj, event);
        }
      };
      app.installEventFilter(new PopupFixer(window));

      QObject* webChannel = qvariant_cast<QObject*>(window->property("webChannel"));
      Q_ASSERT(webChannel);
      ComponentManager::Get().setWebChannel(qobject_cast<QWebChannel*>(webChannel));

      // Initialize WindowManager with window reference
      WindowManager::Get().initializeWindow(window);

      // Handle other app focus by raising window
      QObject::connect(uniqueApp, &UniqueApplication::otherApplicationStarted, []() {
        WindowManager::Get().raiseWindow();
      });
    });
    engine->load(QUrl(QStringLiteral("qrc:/ui/webview.qml")));

    // run our application
    int ret = app.exec();

    delete uniqueApp;
    Globals::EngineDestroy();

    Codecs::Uninit();
    return ret;
  }
  catch (FatalException& e)
  {
    qFatal("Unhandled FatalException: %s", qPrintable(e.message()));
    QApplication errApp(argc, argv);

    auto  msg = new ErrorMessage(e.message(), true);
    msg->show();

    errApp.exec();

    Codecs::Uninit();
    return 1;
  }
}
