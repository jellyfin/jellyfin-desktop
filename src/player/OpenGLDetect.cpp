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

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstring>

static DXInterop g_dxInterop = DXInterop::Native;

///////////////////////////////////////////////////////////////////////////////////////////////////
// The OpenGL library the qwindows platform plugin will load
static QByteArray qtOpenGLLibrary()
{
  QByteArray library = qgetenv("QT_OPENGL_DLL");
  if (library.isEmpty())
    library = qgetenv("QT_OPENGL") == "software" ? "opengl32sw" : "opengl32";
  return library;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Like Qt, treat the small values some drivers return for unknown functions as missing
static bool isValidProc(PROC proc)
{
  auto value = reinterpret_cast<quintptr>(proc);
  return value >= 4 && value != quintptr(-1);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
template <typename T>
static T resolveExport(HMODULE module, const char *name)
{
  return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
template <typename T>
static T resolveExtension(decltype(&wglGetProcAddress) getProcAddress, const char *name)
{
  PROC proc = getProcAddress(name);
  return isValidProc(proc) ? reinterpret_cast<T>(proc) : nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Import a D3D11 texture shared with an NT handle into GL the way DXInteropShim does with
// every frame, to know the driver supports it. Needs a current GL context.
static bool canImportD3D11Texture(HMODULE gl, decltype(&wglGetProcAddress) getProcAddress)
{
  auto genTextures = resolveExport<decltype(&glGenTextures)>(gl, "glGenTextures");
  auto deleteTextures = resolveExport<decltype(&glDeleteTextures)>(gl, "glDeleteTextures");
  auto bindTexture = resolveExport<decltype(&glBindTexture)>(gl, "glBindTexture");
  auto getError = resolveExport<decltype(&glGetError)>(gl, "glGetError");
  auto createMemoryObjects =
    resolveExtension<PFNGLCREATEMEMORYOBJECTSEXTPROC>(getProcAddress, "glCreateMemoryObjectsEXT");
  auto deleteMemoryObjects =
    resolveExtension<PFNGLDELETEMEMORYOBJECTSEXTPROC>(getProcAddress, "glDeleteMemoryObjectsEXT");
  auto memoryObjectParameteriv = resolveExtension<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>(
    getProcAddress, "glMemoryObjectParameterivEXT");
  auto importMemoryWin32Handle = resolveExtension<PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC>(
    getProcAddress, "glImportMemoryWin32HandleEXT");
  auto texStorageMem2D =
    resolveExtension<PFNGLTEXSTORAGEMEM2DEXTPROC>(getProcAddress, "glTexStorageMem2DEXT");
  auto createDevice =
    resolveExport<PFN_D3D11_CREATE_DEVICE>(LoadLibraryW(L"d3d11.dll"), "D3D11CreateDevice");
  if (!genTextures || !deleteTextures || !bindTexture || !getError || !createMemoryObjects ||
      !deleteMemoryObjects || !memoryObjectParameteriv || !importMemoryWin32Handle ||
      !texStorageMem2D || !createDevice)
    return false;

  // Chromium hands over RGBA textures
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = desc.Height = 16;
  desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

  bool ok = false;
  ID3D11Device *device = nullptr;
  ID3D11Texture2D *texture = nullptr;
  IDXGIResource1 *resource = nullptr;
  HANDLE handle = nullptr;
  const DWORD access = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;
  if (SUCCEEDED(createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                             D3D11_SDK_VERSION, &device, nullptr, nullptr)) &&
      SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) &&
      SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(&resource))) &&
      SUCCEEDED(resource->CreateSharedHandle(nullptr, access, nullptr, &handle)))
  {
    const GLint dedicated = GL_TRUE;
    GLuint memory = 0, glTexture = 0;
    createMemoryObjects(1, &memory);
    memoryObjectParameteriv(memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    importMemoryWin32Handle(memory, 16 * 16 * 8, GL_HANDLE_TYPE_D3D11_IMAGE_EXT, handle);
    genTextures(1, &glTexture);
    bindTexture(GL_TEXTURE_2D, glTexture);
    texStorageMem2D(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16, memory, 0);
    ok = getError() == GL_NO_ERROR;
    bindTexture(GL_TEXTURE_2D, 0);
    deleteTextures(1, &glTexture);
    deleteMemoryObjects(1, &memory);
  }
  if (handle)
    CloseHandle(handle);
  if (resource)
    resource->Release();
  if (texture)
    texture->Release();
  if (device)
    device->Release();
  return ok;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// QtWebEngine hands Chromium's D3D11 frames to the OpenGL scene graph through
// WGL_NV_DX_interop, and calls wglDXOpenDeviceNV without checking it exists.
// Some drivers don't provide it (e.g. Microsoft's OpenGLOn12 mapping layer, which is
// what OpenGL runs on with Windows on ARM / Snapdragon GPUs), so startup crashes with
// a call through a null pointer. Probe for it with a throwaway WGL context first, and
// for D3D11 texture import through GL_EXT_memory_object_win32, which lets us emulate it
// (see DXInteropShim).
static DXInterop probeDxInterop()
{
  const QByteArray library = qtOpenGLLibrary();
  // Without a context we can't tell. Keep the default for the system library, but a
  // replacement (such as Qt's software opengl32sw) is safer composited in software.
  const bool replacement = library.compare("opengl32", Qt::CaseInsensitive) != 0;
  DXInterop result = replacement ? DXInterop::Unsupported : DXInterop::Native;
  QByteArray renderer = "probe failed";

  HMODULE gl = LoadLibraryA(library.constData());
  auto createContext = resolveExport<decltype(&wglCreateContext)>(gl, "wglCreateContext");
  auto makeCurrent = resolveExport<decltype(&wglMakeCurrent)>(gl, "wglMakeCurrent");
  auto deleteContext = resolveExport<decltype(&wglDeleteContext)>(gl, "wglDeleteContext");
  auto getProcAddress = resolveExport<decltype(&wglGetProcAddress)>(gl, "wglGetProcAddress");
  auto getString = resolveExport<decltype(&glGetString)>(gl, "glGetString");
  // Like Qt, a replacement library handles pixel formats itself
  auto choosePixelFormat = &ChoosePixelFormat;
  auto setPixelFormat = &SetPixelFormat;
  if (replacement)
  {
    choosePixelFormat = resolveExport<decltype(&ChoosePixelFormat)>(gl, "wglChoosePixelFormat");
    setPixelFormat = resolveExport<decltype(&SetPixelFormat)>(gl, "wglSetPixelFormat");
  }
  bool resolved = createContext && makeCurrent && deleteContext && getProcAddress && getString &&
                  choosePixelFormat && setPixelFormat;

  // STATIC is a predefined window class, so nothing needs registering
  HWND hwnd = resolved ? CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
                                         nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)
                       : nullptr;
  HDC dc = hwnd ? GetDC(hwnd) : nullptr;
  if (dc)
  {
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    int format = choosePixelFormat(dc, &pfd);
    if (format && setPixelFormat(dc, format, &pfd))
    {
      HGLRC ctx = createContext(dc);
      if (ctx && makeCurrent(dc, ctx))
      {
        renderer = reinterpret_cast<const char *>(getString(GL_RENDERER));
        auto extensions = reinterpret_cast<const char *>(getString(GL_EXTENSIONS));
        if (isValidProc(getProcAddress("wglDXOpenDeviceNV")))
          result = DXInterop::Native;
        else if (extensions && strstr(extensions, "GL_EXT_memory_object_win32") &&
                 canImportD3D11Texture(gl, getProcAddress))
          result = DXInterop::Emulated;
        else
          result = DXInterop::Unsupported;
        makeCurrent(nullptr, nullptr);
      }
      if (ctx)
        deleteContext(ctx);
    }
    ReleaseDC(hwnd, dc);
  }
  if (hwnd)
    DestroyWindow(hwnd);

  static const char *const names[] = { "native", "emulated", "unsupported" };
  qInfo() << "OpenGL:" << library.constData() << "-" << renderer.constData()
          << "- WGL_NV_DX_interop" << names[int(result)];
  return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void detectOpenGLEarly()
{
  g_dxInterop = probeDxInterop();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
DXInterop openGLDXInterop()
{
  return g_dxInterop;
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
