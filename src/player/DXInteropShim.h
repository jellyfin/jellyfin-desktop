#ifndef DXINTEROPSHIM_H
#define DXINTEROPSHIM_H

// Emulates WGL_NV_DX_interop on top of GL_EXT_memory_object_win32 for OpenGL drivers
// that lack it (e.g. Microsoft's OpenGLOn12 mapping layer on Windows on ARM), so
// QtWebEngine can keep compositing on the GPU. Must be called after the
// QGuiApplication is created (the qwindows platform plugin must be loaded) and before
// QtWebEngine renders. Returns false if the shim couldn't be installed.
bool installDXInteropShim();

#endif // DXINTEROPSHIM_H
