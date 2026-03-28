# Hello World CEF Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Create a minimal CEF application that renders "Hello World" HTML to an SDL2 window using off-screen rendering and OpenGL.

**Architecture:** SDL2 creates window and OpenGL context. CEF runs in OSR mode, painting to a buffer. OpenGL uploads buffer to texture and renders fullscreen quad. Main loop pumps both SDL and CEF message loops.

**Tech Stack:** C++17, CMake, SDL2, CEF (OSR mode), OpenGL 3.3

**Prerequisites:** CEF binary distribution extracted to `third_party/cef/`

---

### Task 1: CMake Foundation

**Files:**
- Create: `CMakeLists.txt`
- Create: `cmake/FindCEF.cmake`

**Step 1: Create root CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.19)
project(jellyfin-desktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CEF configuration
set(CEF_ROOT "${CMAKE_SOURCE_DIR}/third_party/cef" CACHE PATH "CEF binary distribution root")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(CEF REQUIRED)

# SDL2
find_package(SDL2 REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

add_executable(jellyfin-desktop
    src/main.cpp
)

target_include_directories(jellyfin-desktop PRIVATE
    ${CEF_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(jellyfin-desktop PRIVATE
    ${CEF_LIBRARIES}
    ${SDL2_LIBRARIES}
    OpenGL::GL
)

# Copy CEF resources to build directory
add_custom_command(TARGET jellyfin-desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_ROOT}/Resources"
        "$<TARGET_FILE_DIR:jellyfin-desktop>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_ROOT}/Release"
        "$<TARGET_FILE_DIR:jellyfin-desktop>"
)
```

**Step 2: Create cmake/FindCEF.cmake**

```cmake
# FindCEF.cmake - Find CEF binary distribution

if(NOT CEF_ROOT)
    message(FATAL_ERROR "CEF_ROOT not set. Download CEF from https://cef-builds.spotifycdn.com/index.html and extract to third_party/cef/")
endif()

if(NOT EXISTS "${CEF_ROOT}/include/cef_version.h")
    message(FATAL_ERROR "CEF not found at ${CEF_ROOT}. Ensure CEF binary distribution is extracted there.")
endif()

# Read CEF version
file(READ "${CEF_ROOT}/include/cef_version.h" CEF_VERSION_CONTENT)
string(REGEX MATCH "CEF_VERSION \"([^\"]+)\"" _ ${CEF_VERSION_CONTENT})
set(CEF_VERSION ${CMAKE_MATCH_1})
message(STATUS "Found CEF: ${CEF_VERSION}")

set(CEF_INCLUDE_DIRS "${CEF_ROOT}" "${CEF_ROOT}/include")

# Platform-specific library setup
if(WIN32)
    set(CEF_LIB_DIR "${CEF_ROOT}/Release")
    set(CEF_LIBRARIES
        "${CEF_LIB_DIR}/libcef.lib"
        "${CEF_ROOT}/libcef_dll_wrapper/Release/libcef_dll_wrapper.lib"
    )
elseif(APPLE)
    set(CEF_LIBRARIES
        "${CEF_ROOT}/Release/Chromium Embedded Framework.framework"
        "${CEF_ROOT}/libcef_dll_wrapper/libcef_dll_wrapper.a"
    )
else() # Linux
    set(CEF_LIB_DIR "${CEF_ROOT}/Release")
    set(CEF_LIBRARIES
        "${CEF_LIB_DIR}/libcef.so"
        "${CEF_ROOT}/libcef_dll_wrapper/libcef_dll_wrapper.a"
    )
endif()

# Check for wrapper library - must be built first
if(NOT EXISTS "${CEF_ROOT}/libcef_dll_wrapper")
    message(WARNING "libcef_dll_wrapper not found. You may need to build it first:")
    message(WARNING "  cd ${CEF_ROOT} && cmake -B build && cmake --build build --target libcef_dll_wrapper")
endif()

set(CEF_FOUND TRUE)
```

**Step 3: Create placeholder main.cpp**

```cpp
// src/main.cpp
#include <iostream>

int main(int argc, char* argv[]) {
    std::cout << "Hello from jellyfin-desktop" << std::endl;
    return 0;
}
```

**Step 4: Create src directory and file**

Run:
```bash
mkdir -p src
```

**Step 5: Test CMake configuration (without CEF)**

Run:
```bash
cmake -B build 2>&1 | head -20
```

Expected: Error about CEF not found (confirms CMake logic works)

**Step 6: Commit**

```bash
git add CMakeLists.txt cmake/ src/main.cpp
git commit -m "feat: add CMake build system foundation"
```

---

### Task 2: Build CEF Wrapper Library

**Context:** CEF binary distribution requires building `libcef_dll_wrapper` before use.

**Step 1: Document CEF setup in README**

Create: `README.md`

```markdown
# Jellyfin Desktop

Minimal CEF application with SDL2 and OpenGL.

## Prerequisites

- CMake 3.19+
- SDL2 development libraries
- OpenGL development libraries
- C++17 compiler

## CEF Setup

1. Download CEF binary distribution from https://cef-builds.spotifycdn.com/index.html
   - Choose "Standard Distribution" for your platform
   - Recommended: Latest stable branch

2. Extract to `third_party/cef/`

3. Build the CEF wrapper library:
   ```bash
   cd third_party/cef
   cmake -B build
   cmake --build build --target libcef_dll_wrapper
   ```

## Build

```bash
cmake -B build
cmake --build build
```

## Run

```bash
./build/jellyfin-desktop
```
```

**Step 2: Add third_party to gitignore**

Create/update: `.gitignore`

```
# Build
build/

# CEF binary distribution
third_party/cef/

# IDE
.vscode/
.idea/
*.swp
compile_commands.json
```

**Step 3: Commit**

```bash
git add README.md .gitignore
git commit -m "docs: add README with CEF setup instructions"
```

---

### Task 3: SDL2 Window Creation

**Files:**
- Modify: `src/main.cpp`

**Step 1: Implement SDL2 window with OpenGL context**

Replace `src/main.cpp`:

```cpp
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <iostream>

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    // Request OpenGL 3.3 Core
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    SDL_Window* window = SDL_CreateWindow(
        "Jellyfin Desktop",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
    );

    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // VSync
    SDL_GL_SetSwapInterval(1);

    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;

    // Main loop
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                running = false;
            }
        }

        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        SDL_GL_SwapWindow(window);
    }

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
```

**Step 2: Update CMakeLists.txt for SDL2-only build**

Temporarily comment out CEF requirement for testing SDL2 in isolation. Modify `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.19)
project(jellyfin-desktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SDL2
find_package(SDL2 REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

add_executable(jellyfin-desktop
    src/main.cpp
)

target_include_directories(jellyfin-desktop PRIVATE
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(jellyfin-desktop PRIVATE
    ${SDL2_LIBRARIES}
    OpenGL::GL
)
```

**Step 3: Build and run**

Run:
```bash
cmake -B build && cmake --build build && ./build/jellyfin-desktop
```

Expected: Window appears with teal background. ESC or close button exits.

**Step 4: Commit**

```bash
git add src/main.cpp CMakeLists.txt
git commit -m "feat: add SDL2 window with OpenGL context"
```

---

### Task 4: OpenGL Texture Renderer

**Files:**
- Create: `src/renderer.h`
- Create: `src/renderer.cpp`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`

**Step 1: Create renderer header**

Create `src/renderer.h`:

```cpp
#pragma once

#include <SDL2/SDL_opengl.h>
#include <cstdint>

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(int width, int height);
    void updateTexture(const void* buffer, int width, int height);
    void render();

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint texture_ = 0;
    GLuint shader_program_ = 0;
    int tex_width_ = 0;
    int tex_height_ = 0;

    GLuint compileShader(GLenum type, const char* source);
    GLuint createShaderProgram();
};
```

**Step 2: Create renderer implementation**

Create `src/renderer.cpp`:

```cpp
#include "renderer.h"
#include <iostream>

static const char* vertex_shader_source = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

static const char* fragment_shader_source = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;
uniform sampler2D textureSampler;
void main() {
    FragColor = texture(textureSampler, TexCoord);
}
)";

Renderer::Renderer() = default;

Renderer::~Renderer() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (texture_) glDeleteTextures(1, &texture_);
    if (shader_program_) glDeleteProgram(shader_program_);
}

bool Renderer::init(int width, int height) {
    tex_width_ = width;
    tex_height_ = height;

    shader_program_ = createShaderProgram();
    if (!shader_program_) return false;

    // Fullscreen quad (position + texcoord)
    float vertices[] = {
        // pos      // tex
        -1.0f,  1.0f,  0.0f, 0.0f,  // top-left
        -1.0f, -1.0f,  0.0f, 1.0f,  // bottom-left
         1.0f, -1.0f,  1.0f, 1.0f,  // bottom-right
        -1.0f,  1.0f,  0.0f, 0.0f,  // top-left
         1.0f, -1.0f,  1.0f, 1.0f,  // bottom-right
         1.0f,  1.0f,  1.0f, 0.0f,  // top-right
    };

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Create texture
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);

    glBindVertexArray(0);
    return true;
}

void Renderer::updateTexture(const void* buffer, int width, int height) {
    glBindTexture(GL_TEXTURE_2D, texture_);
    if (width != tex_width_ || height != tex_height_) {
        tex_width_ = width;
        tex_height_ = height;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, buffer);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, buffer);
    }
}

void Renderer::render() {
    glUseProgram(shader_program_);
    glBindVertexArray(vao_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

GLuint Renderer::compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "Shader compile error: " << log << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint Renderer::createShaderProgram() {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragment_shader_source);
    if (!vs || !fs) return 0;

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::cerr << "Shader link error: " << log << std::endl;
        glDeleteProgram(program);
        return 0;
    }
    return program;
}
```

**Step 3: Update CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.19)
project(jellyfin-desktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SDL2
find_package(SDL2 REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

add_executable(jellyfin-desktop
    src/main.cpp
    src/renderer.cpp
)

target_include_directories(jellyfin-desktop PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(jellyfin-desktop PRIVATE
    ${SDL2_LIBRARIES}
    OpenGL::GL
)
```

**Step 4: Update main.cpp to test renderer**

Modify `src/main.cpp` to create test pattern:

```cpp
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <iostream>
#include <vector>
#include "renderer.h"

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    const int width = 1280;
    const int height = 720;

    SDL_Window* window = SDL_CreateWindow(
        "Jellyfin Desktop",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
    );

    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_SetSwapInterval(1);

    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;

    Renderer renderer;
    if (!renderer.init(width, height)) {
        std::cerr << "Renderer init failed" << std::endl;
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Create test pattern (gradient)
    std::vector<uint8_t> test_buffer(width * height * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int i = (y * width + x) * 4;
            test_buffer[i + 0] = (uint8_t)(255 * x / width);  // B
            test_buffer[i + 1] = (uint8_t)(255 * y / height); // G
            test_buffer[i + 2] = 128;                          // R
            test_buffer[i + 3] = 255;                          // A
        }
    }
    renderer.updateTexture(test_buffer.data(), width, height);

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        glClear(GL_COLOR_BUFFER_BIT);
        renderer.render();
        SDL_GL_SwapWindow(window);
    }

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
```

**Step 5: Build and test**

Run:
```bash
cmake -B build && cmake --build build && ./build/jellyfin-desktop
```

Expected: Window shows gradient pattern (purple-ish with blue increasing left-to-right, green increasing top-to-bottom)

**Step 6: Commit**

```bash
git add src/renderer.h src/renderer.cpp src/main.cpp CMakeLists.txt
git commit -m "feat: add OpenGL texture renderer"
```

---

### Task 5: CEF App and Client Skeleton

**Files:**
- Create: `src/cef_app.h`
- Create: `src/cef_app.cpp`
- Create: `src/cef_client.h`
- Create: `src/cef_client.cpp`
- Modify: `CMakeLists.txt`

**Step 1: Create CEF app header**

Create `src/cef_app.h`:

```cpp
#pragma once

#include "include/cef_app.h"

class App : public CefApp, public CefBrowserProcessHandler {
public:
    App() = default;

    // CefApp
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    // CefBrowserProcessHandler
    void OnContextInitialized() override;

private:
    IMPLEMENT_REFCOUNTING(App);
    DISALLOW_COPY_AND_ASSIGN(App);
};
```

**Step 2: Create CEF app implementation**

Create `src/cef_app.cpp`:

```cpp
#include "cef_app.h"
#include <iostream>

void App::OnContextInitialized() {
    std::cout << "CEF context initialized" << std::endl;
}
```

**Step 3: Create CEF client header**

Create `src/cef_client.h`:

```cpp
#pragma once

#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include <functional>

class Client : public CefClient, public CefRenderHandler, public CefLifeSpanHandler {
public:
    using PaintCallback = std::function<void(const void* buffer, int width, int height)>;

    Client(int width, int height, PaintCallback on_paint);

    // CefClient
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                 const RectList& dirtyRects, const void* buffer,
                 int width, int height) override;

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    bool isClosed() const { return is_closed_; }

private:
    int width_;
    int height_;
    PaintCallback on_paint_;
    bool is_closed_ = false;

    IMPLEMENT_REFCOUNTING(Client);
    DISALLOW_COPY_AND_ASSIGN(Client);
};
```

**Step 4: Create CEF client implementation**

Create `src/cef_client.cpp`:

```cpp
#include "cef_client.h"
#include <iostream>

Client::Client(int width, int height, PaintCallback on_paint)
    : width_(width), height_(height), on_paint_(std::move(on_paint)) {}

void Client::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    rect.Set(0, 0, width_, height_);
}

void Client::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                     const RectList& dirtyRects, const void* buffer,
                     int width, int height) {
    if (on_paint_) {
        on_paint_(buffer, width, height);
    }
}

void Client::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    std::cout << "Browser created" << std::endl;
}

