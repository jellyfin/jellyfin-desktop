#ifndef OPENGLDETECT_H
#define OPENGLDETECT_H

void detectOpenGLEarly();
void detectOpenGLLate();

#ifdef _WIN32
#include <QString>

// Whether the OpenGL driver leaks every texture registered with WGL_NV_DX_interop, which
// QtWebEngine's GPU compositing does for each frame. The result is cached in cachePath.
bool dxInteropLeaksTextures(const QString& cachePath);
#endif

#endif // OPENGLDETECT_H
