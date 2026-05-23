#pragma once

#include "include/cef_menu_model.h"

#include "../common.h"

extern "C" void jfn_about_open(void);

// App-level context menu: items appended to every CEF context menu and the
// dispatcher that runs when the user picks one. Lives at the app layer (not
// in the CEF wrapper) because the items and their actions are jellyfin-desktop
// policy, not CEF concerns.
namespace app_menu {

namespace detail {
enum {
    MENU_ID_TOGGLE_FULLSCREEN = MENU_ID_USER_FIRST,
    MENU_ID_ABOUT,
    MENU_ID_EXIT,
};
}

// Append the app's standard items (Toggle Fullscreen, About, Exit) to `model`.
inline void build(CefRefPtr<CefMenuModel> model) {
    model->AddItem(detail::MENU_ID_TOGGLE_FULLSCREEN, "Toggle Fullscreen");
    model->AddItem(detail::MENU_ID_ABOUT, "About");
    model->AddItem(detail::MENU_ID_EXIT, "Exit");
}

// Run the action for `command_id` if it belongs to the app menu.
// Returns true if handled.
inline bool dispatch(int command_id) {
    switch (command_id) {
    case detail::MENU_ID_TOGGLE_FULLSCREEN: g_platform.toggle_fullscreen(); return true;
    case detail::MENU_ID_ABOUT: jfn_about_open(); return true;
    case detail::MENU_ID_EXIT: initiate_shutdown(); return true;
    default: return false;
    }
}

}
