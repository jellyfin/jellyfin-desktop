/*
 * Wayland xdg_positioner size validation (LD_PRELOAD wrapper).
 *
 * Prevents xdg_positioner.set_size(0, 0) from reaching the compositor.
 * The xdg-shell protocol requires width and height to be greater than zero;
 * Mutter enforces this and kills the connection.
 *
 * When an invalid size is detected, both the set_size and the subsequent
 * xdg_popup.reposition are suppressed.  A deferred repositioned event is
 * synthesized on the next protocol call so Qt clears m_waitingForReposition
 * after it has been set.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>

struct wl_proxy;
struct wl_interface;

union wl_argument {
    int32_t  i;
    uint32_t u;
    const void *p;
};

extern const char *wl_proxy_get_class(struct wl_proxy *proxy);
extern const void *wl_proxy_get_listener(struct wl_proxy *proxy);
extern void       *wl_proxy_get_user_data(struct wl_proxy *proxy);

typedef struct wl_proxy *(*marshal_fn_t)(
    struct wl_proxy *, uint32_t, const struct wl_interface *,
    uint32_t, uint32_t, union wl_argument *);

typedef void (*repositioned_fn)(void *data, void *popup, uint32_t token);

static marshal_fn_t real_fn;
static int skip_reposition;

static struct wl_proxy *deferred_popup;
static uint32_t deferred_token;

static void fire_deferred(void)
{
    struct wl_proxy *popup = deferred_popup;
    uint32_t token = deferred_token;
    deferred_popup = NULL;

    const void *listener = wl_proxy_get_listener(popup);
    if (listener) {
        repositioned_fn fn = ((repositioned_fn *)listener)[2];
        if (fn)
            fn(wl_proxy_get_user_data(popup), popup, token);
    }
}

struct wl_proxy *
wl_proxy_marshal_array_flags(struct wl_proxy *proxy, uint32_t opcode,
                             const struct wl_interface *interface,
                             uint32_t version, uint32_t flags,
                             union wl_argument *args)
{
    if (__builtin_expect(!real_fn, 0))
        real_fn = (marshal_fn_t)dlsym(RTLD_NEXT,
                                      "wl_proxy_marshal_array_flags");

    if (__builtin_expect(deferred_popup != NULL, 0))
        fire_deferred();

    if (args) {
        const char *cls = wl_proxy_get_class(proxy);
        if (cls) {
            /* xdg_positioner.set_size — opcode 1 */
            if (opcode == 1 && strcmp(cls, "xdg_positioner") == 0) {
                if (args[0].i <= 0 || args[1].i <= 0) {
                    skip_reposition = 1;
                    return NULL;
                }
                skip_reposition = 0;
            }

            /* xdg_popup.reposition — opcode 2 */
            if (opcode == 2 && skip_reposition &&
                strcmp(cls, "xdg_popup") == 0) {
                skip_reposition = 0;
                deferred_popup = proxy;
                deferred_token = args[1].u;
                return NULL;
            }
        }
    }

    return real_fn(proxy, opcode, interface, version, flags, args);
}
