#pragma once

#include "input_layer.h"
#include "window_state.h"
#include "../cef/cef_client.h"
#include <SDL3/SDL.h>

// Input layer that forwards events to a CEF browser client
class BrowserLayer : public InputLayer, public WindowStateListener {
public:
    explicit BrowserLayer(InputReceiver* receiver) : receiver_(receiver) {}

    void setReceiver(InputReceiver* receiver) { receiver_ = receiver; }
    InputReceiver* receiver() const { return receiver_; }
    void setWindowSize(int w, int h) { window_width_ = w; window_height_ = h; }

    bool handleInput(const SDL_Event& event) override {
        if (!receiver_) return false;

        switch (event.type) {
            case SDL_EVENT_MOUSE_MOTION: {
                mouse_x_ = static_cast<int>(event.motion.x);
                mouse_y_ = static_cast<int>(event.motion.y);
                int mods = getModifiers();
                SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
                if (buttons & SDL_BUTTON_LMASK) mods |= EVENTFLAG_LEFT_MOUSE_BUTTON;
                if (buttons & SDL_BUTTON_MMASK) mods |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
                if (buttons & SDL_BUTTON_RMASK) mods |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
                receiver_->sendMouseMove(mouse_x_, mouse_y_, mods);
                return true;
            }

            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                int btn = event.button.button;
                switch (btn) {
                    case SDL_BUTTON_LEFT:
                    case SDL_BUTTON_MIDDLE:
                    case SDL_BUTTON_RIGHT: {
                        int x = static_cast<int>(event.button.x);
                        int y = static_cast<int>(event.button.y);
                        int mods = getModifiers();
                        updateClickCount(x, y, btn);
                        receiver_->sendFocus(true);
                        receiver_->sendMouseClick(x, y, true, btn, click_count_, mods);
                        break;
                    }
                    case SDL_BUTTON_X1: receiver_->goBack(); break;
                    case SDL_BUTTON_X2: receiver_->goForward(); break;
                }
                return true;
            }

            case SDL_EVENT_MOUSE_BUTTON_UP: {
                int btn = event.button.button;
                switch (btn) {
                    case SDL_BUTTON_LEFT:
                    case SDL_BUTTON_MIDDLE:
                    case SDL_BUTTON_RIGHT: {
                        int x = static_cast<int>(event.button.x);
                        int y = static_cast<int>(event.button.y);
                        int mods = getModifiers();
                        receiver_->sendMouseClick(x, y, false, btn, click_count_, mods);
                        break;
                    }
                }
                return true;
            }

            case SDL_EVENT_MOUSE_WHEEL: {
                int mods = getModifiers();
                receiver_->sendMouseWheel(mouse_x_, mouse_y_, event.wheel.x, event.wheel.y, mods);
                return true;
            }

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                bool down = (event.type == SDL_EVENT_KEY_DOWN);
                int mods = getModifiers();

                // Handle action modifier shortcuts (Cmd on macOS, Ctrl elsewhere)
                if (down && isActionModifier()) {
                    bool shift = mods & EVENTFLAG_SHIFT_DOWN;
                    switch (event.key.key) {
                        case SDLK_V: {
                            static const char* mimeTypes[] = {
                                "image/png", "image/jpeg", "image/gif",
                                "text/html", "text/plain"
                            };
                            for (const char* mime : mimeTypes) {
                                size_t len = 0;
                                void* data = SDL_GetClipboardData(mime, &len);
                                if (data && len > 0) {
                                    receiver_->paste(mime, data, len);
                                    SDL_free(data);
                                    break;
                                }
                            }
                            return true;
                        }
                        case SDLK_C: receiver_->copy(); return true;
                        case SDLK_X: receiver_->cut(); return true;
                        case SDLK_A: receiver_->selectAll(); return true;
                        case SDLK_Z:
                            if (shift) receiver_->redo();
                            else receiver_->undo();
                            return true;
                        case SDLK_Y: receiver_->redo(); return true;
                    }
                }

                receiver_->sendKeyEvent(event.key.key, down, mods);
                return true;
            }

            case SDL_EVENT_TEXT_INPUT: {
                int mods = getModifiers();
                for (const char* c = event.text.text; *c; ++c) {
                    unsigned char ch = static_cast<unsigned char>(*c);
                    // Skip control characters - already handled by KEY_DOWN
                    // (macOS generates TEXT_INPUT for Tab, Backspace, Delete, etc.)
                    if (ch < 0x20 || ch == 0x7F) continue;
                    receiver_->sendChar(ch, mods);
                }
                return true;
            }

            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_MOTION: {
                int type;
                if (event.type == SDL_EVENT_FINGER_DOWN) type = 1;
                else if (event.type == SDL_EVENT_FINGER_UP) type = 0;
                else type = 2;
                // SDL coords are 0-1 normalized, convert to window pixels
                float x = event.tfinger.x * window_width_;
                float y = event.tfinger.y * window_height_;
                int mods = getModifiers();
                receiver_->sendTouch(static_cast<int>(event.tfinger.fingerID & 0xFFFF),
                                     x, y, 0, 0, event.tfinger.pressure, type, mods);
                return true;
            }

            default:
                return false;
        }
    }

    // WindowStateListener
    void onFocusGained() override {
        if (receiver_) receiver_->sendFocus(true);
    }

    void onFocusLost() override {
        if (receiver_) receiver_->sendFocus(false);
    }

private:
    static constexpr int MULTI_CLICK_TIME = 500;
    static constexpr int MULTI_CLICK_DISTANCE = 5;

    int getModifiers() {
        SDL_Keymod mod = SDL_GetModState();
        int mods = 0;
        if (mod & SDL_KMOD_SHIFT) mods |= EVENTFLAG_SHIFT_DOWN;
        if (mod & SDL_KMOD_CTRL) mods |= EVENTFLAG_CONTROL_DOWN;
        if (mod & SDL_KMOD_ALT) mods |= EVENTFLAG_ALT_DOWN;
        return mods;
    }

    // Returns true if the platform's action modifier is pressed (Cmd on macOS, Ctrl elsewhere)
    bool isActionModifier() {
        SDL_Keymod mod = SDL_GetModState();
#ifdef __APPLE__
        return mod & SDL_KMOD_GUI;
#else
        return mod & SDL_KMOD_CTRL;
#endif
    }

    void updateClickCount(int x, int y, int btn) {
        Uint64 now = SDL_GetTicks();
        int dx = x - last_click_x_;
        int dy = y - last_click_y_;
        bool same_spot = (dx * dx + dy * dy) <= (MULTI_CLICK_DISTANCE * MULTI_CLICK_DISTANCE);
        bool same_button = (btn == last_click_button_);
        bool in_time = (now - last_click_time_) <= MULTI_CLICK_TIME;

        if (same_spot && same_button && in_time) {
            click_count_ = (click_count_ % 3) + 1;
        } else {
            click_count_ = 1;
        }

        last_click_time_ = now;
        last_click_x_ = x;
        last_click_y_ = y;
        last_click_button_ = btn;
    }

    InputReceiver* receiver_ = nullptr;
    int window_width_ = 0;
    int window_height_ = 0;
    int mouse_x_ = 0;
    int mouse_y_ = 0;
    Uint64 last_click_time_ = 0;
    int last_click_x_ = 0;
    int last_click_y_ = 0;
    int last_click_button_ = 0;
    int click_count_ = 1;
};
