#include "platform/wayland_browser_surface.h"
#include <SDL3/SDL.h>
#include "logging.h"
#include <cstring>
#include <unistd.h>
#include <drm_fourcc.h>
#include "wayland-protocols/viewporter-client.h"
#include "wayland-protocols/linux-dmabuf-unstable-v1-client.h"

static const wl_registry_listener s_registryListener = {
    .global = WaylandBrowserSurface::registryGlobal,
    .global_remove = WaylandBrowserSurface::registryGlobalRemove,
};

static const wl_buffer_listener s_bufferListener = {
    .release = WaylandBrowserSurface::bufferRelease,
};

WaylandBrowserSurface::WaylandBrowserSurface() = default;
WaylandBrowserSurface::~WaylandBrowserSurface() { cleanup(); }

void WaylandBrowserSurface::registryGlobal(void* data, wl_registry* registry,
                                            uint32_t name, const char* interface, uint32_t version) {
    auto* self = static_cast<WaylandBrowserSurface*>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        self->wl_compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        self->wl_subcompositor_ = static_cast<wl_subcompositor*>(
            wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        self->viewporter_ = static_cast<wp_viewporter*>(
            wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        self->dmabuf_manager_ = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, std::min(version, 3u)));
    }
}

bool WaylandBrowserSurface::init(SDL_Window* window) {
    if (!initWayland(window)) return false;

    if (!dmabuf_manager_) {
        LOG_ERROR(LOG_PLATFORM, "WaylandBrowserSurface: zwp_linux_dmabuf_v1 not available");
        return false;
    }

    LOG_INFO(LOG_PLATFORM, "WaylandBrowserSurface: initialized (direct dmabuf attach)");
    return true;
}

bool WaylandBrowserSurface::initWayland(SDL_Window* window) {
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (!props) return false;

    wl_display_ = static_cast<wl_display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    wl_surface* parent = static_cast<wl_surface*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    if (!wl_display_ || !parent) return false;

    wl_registry* reg = wl_display_get_registry(wl_display_);
    wl_registry_add_listener(reg, &s_registryListener, this);
    wl_display_roundtrip(wl_display_);
    wl_registry_destroy(reg);

    if (!wl_compositor_ || !wl_subcompositor_) return false;
    return createSubsurface(parent);
}

bool WaylandBrowserSurface::createSubsurface(wl_surface* parent) {
    surface_ = wl_compositor_create_surface(wl_compositor_);
    if (!surface_) return false;

    subsurface_ = wl_subcompositor_get_subsurface(wl_subcompositor_, surface_, parent);
    if (!subsurface_) return false;

    // Place ABOVE parent (CEF overlay is on top of everything)
    wl_subsurface_set_position(subsurface_, 0, 0);
    wl_subsurface_place_above(subsurface_, parent);
    wl_subsurface_set_desync(subsurface_);

    // Pass-through input to parent
    wl_region* empty = wl_compositor_create_region(wl_compositor_);
    wl_surface_set_input_region(surface_, empty);
    wl_region_destroy(empty);

    if (viewporter_)
        viewport_ = wp_viewporter_get_viewport(viewporter_, surface_);

    wl_surface_commit(surface_);
    wl_display_roundtrip(wl_display_);
    return true;
}

void WaylandBrowserSurface::queueDmabuf(int fd, uint32_t stride, uint64_t modifier, int width, int height) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    // Close previously queued but uncommitted fd
    if (pending_.load(std::memory_order_relaxed) && queued_.fd >= 0) {
        close(queued_.fd);
    }
    queued_.fd = fd;
    queued_.stride = stride;
    queued_.modifier = modifier;
    queued_.width = width;
    queued_.height = height;
    pending_.store(true, std::memory_order_release);
}