void Client::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    std::cout << "Browser closing" << std::endl;
    is_closed_ = true;
}
```

**Step 5: Update CMakeLists.txt with CEF**

```cmake
cmake_minimum_required(VERSION 3.19)
project(jellyfin-desktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CEF configuration
set(CEF_ROOT "${CMAKE_SOURCE_DIR}/third_party/cef" CACHE PATH "CEF binary distribution root")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(CEF REQUIRED)

# SDL2
find_package(SDL2 REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

add_executable(jellyfin-desktop
    src/main.cpp
    src/renderer.cpp
    src/cef_app.cpp
    src/cef_client.cpp
)

target_include_directories(jellyfin-desktop PRIVATE
    ${CEF_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(jellyfin-desktop PRIVATE
    ${CEF_LIBRARIES}
    ${SDL2_LIBRARIES}
    OpenGL::GL
)

# Copy CEF resources to build directory
add_custom_command(TARGET jellyfin-desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_ROOT}/Resources"
        "$<TARGET_FILE_DIR:jellyfin-desktop>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_ROOT}/Release"
        "$<TARGET_FILE_DIR:jellyfin-desktop>"
)
```

**Step 6: Verify compilation (requires CEF)**

Run:
```bash
cmake -B build && cmake --build build
```

Expected: Compiles successfully (CEF must be set up per README)

**Step 7: Commit**

```bash
git add src/cef_app.h src/cef_app.cpp src/cef_client.h src/cef_client.cpp CMakeLists.txt
git commit -m "feat: add CEF app and client skeleton"
```

---

### Task 6: Integrate CEF with Main Loop

**Files:**
- Modify: `src/main.cpp`
- Create: `resources/index.html`

**Step 1: Create hello world HTML**

Create `resources/index.html`:

```html
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <style>
        body {
            margin: 0;
            display: flex;
            justify-content: center;
            align-items: center;
            height: 100vh;
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
        }
        h1 {
            color: #00d9ff;
            font-size: 4rem;
            text-shadow: 0 0 20px rgba(0, 217, 255, 0.5);
        }
    </style>
</head>
<body>
    <h1>Hello, Jellyfin!</h1>
</body>
</html>
```

**Step 2: Update main.cpp with full CEF integration**

Replace `src/main.cpp`:

```cpp
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <iostream>
#include <filesystem>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"

#include "renderer.h"
#include "cef_app.h"
#include "cef_client.h"

int main(int argc, char* argv[]) {
    // CEF initialization
    CefMainArgs main_args(argc, argv);

    CefRefPtr<App> app(new App());

    // Check if this is a subprocess
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }

    // SDL initialization
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    const int width = 1280;
    const int height = 720;

    SDL_Window* window = SDL_CreateWindow(
        "Jellyfin Desktop",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
    );

    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_SetSwapInterval(1);
    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;

    // Initialize renderer
    Renderer renderer;
    if (!renderer.init(width, height)) {
        std::cerr << "Renderer init failed" << std::endl;
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // CEF settings
    CefSettings settings;
    settings.windowless_rendering_enabled = true;
    settings.no_sandbox = true;  // Sandbox requires separate executable

    // Get executable path for resources
    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe").parent_path();
    CefString(&settings.resources_dir_path).FromString(exe_path.string());
    CefString(&settings.locales_dir_path).FromString((exe_path / "locales").string());

    if (!CefInitialize(main_args, settings, app, nullptr)) {
        std::cerr << "CefInitialize failed" << std::endl;
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Create browser
    bool texture_dirty = false;
    const void* paint_buffer = nullptr;
    int paint_width = 0, paint_height = 0;

    CefRefPtr<Client> client(new Client(width, height,
        [&](const void* buffer, int w, int h) {
            paint_buffer = buffer;
            paint_width = w;
            paint_height = h;
            texture_dirty = true;
        }
    ));

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);

    CefBrowserSettings browser_settings;

    // Load local HTML file
    std::string html_path = "file://" + (exe_path.parent_path() / "resources" / "index.html").string();
    std::cout << "Loading: " << html_path << std::endl;

    CefBrowserHost::CreateBrowser(window_info, client, html_path, browser_settings, nullptr, nullptr);

    // Main loop
    bool running = true;
    while (running && !client->isClosed()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        CefDoMessageLoopWork();

        if (texture_dirty && paint_buffer) {
            renderer.updateTexture(paint_buffer, paint_width, paint_height);
            texture_dirty = false;
        }

        glClear(GL_COLOR_BUFFER_BIT);
        renderer.render();
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    CefShutdown();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
```

**Step 3: Update CMakeLists.txt to copy resources**

Add to `CMakeLists.txt` after existing post-build commands:

```cmake
# Copy application resources
add_custom_command(TARGET jellyfin-desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:jellyfin-desktop>/../resources"
)
```

**Step 4: Build and test**

Run:
```bash
cmake -B build && cmake --build build && ./build/jellyfin-desktop
```

Expected: Window shows "Hello, Jellyfin!" with dark gradient background and cyan text

**Step 5: Commit**

```bash
git add src/main.cpp resources/index.html CMakeLists.txt
git commit -m "feat: integrate CEF OSR with main loop

Loads local HTML file and renders via OpenGL texture."
```

---

### Task 7: Platform-Specific Fixes (Linux)

**Files:**
- Modify: `src/main.cpp` (if needed)
- Modify: `CMakeLists.txt` (if needed)

**Step 1: Test on Linux**

Run and verify no crashes, proper shutdown.

**Step 2: Add subprocess handling note**

CEF spawns subprocesses. On Linux, the main executable serves as subprocess. Verify `CefExecuteProcess` early return works.

**Step 3: Commit any fixes**

```bash
git add -A
git commit -m "fix: platform-specific adjustments for Linux"
```

---

## Summary

After completing all tasks:
- SDL2 window with OpenGL 3.3 context
- CEF in off-screen rendering mode
- HTML renders to texture
- OpenGL displays texture as fullscreen quad
- Clean shutdown of both CEF and SDL2

Foundation ready for:
- Input handling (Task 8+)
- libmpv integration
- Vulkan migration
