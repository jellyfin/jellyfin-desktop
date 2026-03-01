#include <QGuiApplication>
#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QtQml>
#include <optional>
#include <Qt>
#include <QtWebEngineQuick>
#include <qtwebenginecoreglobal.h>
#include <QtWebEngineCore/QWebEngineProfile>
#include <QtWebEngineCore/QWebEngineScript>
#include <QCommandLineOption>
#include <QDebug>
#include <QSettings>

#include "shared/Names.h"
#include "system/SystemComponent.h"
#include "Paths.h"
#include "core/ProfileManager.h"
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

#if defined(Q_OS_WIN) && defined(_M_X64)
#include <windows.h>
#include <cstdio>

/////////////////////////////////////////////////////////////////////////////////////////
// Check AVX2 support and swap libmpv DLL if needed (before delay-load triggers)
// Only relevant for x64 - ARM64 doesn't have AVX2
static void setupMpvFallback()
{
  // Check if AVX2 is available (PF_AVX2_INSTRUCTIONS_AVAILABLE = 40)
  if (IsProcessorFeaturePresent(40))
  {
    fprintf(stderr, "AVX2 supported, using primary libmpv\n");
    return;
  }

  fprintf(stderr, "AVX2 not supported, checking for fallback libmpv\n");

  // Get application directory
  wchar_t exePath[MAX_PATH];
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  std::wstring appDir(exePath);
  size_t lastSlash = appDir.find_last_of(L"\\/");
  if (lastSlash != std::wstring::npos)
    appDir = appDir.substr(0, lastSlash + 1);

  std::wstring fallbackPath = appDir + L"libmpv-fallback.dll";
  std::wstring primaryPath = appDir + L"libmpv-2.dll";

  // Check if fallback exists
  if (GetFileAttributesW(fallbackPath.c_str()) == INVALID_FILE_ATTRIBUTES)
  {
    fprintf(stderr, "No fallback libmpv available\n");
    return;
  }

  // Delete primary and rename fallback
  DeleteFileW(primaryPath.c_str());
  if (MoveFileW(fallbackPath.c_str(), primaryPath.c_str()))
    fprintf(stderr, "Switched to fallback libmpv (non-AVX2)\n");
  else
    fprintf(stderr, "Failed to switch to fallback libmpv\n");
}
#endif

