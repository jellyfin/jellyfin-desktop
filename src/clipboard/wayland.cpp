#include "clipboard/wayland.h"

#include "wake_event.h"

#include <wayland-client.h>
#include "ext-data-control-v1-client.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Wayland clipboard (CLIPBOARD selection) using ext-data-control-v1.
//
// Why not wl_data_device_manager on the main display: that protocol is
// focus-bound — the compositor only routes selection events to the
// currently focused seat client. Because CEF is an X11 client via ozone,
// binding wl_data_device on our main (jellyfin) wl_display competes with
// XWayland's own wl_data_device bridge on the same seat and breaks the
// Wayland→X11 clipboard flow CEF depends on for Ctrl+V.
//
// ext-data-control-v1 is designed for clipboard managers and automation
// tools — focus-independent, purely observational. mpv's
// clipboard-wayland.c uses the same approach. We follow its pattern:
// dedicated wl_display_connect(NULL), dedicated event queue implicit in
// that new connection, dedicated worker thread. The main jellyfin
// Wayland connection (and XWayland's clipboard bridge) are untouched.

namespace clipboard_wayland {
namespace {

// Accepted text MIME types, in preference order. UTF-8 is preferred; the
// legacy X11 names are kept for apps that still advertise only those.
constexpr const char* kMimeTextPlainUtf8 = "text/plain;charset=utf-8";
constexpr const char* kMimeTextPlain     = "text/plain";
constexpr const char* kMimeUtf8String    = "UTF8_STRING";
constexpr const char* kMimeString        = "STRING";
constexpr const char* kMimeText          = "TEXT";

struct OfferState {
    ext_data_control_offer_v1* offer = nullptr;

    bool has_text_plain_utf8 = false;
    bool has_text_plain      = false;
    bool has_utf8_string     = false;
    bool has_string          = false;
    bool has_text            = false;
};

const char* best_text_mime(const OfferState& s) {
    if (s.has_text_plain_utf8) return kMimeTextPlainUtf8;
    if (s.has_text_plain)      return kMimeTextPlain;
    if (s.has_utf8_string)     return kMimeUtf8String;
    if (s.has_string)          return kMimeString;
    if (s.has_text)            return kMimeText;
    return nullptr;
}

void destroy_offer(OfferState* o) {
    if (!o) return;
    if (o->offer) ext_data_control_offer_v1_destroy(o->offer);
    delete o;
}

// An outstanding async read: worker polls `fd`, appends bytes into `text`,
// fires `on_done` when the source closes the pipe.
struct PendingRead {
    int fd = -1;
    std::string text;
    std::function<void(std::string)> on_done;
};

struct State {
    // Own wl_display connection — isolated from the main jellyfin display.
    wl_display*                      display = nullptr;
    wl_seat*                         seat = nullptr;
    ext_data_control_manager_v1*     mgr = nullptr;
    ext_data_control_device_v1*      device = nullptr;

    // Current CLIPBOARD offer (accessed only from the worker thread).
    OfferState*                      current_offer = nullptr;

    // Cross-thread mailbox of new read requests.
    std::mutex                       mtx;
    std::vector<PendingRead>         queued;

    // Worker-owned outstanding reads, polled on each iteration.
    std::vector<PendingRead>         active;

