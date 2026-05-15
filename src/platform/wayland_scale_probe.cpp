#include "wayland_scale_probe.h"

#include <wayland-client.h>
#include "xdg-output-unstable-v1-client.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

struct OutputInfo {
    wl_output* output = nullptr;
    zxdg_output_v1* xdg_output = nullptr;
    int32_t x = 0, y = 0;            // logical position
    int32_t logical_w = 0, logical_h = 0;
    int32_t mode_w = 0, mode_h = 0;  // physical pixels

    ~OutputInfo() {
        if (xdg_output) zxdg_output_v1_destroy(xdg_output);
        if (output) wl_output_release(output);
    }
};

struct ProbeState {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    zxdg_output_manager_v1* xdg_output_manager = nullptr;
    std::vector<std::unique_ptr<OutputInfo>> outputs;

    ~ProbeState() {
        outputs.clear();
        if (xdg_output_manager) zxdg_output_manager_v1_destroy(xdg_output_manager);
        if (registry) wl_registry_destroy(registry);
        if (display) wl_display_disconnect(display);
    }
};

// wl_output listener — only `mode` carries data we use; the rest are
// required no-ops because Wayland mandates a full listener vtable.
void out_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t,
                  int32_t, const char*, const char*, int32_t) {}
void out_mode(void* data, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t) {
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    auto* o = static_cast<OutputInfo*>(data);
    o->mode_w = w;
    o->mode_h = h;
}
void out_done(void*, wl_output*) {}
void out_scale(void*, wl_output*, int32_t) {}
void out_name(void*, wl_output*, const char*) {}
void out_description(void*, wl_output*, const char*) {}
const wl_output_listener output_listener = {
    out_geometry, out_mode, out_done, out_scale, out_name, out_description,
};

void xdg_out_position(void* data, zxdg_output_v1*, int32_t x, int32_t y) {
    auto* o = static_cast<OutputInfo*>(data);
    o->x = x; o->y = y;
}
void xdg_out_logical_size(void* data, zxdg_output_v1*, int32_t w, int32_t h) {
    auto* o = static_cast<OutputInfo*>(data);
    o->logical_w = w; o->logical_h = h;
}
void xdg_out_done(void*, zxdg_output_v1*) {}
void xdg_out_name(void*, zxdg_output_v1*, const char*) {}
void xdg_out_description(void*, zxdg_output_v1*, const char*) {}
const zxdg_output_v1_listener xdg_output_listener = {
    xdg_out_position, xdg_out_logical_size, xdg_out_done,
    xdg_out_name, xdg_out_description,
};

void reg_global(void* data, wl_registry* reg, uint32_t name,
                const char* interface, uint32_t version) {
    auto* s = static_cast<ProbeState*>(data);
    if (!std::strcmp(interface, wl_output_interface.name)) {
        auto o = std::make_unique<OutputInfo>();
        uint32_t ver = version < 4 ? version : 4;
        o->output = static_cast<wl_output*>(
            wl_registry_bind(reg, name, &wl_output_interface, ver));
        wl_output_add_listener(o->output, &output_listener, o.get());
        s->outputs.push_back(std::move(o));
    } else if (!std::strcmp(interface, zxdg_output_manager_v1_interface.name)) {
        uint32_t ver = version < 3 ? version : 3;
        s->xdg_output_manager = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, ver));
    }
}
void reg_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {
    reg_global, reg_global_remove,
};

}

namespace wayland_scale_probe {

double query_scale(int probe_x, int probe_y) {
    if (!std::getenv("WAYLAND_DISPLAY") && !std::getenv("WAYLAND_SOCKET"))
        return 0.0;

    ProbeState s;
    s.display = wl_display_connect(nullptr);
    if (!s.display) return 0.0;

    s.registry = wl_display_get_registry(s.display);
    wl_registry_add_listener(s.registry, &registry_listener, &s);

    // Roundtrip 1: receive globals (wl_output, xdg_output_manager).
    wl_display_roundtrip(s.display);
    // Roundtrip 2: receive wl_output.mode events for each output.
    wl_display_roundtrip(s.display);

    if (!s.xdg_output_manager) return 0.0;

    for (auto& o : s.outputs) {
        o->xdg_output = zxdg_output_manager_v1_get_xdg_output(
            s.xdg_output_manager, o->output);
        zxdg_output_v1_add_listener(o->xdg_output, &xdg_output_listener, o.get());
    }

    // Roundtrip 3: receive xdg_output position + logical_size.
    wl_display_roundtrip(s.display);

    OutputInfo* picked = nullptr;
    for (auto& o : s.outputs) {
        if (o->logical_w <= 0 || o->mode_w <= 0) continue;
        if (probe_x >= 0 && probe_y >= 0 &&
            probe_x >= o->x && probe_x < o->x + o->logical_w &&
            probe_y >= o->y && probe_y < o->y + o->logical_h) {
            picked = o.get();
            break;
        }
        if (!picked) picked = o.get();
    }
    if (!picked) return 0.0;
    return static_cast<double>(picked->mode_w) / picked->logical_w;
}

}