void WaylandBrowserSurface::bufferRelease(void* data, wl_buffer* buffer) {
    auto* self = static_cast<WaylandBrowserSurface*>(data);
    if (self->prev_buffer_ == buffer) {
        wl_buffer_destroy(self->prev_buffer_);
        self->prev_buffer_ = nullptr;
        if (self->prev_fd_ >= 0) {
            close(self->prev_fd_);
            self->prev_fd_ = -1;
        }
    } else if (self->current_buffer_ == buffer) {
        wl_buffer_destroy(self->current_buffer_);
        self->current_buffer_ = nullptr;
        if (self->current_fd_ >= 0) {
            close(self->current_fd_);
            self->current_fd_ = -1;
        }
    }
}

bool WaylandBrowserSurface::commitQueued() {
    if (!pending_.load(std::memory_order_acquire)) return false;
    if (!dmabuf_manager_ || !surface_) return false;

    int fd;
    uint32_t stride;
    uint64_t modifier;
    int w, h;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!pending_.load(std::memory_order_relaxed)) return false;
        fd = queued_.fd;
        stride = queued_.stride;
        modifier = queued_.modifier;
        w = queued_.width;
        h = queued_.height;
        pending_.store(false, std::memory_order_relaxed);
        queued_.fd = -1;
    }
    if (fd < 0) return false;

    // Create wl_buffer from dmabuf
    struct zwp_linux_buffer_params_v1* params =
        zwp_linux_dmabuf_v1_create_params(dmabuf_manager_);

    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride,
                                    modifier >> 32, modifier & 0xFFFFFFFF);

    wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(
        params, w, h, DRM_FORMAT_ARGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(params);

    if (!buffer) {
        LOG_ERROR(LOG_PLATFORM, "WaylandBrowserSurface: create_immed failed (%dx%d stride=%u mod=0x%lx)",
                  w, h, stride, modifier);
        close(fd);
        return false;
    }

    // Track previous buffer for release
    if (prev_buffer_) {
        // Previous buffer not yet released by compositor — destroy it now.
        // The compositor may have already released it via the listener,
        // in which case prev_buffer_ is already nullptr.
        wl_buffer_destroy(prev_buffer_);
        if (prev_fd_ >= 0) close(prev_fd_);
    }
    prev_buffer_ = current_buffer_;
    prev_fd_ = current_fd_;

    current_buffer_ = buffer;
    current_fd_ = fd;

    // Listen for release so we can clean up
    wl_buffer_add_listener(buffer, &s_bufferListener, this);

    // Attach and commit
    wl_surface_attach(surface_, buffer, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, w, h);
    wl_surface_commit(surface_);

    return true;
}

void WaylandBrowserSurface::setDestinationSize(int w, int h) {
    if (viewport_ && w > 0 && h > 0)
        wp_viewport_set_destination(viewport_, w, h);
}

void WaylandBrowserSurface::hide() {
    if (!surface_) return;
    wl_surface_attach(surface_, nullptr, 0, 0);
    wl_surface_commit(surface_);
    wl_display_flush(wl_display_);
}

void WaylandBrowserSurface::cleanup() {
    if (current_buffer_) { wl_buffer_destroy(current_buffer_); current_buffer_ = nullptr; }
    if (current_fd_ >= 0) { close(current_fd_); current_fd_ = -1; }
    if (prev_buffer_) { wl_buffer_destroy(prev_buffer_); prev_buffer_ = nullptr; }
    if (prev_fd_ >= 0) { close(prev_fd_); prev_fd_ = -1; }
    // Close any pending queued fd
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queued_.fd >= 0) { close(queued_.fd); queued_.fd = -1; }
    }
    if (viewport_) { wp_viewport_destroy(viewport_); viewport_ = nullptr; }
    if (viewporter_) { wp_viewporter_destroy(viewporter_); viewporter_ = nullptr; }
    if (subsurface_) { wl_subsurface_destroy(subsurface_); subsurface_ = nullptr; }
    if (surface_) { wl_surface_destroy(surface_); surface_ = nullptr; }
    if (dmabuf_manager_) { zwp_linux_dmabuf_v1_destroy(dmabuf_manager_); dmabuf_manager_ = nullptr; }
    wl_compositor_ = nullptr;
    wl_subcompositor_ = nullptr;
    wl_display_ = nullptr;
}
