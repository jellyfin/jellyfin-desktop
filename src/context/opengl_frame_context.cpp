#include "opengl_frame_context.h"

#ifdef _WIN32
#include "wgl_context.h"
#include <GL/gl.h>
#else
#include "egl_context.h"
#endif

OpenGLFrameContext::OpenGLFrameContext(GLContext* gl) : gl_(gl) {}

void OpenGLFrameContext::beginFrame(float bg_color, float alpha) {
#ifdef _WIN32
    gl_->makeCurrent();  // Ensure WGL context is current each frame
    // DWM per-pixel alpha requires premultiplied values:
    // RGB must be pre-multiplied by alpha, so transparent clear = (0,0,0,0)
    float premul_color = bg_color * alpha;
    glClearColor(premul_color, premul_color, premul_color, alpha);
#else
    glClearColor(bg_color, bg_color, bg_color, alpha);
#endif
    glClear(GL_COLOR_BUFFER_BIT);
}

void OpenGLFrameContext::endFrame() {
    gl_->swapBuffers();
}
