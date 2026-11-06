#include "DXInteropShim.h"

#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstring>
#include <mutex>
#include <vector>

// QtWebEngine (6.9) shares Chromium's frames with the OpenGL scene graph like this,
// once per frame: copy the frame into a fresh D3D11 texture, glGenTextures(),
// wglDXRegisterObjectNV(), wglDXLockObjectsNV(); and on release unlock, unregister and
// delete. We implement those calls with an NT-handle shared D3D11 texture imported into
// GL through GL_EXT_memory_object_win32, which locking copies the registered texture
// into, so the frames stay on the GPU. OpenGLOn12 ignores the values of fences shared
// through GL_EXT_semaphore_win32, so they can't order the copies against GL: we wait for
// each copy on the CPU, and pooled textures are only reused once the GPU must be done
// with them. They're pooled since Qt registers a new texture every frame.

namespace
{

struct GLFunctions
{
  PFNGLCREATEMEMORYOBJECTSEXTPROC CreateMemoryObjectsEXT = nullptr;
  PFNGLDELETEMEMORYOBJECTSEXTPROC DeleteMemoryObjectsEXT = nullptr;
  PFNGLMEMORYOBJECTPARAMETERIVEXTPROC MemoryObjectParameterivEXT = nullptr;
  PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC ImportMemoryWin32HandleEXT = nullptr;
  PFNGLTEXSTORAGEMEM2DEXTPROC TexStorageMem2DEXT = nullptr;
};

struct PoolEntry
{
  ID3D11Texture2D *texture = nullptr; // the copy GL samples
  HANDLE handle = nullptr;
  GLuint memory = 0;
  UINT width = 0, height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  ID3D11Texture2D *source = nullptr;  // the registered texture, null while the entry is free
  quint64 lastRelease = 0;
};

// Returned by wglDXOpenDeviceNV, and never freed (see shimDXCloseDevice)
struct Device
{
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  ID3D11Query *query = nullptr;
  GLFunctions gl;
  std::mutex mutex;
  std::vector<PoolEntry *> pool;
  quint64 releases = 0;
};

FARPROC (WINAPI *g_realGetProcAddress)(HMODULE, LPCSTR) = nullptr;

///////////////////////////////////////////////////////////////////////////////////////////////////
Device *toDevice(HANDLE handle)
{
  return handle && handle != INVALID_HANDLE_VALUE ? static_cast<Device *>(handle) : nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Errors left behind by other GL code would make our error checks fail
void clearGLErrors(QOpenGLFunctions *f)
{
  for (int i = 0; i < 16 && f->glGetError() != GL_NO_ERROR; i++)
    ;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
template <typename T>
bool resolve(QOpenGLContext *context, T &function, const char *name)
{
  function = reinterpret_cast<T>(context->getProcAddress(name));
  return function != nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool resolveGL(QOpenGLContext *context, GLFunctions &gl)
{
#define RESOLVE(function) resolve(context, gl.function, "gl" #function)
  bool resolved = RESOLVE(CreateMemoryObjectsEXT) && RESOLVE(DeleteMemoryObjectsEXT) &&
                  RESOLVE(MemoryObjectParameterivEXT) && RESOLVE(ImportMemoryWin32HandleEXT) &&
                  RESOLVE(TexStorageMem2DEXT);
#undef RESOLVE
  return resolved;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The storage format must match the imported resource's: OpenGLOn12 can't import BGRA
// resources at all, but Chromium hands over RGBA ones.
GLenum glFormatFor(DXGI_FORMAT format)
{
  switch (format)
  {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return GL_RGBA8;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return GL_SRGB8_ALPHA8;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return GL_RGB10_A2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return GL_RGBA16F;
    default:
      return 0;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Needs a current GL context
void destroyEntry(Device *dev, PoolEntry *entry)
{
  if (entry->memory)
    dev->gl.DeleteMemoryObjectsEXT(1, &entry->memory);
  if (entry->source)
    entry->source->Release();
  if (entry->handle)
    CloseHandle(entry->handle);
  if (entry->texture)
    entry->texture->Release();
  delete entry;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Needs a current GL context
void destroyDevice(Device *dev)
{
  for (PoolEntry *entry : dev->pool)
    destroyEntry(dev, entry);
  if (dev->query)
    dev->query->Release();
  if (dev->context)
    dev->context->Release();
  if (dev->device)
    dev->device->Release();
  delete dev;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
PoolEntry *createEntry(Device *dev, QOpenGLFunctions *f, UINT width, UINT height,
                       DXGI_FORMAT format)
{
  auto entry = new PoolEntry;
  entry->width = width;
  entry->height = height;
  entry->format = format;

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

  HRESULT hr = dev->device->CreateTexture2D(&desc, nullptr, &entry->texture);
  IDXGIResource1 *resource = nullptr;
  if (SUCCEEDED(hr))
    hr = entry->texture->QueryInterface(IID_PPV_ARGS(&resource));
  if (SUCCEEDED(hr))
  {
    const DWORD access = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;
    hr = resource->CreateSharedHandle(nullptr, access, nullptr, &entry->handle);
    resource->Release();
  }
  if (FAILED(hr))
  {
    qWarning() << "DX interop shim: failed to create shared texture" << Qt::hex << hr;
    destroyEntry(dev, entry);
    return nullptr;
  }

  const GLint dedicated = GL_TRUE;
  clearGLErrors(f);
  dev->gl.CreateMemoryObjectsEXT(1, &entry->memory);
  dev->gl.MemoryObjectParameterivEXT(entry->memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
  dev->gl.ImportMemoryWin32HandleEXT(entry->memory, GLuint64(width) * height * 8,
                                     GL_HANDLE_TYPE_D3D11_IMAGE_EXT, entry->handle);
  if (f->glGetError() != GL_NO_ERROR)
  {
    qWarning() << "DX interop shim: glImportMemoryWin32HandleEXT failed";
    destroyEntry(dev, entry);
    return nullptr;
  }
  return entry;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Reuse the free entry released longest ago, leaving the three most recent releases alone
// since the GPU may still be sampling them. Free entries of another size (left over from
// a resize) or unused for a while are dropped here, where a GL context is current.
PoolEntry *acquireEntry(Device *dev, QOpenGLFunctions *f, UINT width, UINT height,
                        DXGI_FORMAT format)
{
  PoolEntry *best = nullptr;
  int freeMatching = 0;
  for (auto it = dev->pool.begin(); it != dev->pool.end();)
  {
    PoolEntry *entry = *it;
    bool matches = entry->width == width && entry->height == height && entry->format == format;
    if (!entry->source && (!matches || dev->releases - entry->lastRelease > 30))
    {
      destroyEntry(dev, entry);
      it = dev->pool.erase(it);
      continue;
    }
    if (!entry->source)
    {
      freeMatching++;
      if (!best || entry->lastRelease < best->lastRelease)
        best = entry;
    }
    ++it;
  }
  if (best && freeMatching > 3)
    return best;

  PoolEntry *entry = createEntry(dev, f, width, height, format);
  if (!entry)
    return best; // rather than dropping the frame
  dev->pool.push_back(entry);
  return entry;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Block until the copies are done, so GL never samples a partial frame
void waitForCopies(Device *dev)
{
  if (!dev->query)
  {
    D3D11_QUERY_DESC desc = { D3D11_QUERY_EVENT, 0 };
    dev->device->CreateQuery(&desc, &dev->query);
  }
  if (!dev->query)
  {
    dev->context->Flush();
    return;
  }
  dev->context->End(dev->query);
  BOOL done = FALSE;
  while (dev->context->GetData(dev->query, &done, sizeof(done), 0) == S_FALSE)
    SwitchToThread();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
HANDLE WINAPI shimDXOpenDevice(void *dxDevice)
{
  QOpenGLContext *glContext = QOpenGLContext::currentContext();
  ID3D11Device *d3d = nullptr;
  if (!dxDevice || !glContext ||
      FAILED(static_cast<IUnknown *>(dxDevice)->QueryInterface(IID_PPV_ARGS(&d3d))))
  {
    SetLastError(ERROR_NOT_SUPPORTED);
    return nullptr;
  }

  auto dev = new Device;
  dev->device = d3d;
  d3d->GetImmediateContext(&dev->context);
  if (!resolveGL(glContext, dev->gl))
  {
    qWarning() << "DX interop shim: GL_EXT_memory_object_win32 entry points missing";
    destroyDevice(dev);
    SetLastError(ERROR_NOT_SUPPORTED);
    return nullptr;
  }
  return dev;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Qt closes the device from a static destructor while the process exits, after the GL
// context is gone and the GPU driver has shut down: destroying the D3D11 device then
// crashes in the driver. So everything, including our reference that keeps the device
// alive, is left to process teardown.
BOOL WINAPI shimDXCloseDevice(HANDLE hDevice)
{
  if (!toDevice(hDevice))
  {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  return TRUE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
HANDLE WINAPI shimDXRegisterObject(HANDLE hDevice, void *dxObject, GLuint name, GLenum type,
                                   GLenum /*access*/)
{
  Device *dev = toDevice(hDevice);
  QOpenGLContext *glContext = QOpenGLContext::currentContext();
  ID3D11Texture2D *source = nullptr;
  if (!dev || !glContext || type != GL_TEXTURE_2D || !dxObject ||
      FAILED(static_cast<IUnknown *>(dxObject)->QueryInterface(IID_PPV_ARGS(&source))))
  {
    SetLastError(ERROR_INVALID_DATA);
    return nullptr;
  }

  D3D11_TEXTURE2D_DESC desc;
  source->GetDesc(&desc);
  static bool logged = false;
  if (!logged)
    qDebug() << "DX interop shim: sharing" << desc.Width << "x" << desc.Height
             << "textures, format" << desc.Format;
  logged = true;

  GLenum internalFormat = glFormatFor(desc.Format);
  if (!internalFormat || desc.SampleDesc.Count != 1)
  {
    static bool warned = false;
    if (!warned)
      qWarning() << "DX interop shim: unsupported texture, format" << desc.Format << "samples"
                 << desc.SampleDesc.Count;
    warned = true;
    source->Release();
    SetLastError(ERROR_INVALID_DATA);
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(dev->mutex);
  QOpenGLFunctions *f = glContext->functions();
  PoolEntry *entry = acquireEntry(dev, f, desc.Width, desc.Height, desc.Format);
  if (!entry)
  {
    source->Release();
    SetLastError(ERROR_OUTOFMEMORY);
    return nullptr;
  }

  clearGLErrors(f);
  GLint previous = 0;
  f->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
  f->glBindTexture(GL_TEXTURE_2D, name);
  dev->gl.TexStorageMem2DEXT(GL_TEXTURE_2D, 1, internalFormat, desc.Width, desc.Height,
                             entry->memory, 0);
  GLenum error = f->glGetError();
  f->glBindTexture(GL_TEXTURE_2D, GLuint(previous));
  if (error != GL_NO_ERROR)
  {
    static bool warned = false;
    if (!warned)
      qWarning() << "DX interop shim: glTexStorageMem2DEXT failed" << Qt::hex << error << Qt::dec
                 << "format" << desc.Format;
    warned = true;
    source->Release();
    SetLastError(ERROR_INVALID_DATA);
    return nullptr;
  }

  entry->source = source;
  return entry;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// No GL calls here: Qt unregisters before making its context current.
BOOL WINAPI shimDXUnregisterObject(HANDLE hDevice, HANDLE hObject)
{
  Device *dev = toDevice(hDevice);
  auto entry = static_cast<PoolEntry *>(hObject);
  if (!dev || !entry)
  {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }

  std::lock_guard<std::mutex> lock(dev->mutex);
  if (entry->source)
  {
    entry->source->Release();
    entry->source = nullptr;
  }
  entry->lastRelease = ++dev->releases;
  return TRUE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Publishes the registered textures' current contents to GL, like the real extension.
BOOL WINAPI shimDXLockObjects(HANDLE hDevice, GLint count, HANDLE *objects)
{
  Device *dev = toDevice(hDevice);
  if (!dev || count < 0 || (count && !objects))
  {
    SetLastError(ERROR_INVALID_DATA);
    return FALSE;
  }

  std::lock_guard<std::mutex> lock(dev->mutex);
  for (GLint i = 0; i < count; i++)
  {
    auto entry = static_cast<PoolEntry *>(objects[i]);
    if (entry && entry->source)
      dev->context->CopySubresourceRegion(entry->texture, 0, 0, 0, 0, entry->source, 0, nullptr);
  }
  waitForCopies(dev);
  return TRUE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Nothing to do: GL can't signal D3D11 when it's done (see the top of the file).
BOOL WINAPI shimDXUnlockObjects(HANDLE hDevice, GLint count, HANDLE *objects)
{
  if (!toDevice(hDevice) || count < 0 || (count && !objects))
  {
    SetLastError(ERROR_INVALID_DATA);
    return FALSE;
  }
  return TRUE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
FARPROC WINAPI hookedGetProcAddress(HMODULE module, LPCSTR name)
{
  FARPROC proc = g_realGetProcAddress(module, name);
  // Only fill in the interop functions the GL library lacks (Qt looks them up there when
  // wglGetProcAddress fails). Ordinal lookups have no name to compare.
  if (proc || IS_INTRESOURCE(name) || strncmp(name, "wglDX", 5) != 0)
    return proc;

  static const struct { const char *name; FARPROC proc; } shims[] = {
    { "wglDXOpenDeviceNV", reinterpret_cast<FARPROC>(shimDXOpenDevice) },
    { "wglDXCloseDeviceNV", reinterpret_cast<FARPROC>(shimDXCloseDevice) },
    { "wglDXRegisterObjectNV", reinterpret_cast<FARPROC>(shimDXRegisterObject) },
    { "wglDXUnregisterObjectNV", reinterpret_cast<FARPROC>(shimDXUnregisterObject) },
    { "wglDXLockObjectsNV", reinterpret_cast<FARPROC>(shimDXLockObjects) },
    { "wglDXUnlockObjectsNV", reinterpret_cast<FARPROC>(shimDXUnlockObjects) },
  };
  for (const auto &shim : shims)
  {
    if (strcmp(name, shim.name) == 0)
      return shim.proc;
  }
  return nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// GetProcAddress comes from kernel32 or, when linked against umbrella libraries, an API set
bool isLoaderLibrary(const char *name)
{
  static const char apiSet[] = "api-ms-win-core-libraryloader-";
  return _stricmp(name, "kernel32.dll") == 0 || _strnicmp(name, apiSet, sizeof(apiSet) - 1) == 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Point the platform plugin's import of GetProcAddress at our hook. Qt resolves WGL
// extension functions with wglGetProcAddress and falls back to GetProcAddress on the GL
// library, which is where the hook answers for the missing wglDX* functions.
bool patchImport(HMODULE module)
{
  auto base = reinterpret_cast<BYTE *>(module);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress)
    return false;

  auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
  for (; desc->Name; desc++)
  {
    auto library = reinterpret_cast<const char *>(base + desc->Name);
    if (!isLoaderLibrary(library) || !desc->OriginalFirstThunk)
      continue;

    auto names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + desc->OriginalFirstThunk);
    auto thunks = reinterpret_cast<IMAGE_THUNK_DATA *>(base + desc->FirstThunk);
    for (; names->u1.AddressOfData; names++, thunks++)
    {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
        continue;
      auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char *>(byName->Name), "GetProcAddress") != 0)
        continue;

      auto function = &thunks->u1.Function;
      DWORD oldProtect;
      if (!VirtualProtect(function, sizeof(*function), PAGE_READWRITE, &oldProtect))
        return false;
      *function = reinterpret_cast<ULONG_PTR>(&hookedGetProcAddress);
      VirtualProtect(function, sizeof(*function), oldProtect, &oldProtect);
      return true;
    }
  }
  return false;
}

} // namespace

///////////////////////////////////////////////////////////////////////////////////////////////////
bool installDXInteropShim()
{
  g_realGetProcAddress = &GetProcAddress;
  // Debug builds of Qt name the plugin with a d suffix
  for (const wchar_t *name : { L"qwindows.dll", L"qwindowsd.dll" })
  {
    HMODULE platform = GetModuleHandleW(name);
    if (platform && patchImport(platform))
    {
      qInfo() << "DX interop shim: emulating WGL_NV_DX_interop with GL_EXT_memory_object_win32";
      return true;
    }
  }
  qWarning() << "DX interop shim: could not hook the qwindows platform plugin";
  return false;
}
