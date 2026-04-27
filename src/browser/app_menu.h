#pragma once

#include "include/cef_menu_model.h"

// App-level context menu: items appended to every CEF context menu and the
// dispatcher that runs when the user picks one. Lives at the app layer (not
// in the CEF wrapper) because the items and their actions are jellyfin-desktop
// policy, not CEF concerns.
namespace app_menu {

// Append the app's standard items (Toggle Fullscreen, About, Exit) to `model`.
void build(CefRefPtr<CefMenuModel> model);

// Run the action for `command_id` if it belongs to the app menu.
// Returns true if handled.
bool dispatch(int command_id);

}
