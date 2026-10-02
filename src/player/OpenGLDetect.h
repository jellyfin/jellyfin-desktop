#ifndef OPENGLDETECT_H
#define OPENGLDETECT_H

void detectOpenGLEarly();
void detectOpenGLLate();

#ifdef _WIN32
// WGL_NV_DX_interop support, which QtWebEngine's GPU compositing needs
enum class DXInterop
{
  Native,      // provided by the driver
  Emulated,    // missing, but DXInteropShim can provide it
  Unsupported  // missing, QtWebEngine must composite in software
};
DXInterop openGLDXInterop();
#endif

#endif // OPENGLDETECT_H
