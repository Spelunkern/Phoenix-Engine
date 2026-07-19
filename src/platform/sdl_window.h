#pragma once

#include <SDL.h>

#include <array>
#include <string>
#include <utility>

namespace phoenix::platform
{
    class SdlWindow
    {
    public:
        SdlWindow() = default;
        ~SdlWindow();
        SdlWindow(const SdlWindow&) = delete;
        SdlWindow& operator=(const SdlWindow&) = delete;

        bool create(int width, int height, const std::string& title);
        bool pump_messages();
        void set_title(const std::string& title);
        // Hide the window instantly (used right before a hard process exit so
        // the close feels immediate while the OS reclaims memory in the
        // background).
        void hide();
        bool is_key_down(int key) const;
        bool is_mouse_button_down(int button) const;
        std::pair<int, int> consume_mouse_delta();
        int consume_mouse_wheel_delta();
        std::pair<int, int> mouse_position() const;
        std::pair<int, int> client_size() const;
        bool is_minimized() const { return minimized_; }
        bool consume_restore_event();

        SDL_Window* handle() const { return window_; }
        SDL_GLContext gl_context() const { return glContext_; }
        // False if the driver rejected the multisample request during
        // create() and the context fell back to single-sampled — the
        // "Anti-aliasing" toggle should hide/disable itself in that case,
        // since GL_MULTISAMPLE has nothing to enable.
        bool has_multisample_context() const { return hasMultisampleContext_; }

    private:
        SDL_Window* window_{};
        SDL_GLContext glContext_{};
        std::array<bool, SDL_NUM_SCANCODES> keys_{};
        std::array<bool, 5> mouseButtons_{};
        int lastMouseX_{};
        int lastMouseY_{};
        bool hasMousePosition_{};
        int mouseDeltaX_{};
        int mouseDeltaY_{};
        int mouseWheelDelta_{};
        bool minimized_{};
        bool restoredEvent_{};
        bool hasMultisampleContext_{};
    };
}
