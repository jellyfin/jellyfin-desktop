#include "app_menu.h"
#include "about_browser.h"
#include "../common.h"
#include "../platform/platform.h"

extern Platform g_platform;

namespace app_menu {
namespace {
enum {
    MENU_ID_TOGGLE_FULLSCREEN = MENU_ID_USER_FIRST,
    MENU_ID_ABOUT,
    MENU_ID_EXIT,
};
}

void build(CefRefPtr<CefMenuModel> model) {
    model->AddItem(MENU_ID_TOGGLE_FULLSCREEN, "Toggle Fullscreen");
    model->AddItem(MENU_ID_ABOUT, "About");
    model->AddItem(MENU_ID_EXIT, "Exit");
}

bool dispatch(int command_id) {
    switch (command_id) {
    case MENU_ID_TOGGLE_FULLSCREEN: g_platform.toggle_fullscreen(); return true;
    case MENU_ID_ABOUT: AboutBrowser::open(); return true;
    case MENU_ID_EXIT: initiate_shutdown(); return true;
    default: return false;
    }
}

}