    WakeEvent                        wake;   // poked when `queued` grows
    std::thread                      worker;
    std::atomic<bool>                stop{false};
};

State g;

// --- Offer listener ---------------------------------------------------------

void offer_offer(void* data, ext_data_control_offer_v1*, const char* mime) {
    auto* s = static_cast<OfferState*>(data);
    if (!s || !mime) return;
    if      (strcmp(mime, kMimeTextPlainUtf8) == 0) s->has_text_plain_utf8 = true;
    else if (strcmp(mime, kMimeTextPlain)     == 0) s->has_text_plain      = true;
    else if (strcmp(mime, kMimeUtf8String)    == 0) s->has_utf8_string     = true;
    else if (strcmp(mime, kMimeString)        == 0) s->has_string          = true;
    else if (strcmp(mime, kMimeText)          == 0) s->has_text            = true;
}

const ext_data_control_offer_v1_listener s_offer = { .offer = offer_offer };

// --- Device listener --------------------------------------------------------

void dd_data_offer(void*, ext_data_control_device_v1*, ext_data_control_offer_v1* id) {
    auto* state = new OfferState{id, false, false, false, false, false};
    ext_data_control_offer_v1_add_listener(id, &s_offer, state);
}

void dd_selection(void*, ext_data_control_device_v1*, ext_data_control_offer_v1* id) {
    // The offer argument is either one of the offers we already attached
    // via dd_data_offer (in which case its user_data is our OfferState),
    // or null (clipboard cleared). We take ownership and destroy the
    // previous selection offer.
    if (g.current_offer) {
        destroy_offer(g.current_offer);
        g.current_offer = nullptr;
    }
    if (id) {
        g.current_offer = static_cast<OfferState*>(ext_data_control_offer_v1_get_user_data(id));
    }
}

void dd_finished(void*, ext_data_control_device_v1* dev) {
    // Compositor invalidated the device (e.g. manager destroyed mid-run).
    ext_data_control_device_v1_destroy(dev);
    if (g.device == dev) g.device = nullptr;
}

void dd_primary_selection(void*, ext_data_control_device_v1*, ext_data_control_offer_v1* id) {
    // We don't surface primary selection; drop the offer.
    if (id) {
        auto* state = static_cast<OfferState*>(ext_data_control_offer_v1_get_user_data(id));
        destroy_offer(state);
    }
}

const ext_data_control_device_v1_listener s_device = {
    .data_offer        = dd_data_offer,
    .selection         = dd_selection,
    .finished          = dd_finished,
    .primary_selection = dd_primary_selection,
};

// --- Registry ---------------------------------------------------------------

void reg_global(void*, wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    if (strcmp(iface, wl_seat_interface.name) == 0) {
        // We only need the seat as a handle for get_data_device — no input
        // listener required.
        unsigned v = ver < 1 ? 1 : (ver < 8 ? ver : 8);
        g.seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, v));
    } else if (strcmp(iface, ext_data_control_manager_v1_interface.name) == 0) {
        unsigned v = ver < 2 ? ver : 2;
        g.mgr = static_cast<ext_data_control_manager_v1*>(
            wl_registry_bind(reg, name, &ext_data_control_manager_v1_interface, v));
    }
}
void reg_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener s_reg = { .global = reg_global, .global_remove = reg_remove };

// --- Worker thread ----------------------------------------------------------

// Issue a new receive on the current selection offer. Returns the read end
// of the pipe, or -1 if there's nothing to read.
int start_receive() {
    if (!g.current_offer || !g.current_offer->offer) return -1;
    const char* mime = best_text_mime(*g.current_offer);
    if (!mime) return -1;

    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) return -1;
    ext_data_control_offer_v1_receive(g.current_offer->offer, mime, fds[1]);
    wl_display_flush(g.display);
    close(fds[1]);
    return fds[0];
}

// Serialize reads: at most one outstanding ext_data_control_offer_v1_receive
// at a time. This is natural back-pressure — a stuck source can't stack up
// pipe fds if the user holds Ctrl+V — and matches the "paste once per user
// intent" model. When the active read completes, worker_thread_func calls
// this again to pick up the next queued request.
void promote_next_queued() {
    if (!g.active.empty()) return;

    PendingRead req;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        if (g.queued.empty()) return;
        req = std::move(g.queued.front());
        g.queued.erase(g.queued.begin());
    }

    int fd = start_receive();
    if (fd < 0) {
        req.on_done(std::string{});
        // Whatever was wrong (no offer, no text mime, pipe creation
        // failed) applies equally to any further queued requests — drop
        // them all with empty results rather than spinning.
        std::vector<PendingRead> drained;
        {
            std::lock_guard<std::mutex> lk(g.mtx);
            drained.swap(g.queued);
        }
        for (auto& r : drained) r.on_done(std::string{});
        return;
    }
    req.fd = fd;
    g.active.push_back(std::move(req));
}