/////////////////////////////////////////////////////////////////////////////////////////
static void preinitQt()
{
  QCoreApplication::setApplicationName(Names::DataName());
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
QStringList g_qtFlags = {
  "--enable-gpu-rasterization",
  "--disable-features=MediaSessionService"
};

/////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char *argv[])
{
#if defined(Q_OS_WIN) && defined(_M_X64)
  // Must run before any mpv code triggers delay-load
  setupMpvFallback();
#endif

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

    char **newArgv = appendCommandLineArguments(argc, argv, g_qtFlags);
    int newArgc = argc + static_cast<int>(g_qtFlags.size());

    preinitQt();

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

      // Detect portable mode while QCoreApplication exists (needed for applicationDirPath)
      Paths::detectAndEnablePortableMode();

#ifdef Q_OS_WIN
      // Disable pipeline cache in portable mode to avoid writing to user's AppData
      if (Paths::isPortableMode())
        qputenv("QSG_RHI_DISABLE_DISK_CACHE", "1");
#endif
    }

    detectOpenGLEarly();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

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

    // Set QSettings path for portable mode
    if (Paths::isPortableMode())
    {
      QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                         Paths::globalDataDir());
    }

    // Handle config-dir (overrides portable mode if explicitly set)
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

    // Handle --list-profiles
    if (parser.isSet("list-profiles"))
    {
      auto allProfiles = ProfileManager::profiles();
      auto defaultProf = ProfileManager::defaultProfile();
      if (allProfiles.isEmpty())
      {
        printf("No profiles found.\n");
      }
      else
      {
        for (const Profile& p : allProfiles)
        {
          QString marker = (defaultProf && p.name() == defaultProf->name()) ? " *" : "";
          printf("%s%s\n", qPrintable(p.name()), qPrintable(marker));
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
      if (ProfileManager::profileByName(name))
      {
        fprintf(stderr, "Profile already exists: %s\n", qPrintable(name));
        return EXIT_FAILURE;
      }
      ProfileManager::createProfile(name);
      printf("Created profile: %s\n", qPrintable(name));
      return EXIT_SUCCESS;
    }

    // Handle --delete-profile
    if (parser.isSet("delete-profile"))
    {
      QString name = parser.value("delete-profile");
      auto profile = ProfileManager::profileByName(name);
      if (!profile)
      {
        fprintf(stderr, "Profile not found: %s\n", qPrintable(name));
        return EXIT_FAILURE;
      }
      if (!ProfileManager::deleteProfile(*profile))
      {
        fprintf(stderr, "Failed to delete profile: %s\n", qPrintable(name));
        return EXIT_FAILURE;
      }
      printf("Deleted profile: %s\n", qPrintable(name));
      return EXIT_SUCCESS;
    }

    // Handle --set-default-profile
    if (parser.isSet("set-default-profile"))
    {
      QString name = parser.value("set-default-profile");
      auto profile = ProfileManager::profileByName(name);
      if (!profile)
      {
        fprintf(stderr, "Profile not found: %s\n", qPrintable(name));
        return EXIT_FAILURE;
      }
      ProfileManager::setDefaultProfile(*profile);
      printf("Default profile set to: %s\n", qPrintable(name));
      return EXIT_SUCCESS;
    }

    // Determine which profile to use for this session
    std::optional<Profile> activeProfile;

    // --profile overrides everything
    if (parser.isSet("profile"))
    {
      QString name = parser.value("profile");
      activeProfile = ProfileManager::profileByName(name);
      if (!activeProfile)
      {
        fprintf(stderr, "Profile not found: %s\n", qPrintable(name));
        return EXIT_FAILURE;
      }
    }
    else
    {
      // Try default profile
      activeProfile = ProfileManager::defaultProfile();

      // Try first available profile
      if (!activeProfile)
      {
        auto allProfiles = ProfileManager::profiles();
        if (!allProfiles.isEmpty())
          activeProfile = allProfiles.first();
      }

      // Create new profile if none exist
      if (!activeProfile)
        activeProfile = ProfileManager::createProfile("Default");

      // Update default
      ProfileManager::setDefaultProfile(*activeProfile);
    }

    ProfileManager::Get().setActiveProfile(*activeProfile);

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
#ifdef Q_OS_LINUX
    // Disable QtWebEngine's automatic MPRIS registration - we handle it ourselves
    chromiumFlags << "--disable-features=MediaSessionService,HardwareMediaKeyHandling";
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

    QtWebEngineQuick::initialize();
    QApplication app(newArgc, newArgv);

#if defined(Q_OS_WIN) 
    // Setting window icon on OSX will break user ability to change it
    app.setWindowIcon(QIcon(":/images/icon.png"));
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
  	// Set window icon on Linux using system icon theme
  	app.setWindowIcon(QIcon::fromTheme("org.jellyfin.JellyfinDesktop", QIcon(":/images/icon.png")));
    // Set app id for Wayland compositor window icon
    app.setDesktopFileName("org.jellyfin.JellyfinDesktop");
#endif

    // Configure default WebEngineProfile paths early (profile is already set)
    {
      QWebEngineProfile* defaultProfile = QWebEngineProfile::defaultProfile();
      defaultProfile->setCachePath(ProfileManager::activeProfile().cacheDir("QtWebEngine"));
      defaultProfile->setPersistentStoragePath(ProfileManager::activeProfile().dataDir("QtWebEngine"));
    }

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
    SignalManager signalManager(&app);
    Q_UNUSED(signalManager);
#endif

    detectOpenGLLate();

    // Initialize all the components. This needs to be done
    // early since most everything else relies on it
    //
    ComponentManager::Get().initialize();

    // Rotate temp log to main log file and rotate old logs
    Log::RotateLog();

    qInfo() << "Config directory:" << qPrintable(ProfileManager::activeProfile().dataDir());

    Log::ApplyConfigLogLevel();

    SettingsComponent::Get().setCommandLineValues(parser.optionNames());

    // Set user agent now that SystemComponent is available
    QWebEngineProfile::defaultProfile()->setHttpUserAgent(SystemComponent::Get().getUserAgent());

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
    engine->load(QUrl(QStringLiteral("qrc:/webview.qml")));

    // run our application
    int ret = app.exec();

    delete uniqueApp;
    Globals::EngineDestroy();

    return ret;
  }
  catch (FatalException& e)
  {
    qFatal("Unhandled FatalException: %s", qPrintable(e.message()));
    QApplication errApp(argc, argv);

    auto  msg = new ErrorMessage(e.message(), true);
    msg->show();

    errApp.exec();

    return 1;
  }
}
