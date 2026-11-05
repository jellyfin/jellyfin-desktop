#include <QtGlobal>
#include <QColorSpace>
#include <QSurfaceFormat>
#include <QCoreApplication>
#include <QOpenGLContext>
#include <QDebug>

#include <mpv/client.h>


#include "QtHelper.h"
#include "OpenGLDetect.h"

#if defined(Q_OS_MAC)

///////////////////////////////////////////////////////////////////////////////////////////////////
void detectOpenGLEarly()
{
  // Request OpenGL 4.1 if possible on OSX, otherwise it defaults to 2.0
  // This needs to be done before we create the QGuiApplication
  //
  QSurfaceFormat format = QSurfaceFormat::defaultFormat();
  format.setMajorVersion(3);
  format.setMinorVersion(2);
  format.setProfile(QSurfaceFormat::CoreProfile);
  // Fix oversaturated colors on HDR displays
  format.setColorSpace(QColorSpace::SRgb);
  QSurfaceFormat::setDefaultFormat(format);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void detectOpenGLLate()
{
}

#elif defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)

///////////////////////////////////////////////////////////////////////////////////////////////////
// Attempt to reuse mpv's code for detecting whether we want GLX or EGL (which
// is tricky to do because of hardware decoding concerns). This is not pretty,
// but quite effective and without having to duplicate too much GLX/EGL code.
static QString probeHwdecInterop()
{
  // Qt sets the locale, but libmpv requires the LC_NUMERIC category to be set to "C",
  // so change it back.
  std::setlocale(LC_NUMERIC, "C");

  auto mpv = mpv::qt::Handle::FromRawHandle(mpv_create());
  if (!mpv)
    return "";
  mpv::qt::set_property(mpv, "gpu-hwdec-interop", "auto");
  // Actually creating a window is required. There is currently no way to keep
  // this window hidden or invisible.
  mpv::qt::set_property(mpv, "force-window", true);
  // As a mitigation, put the window in the top/right corner, and make it as
  // small as possible by forcing 1x1 size and removing window borders.
  mpv::qt::set_property(mpv, "geometry", "1x1+0+0");
  mpv::qt::set_property(mpv, "border", false);
  if (mpv_initialize(mpv) < 0)
    return "";
  return mpv::qt::get_property(mpv, "hwdec-interop").toString();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void detectOpenGLEarly()
{
  // The putenv call must happen before Qt initializes its platform stuff.
  if (probeHwdecInterop() == "vaapi-egl")
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void detectOpenGLLate()
{
}

#elif defined(Q_OS_WIN)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <GL/gl.h>

#include <optional>

#include <QSettings>

using Microsoft::WRL::ComPtr;

#define WGL_ACCESS_READ_ONLY_NV 0x0000
typedef HANDLE(WINAPI* PFNWGLDXOPENDEVICENVPROC)(void* dxDevice);
typedef BOOL(WINAPI* PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE(WINAPI* PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice, void* dxObject, GLuint name, GLenum type,
                                                     GLenum access);
typedef BOOL(WINAPI* PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice, HANDLE hObject);
typedef BOOL(WINAPI* PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE* hObjects);

// Number of register/unregister cycles the leak probe runs
static const int PROBE_CYCLES = 16;

///////////////////////////////////////////////////////////////////////////////////////////////////
template <typename T>
static T resolveExport(HMODULE module, const char* name)
{
  return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Like Qt, treat the small values some drivers return for unknown functions as missing
template <typename T>
static T resolveGL(decltype(&wglGetProcAddress) getProcAddress, const char* name)
{
  auto value = reinterpret_cast<quintptr>(getProcAddress(name));
  return value >= 4 && value != quintptr(-1) ? reinterpret_cast<T>(value) : nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static DWORD processHandleCount()
{
  DWORD count = 0;
  GetProcessHandleCount(GetCurrentProcess(), &count);
  return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The adapter Chromium and the OpenGL driver use by default, with a key that changes whenever
// the GPU or its driver does
static ComPtr<IDXGIAdapter1> defaultAdapter(QString* key, UINT* vendorId)
{
  auto createFactory = resolveExport<decltype(&CreateDXGIFactory1)>(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1");
  ComPtr<IDXGIFactory1> factory;
  ComPtr<IDXGIAdapter1> adapter;
  if (!createFactory || FAILED(createFactory(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters1(0, &adapter)))
    return nullptr;

  DXGI_ADAPTER_DESC1 desc = {};
  adapter->GetDesc1(&desc);
  LARGE_INTEGER driver = {};
  adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
  *vendorId = desc.VendorId;
  *key = QString("%1:%2:%3.%4.%5.%6")
           .arg(desc.VendorId, 4, 16, QChar('0'))
           .arg(desc.DeviceId, 4, 16, QChar('0'))
           .arg(HIWORD(driver.HighPart))
           .arg(LOWORD(driver.HighPart))
           .arg(HIWORD(driver.LowPart))
           .arg(LOWORD(driver.LowPart));
  return adapter;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// QtWebEngine hands every Chromium frame to the OpenGL scene graph by registering a new D3D11
// texture with WGL_NV_DX_interop and unregistering it after use. Some drivers (AMD's, as of
// 31.0.21925) never free a texture that was registered, and leak an Event handle with it: about
// 1 GB per minute of browsing. Repeat that cycle with tiny textures and count the leaked handles.
// Returns nothing if the probe couldn't run.
static std::optional<bool> probeDxInteropLeak(IDXGIAdapter1* adapter)
{
  auto createDevice = resolveExport<PFN_D3D11_CREATE_DEVICE>(LoadLibraryW(L"d3d11.dll"), "D3D11CreateDevice");
  HMODULE gl = LoadLibraryW(L"opengl32.dll");
  auto createContext = resolveExport<decltype(&wglCreateContext)>(gl, "wglCreateContext");
  auto makeCurrent = resolveExport<decltype(&wglMakeCurrent)>(gl, "wglMakeCurrent");
  auto deleteContext = resolveExport<decltype(&wglDeleteContext)>(gl, "wglDeleteContext");
  auto getProcAddress = resolveExport<decltype(&wglGetProcAddress)>(gl, "wglGetProcAddress");
  auto genTextures = resolveExport<decltype(&glGenTextures)>(gl, "glGenTextures");
  auto deleteTextures = resolveExport<decltype(&glDeleteTextures)>(gl, "glDeleteTextures");
  if (!createDevice || !createContext || !makeCurrent || !deleteContext || !getProcAddress || !genTextures ||
      !deleteTextures)
    return std::nullopt;

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  if (FAILED(createDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                          D3D11_SDK_VERSION, &device, nullptr, &context)))
    return std::nullopt;

  std::optional<bool> leaks;

  // STATIC is a predefined window class, so nothing needs registering
  HWND hwnd = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
  HDC dc = hwnd ? GetDC(hwnd) : nullptr;
  HGLRC ctx = nullptr;
  if (dc)
  {
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    int format = ChoosePixelFormat(dc, &pfd);
    if (format && SetPixelFormat(dc, format, &pfd))
      ctx = createContext(dc);
  }

  if (ctx && makeCurrent(dc, ctx))
  {
    auto openDevice = resolveGL<PFNWGLDXOPENDEVICENVPROC>(getProcAddress, "wglDXOpenDeviceNV");
    auto closeDevice = resolveGL<PFNWGLDXCLOSEDEVICENVPROC>(getProcAddress, "wglDXCloseDeviceNV");
    auto registerObject = resolveGL<PFNWGLDXREGISTEROBJECTNVPROC>(getProcAddress, "wglDXRegisterObjectNV");
    auto unregisterObject = resolveGL<PFNWGLDXUNREGISTEROBJECTNVPROC>(getProcAddress, "wglDXUnregisterObjectNV");
    auto lockObjects = resolveGL<PFNWGLDXLOCKOBJECTSNVPROC>(getProcAddress, "wglDXLockObjectsNV");
    auto unlockObjects = resolveGL<PFNWGLDXLOCKOBJECTSNVPROC>(getProcAddress, "wglDXUnlockObjectsNV");
    HANDLE interop = openDevice && closeDevice && registerObject && unregisterObject && lockObjects && unlockObjects
                       ? openDevice(device.Get())
                       : nullptr;
    if (interop)
    {
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = 16;
      desc.Height = 16;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

      // Handles left over after creating and releasing textures, with or without interop
      auto handlesLeaked = [&](int cycles, bool interopCycle) -> std::optional<int>
      {
        context->Flush();
        DWORD before = processHandleCount();
        for (int i = 0; i < cycles; i++)
        {
          ComPtr<ID3D11Texture2D> texture;
          if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)))
            return std::nullopt;
          if (interopCycle)
          {
            GLuint name = 0;
            genTextures(1, &name);
            HANDLE object = registerObject(interop, texture.Get(), name, GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
            if (object)
            {
              lockObjects(interop, 1, &object);
              unlockObjects(interop, 1, &object);
              unregisterObject(interop, object);
            }
            deleteTextures(1, &name);
            if (!object)
              return std::nullopt;
          }
          texture.Reset();
          context->Flush();
        }
        return int(processHandleCount()) - int(before);
      };

      // The first registration allocates driver state that stays around, so leave it out
      if (handlesLeaked(1, true))
      {
        auto control = handlesLeaked(PROBE_CYCLES, false);
        auto leaked = handlesLeaked(PROBE_CYCLES, true);
        if (control && leaked)
        {
          qInfo() << "DX interop probe:" << *leaked - *control << "handles leaked in" << PROBE_CYCLES << "cycles";
          // Affected drivers leak exactly one per cycle, others none
          leaks = *leaked - *control >= PROBE_CYCLES * 3 / 4;
        }
      }
      closeDevice(interop);
    }
    makeCurrent(nullptr, nullptr);
  }

  if (ctx)
    deleteContext(ctx);
  if (dc)
    ReleaseDC(hwnd, dc);
  if (hwnd)
    DestroyWindow(hwnd);
  return leaks;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool dxInteropLeaksTextures(const QString& cachePath)
{
  // The probe only says something about the system OpenGL driver
  if (!qgetenv("QT_OPENGL_DLL").isEmpty() || qgetenv("QT_OPENGL") == "software")
    return false;

  QString adapterKey;
  UINT vendorId = 0;
  ComPtr<IDXGIAdapter1> adapter = defaultAdapter(&adapterKey, &vendorId);
  if (!adapter)
    return false;

  // Probing takes about 150 ms, so only do it again when the GPU or its driver changes
  std::optional<QSettings> cache;
  if (!cachePath.isEmpty())
  {
    cache.emplace(cachePath, QSettings::IniFormat);
    if (cache->value("dxInteropProbe/adapter").toString() == adapterKey)
      return cache->value("dxInteropProbe/leaks").toBool();
  }

  std::optional<bool> leaks = probeDxInteropLeak(adapter.Get());
  if (!leaks)
  {
    // Don't remember an inconclusive result. AMD is the driver known to leak.
    qWarning() << "DX interop probe failed for adapter" << adapterKey;
    return vendorId == 0x1002;
  }

  if (cache)
  {
    cache->setValue("dxInteropProbe/adapter", adapterKey);
    cache->setValue("dxInteropProbe/leaks", *leaks);
  }
  return *leaks;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void detectOpenGLEarly()
{
}

/////////////////////////////////////////////////////////////////////////////////////////
void detectOpenGLLate()
{
  // Qt 6 does not support AA_UseOpenGLES - it uses desktop OpenGL by default
  // No need to force GLES version, let Qt use the native OpenGL
  qInfo() << "Using native desktop OpenGL (Qt 6)";
}

#else

///////////////////////////////////////////////////////////////////////////////////////////////////
void detectOpenGLEarly()
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void detectOpenGLLate()
{
}

#endif