void worker_thread_func() {
    int display_fd = wl_display_get_fd(g.display);

    while (!g.stop.load(std::memory_order_relaxed)) {
        // Drain anything already buffered before preparing a new read.
        while (wl_display_prepare_read(g.display) != 0)
            wl_display_dispatch_pending(g.display);
        wl_display_flush(g.display);

        // Build pollfd set: display, wake, then each active receive.
        std::vector<pollfd> pfds;
        pfds.reserve(2 + g.active.size());
        pfds.push_back({display_fd,  POLLIN, 0});
        pfds.push_back({g.wake.fd(), POLLIN, 0});
        for (auto& req : g.active)
            pfds.push_back({req.fd, POLLIN, 0});

        int r = poll(pfds.data(), pfds.size(), -1);
        if (r < 0) {
            if (errno == EINTR) { wl_display_cancel_read(g.display); continue; }
            wl_display_cancel_read(g.display);
            break;
        }

        // Display socket.
        if (pfds[0].revents & POLLIN) {
            wl_display_read_events(g.display);
        } else {
            wl_display_cancel_read(g.display);
        }
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        // Wake: drain the eventfd, then dispatch any queued events so
        // current_offer reflects the latest selection before we start
        // the next receive.
        if (pfds[1].revents & POLLIN) {
            g.wake.drain();
            wl_display_dispatch_pending(g.display);
        }

        // Active receive. There's at most one, at pfds index 2.
        if (!g.active.empty() && pfds.size() > 2) {
            short revents = pfds[2].revents;
            bool done = false;

            if (revents & POLLIN) {
                char buf[4096];
                for (;;) {
                    ssize_t n = read(g.active[0].fd, buf, sizeof(buf));
                    if (n > 0) { g.active[0].text.append(buf, n); continue; }
                    if (n == 0) { done = true; break; }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    done = true;
                    break;
                }
            }
            if (revents & (POLLHUP | POLLERR | POLLNVAL)) done = true;

            if (done) {
                close(g.active[0].fd);
                auto cb = std::move(g.active[0].on_done);
                auto text = std::move(g.active[0].text);
                g.active.clear();
                cb(std::move(text));
            }
        }

        // Start the next receive if the active slot is free and something
        // is queued — covers both the wake path and the just-completed
        // path above, without duplicating logic.
        promote_next_queued();

        wl_display_dispatch_pending(g.display);
    }

    wl_display_cancel_read(g.display);

    // Fire still-outstanding callbacks with empty results so waiters
    // don't get lost.
    for (auto& req : g.active) {
        if (req.fd >= 0) close(req.fd);
        if (req.on_done) req.on_done(std::string{});
    }
    g.active.clear();
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        for (auto& req : g.queued) {
            if (req.on_done) req.on_done(std::string{});
        }
        g.queued.clear();
    }
}

}  // namespace

// --- Public API -------------------------------------------------------------

void init() {
    // Dedicated connection so we don't share queues/globals with the main
    // jellyfin display or XWayland's clipboard bridge on the same seat.
    g.display = wl_display_connect(nullptr);
    if (!g.display) return;

    auto* reg = wl_display_get_registry(g.display);
    wl_registry_add_listener(reg, &s_reg, nullptr);
    wl_display_roundtrip(g.display);
    wl_registry_destroy(reg);

    if (!g.mgr || !g.seat) {
        if (g.mgr)     { ext_data_control_manager_v1_destroy(g.mgr); g.mgr = nullptr; }
        if (g.seat)    { wl_seat_destroy(g.seat); g.seat = nullptr; }
        wl_display_disconnect(g.display);
        g.display = nullptr;
        return;
    }

    g.device = ext_data_control_manager_v1_get_data_device(g.mgr, g.seat);
    if (!g.device) {
        ext_data_control_manager_v1_destroy(g.mgr); g.mgr = nullptr;
        wl_seat_destroy(g.seat); g.seat = nullptr;
        wl_display_disconnect(g.display); g.display = nullptr;
        return;
    }
    ext_data_control_device_v1_add_listener(g.device, &s_device, nullptr);

    // Settle the initial selection state before any reads come in.
    wl_display_roundtrip(g.display);

    g.worker = std::thread(worker_thread_func);
}

bool available() {
    return g.device != nullptr && g.worker.joinable();
}

void read_text_async(std::function<void(std::string)> on_done) {
    if (!g.device || !g.worker.joinable()) {
        if (on_done) on_done(std::string{});
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        g.queued.push_back(PendingRead{-1, std::string{}, std::move(on_done)});
    }
    g.wake.signal();
}

void cleanup() {
    if (g.worker.joinable()) {
        g.stop.store(true, std::memory_order_relaxed);
        g.wake.signal();
        g.worker.join();
    }
    if (g.current_offer) { destroy_offer(g.current_offer); g.current_offer = nullptr; }
    if (g.device)  { ext_data_control_device_v1_destroy(g.device); g.device = nullptr; }
    if (g.mgr)     { ext_data_control_manager_v1_destroy(g.mgr); g.mgr = nullptr; }
    if (g.seat)    { wl_seat_destroy(g.seat); g.seat = nullptr; }
    if (g.display) { wl_display_disconnect(g.display); g.display = nullptr; }
}

}  // namespace clipboard_wayland
