#ifndef OPENGLDETECT_H
#define OPENGLDETECT_H

void detectOpenGLEarly();
void detectOpenGLLate();

#ifdef _WIN32
// Whether the OpenGL driver has WGL_NV_DX_interop, which QtWebEngine's GPU compositing needs
bool hasOpenGLDXInterop();
#endif

#endif // OPENGLDETECT_H
