#pragma once

#include <SDL.h>
#include <SDL_vulkan.h>

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
        bool is_key_down(int key) const;
        bool is_mouse_button_down(int button) const;
        std::pair<int, int> consume_mouse_delta();
        int consume_mouse_wheel_delta();
        std::pair<int, int> mouse_position() const;
        std::pair<int, int> client_size() const;

        SDL_Window* handle() const { return window_; }

        unsigned vulkan_extension_count() const;
        const char* const* vulkan_extensions() const;
        bool create_vulkan_surface(void* vkInstance, void* vkSurfaceOut) const;

    private:
        SDL_Window* window_{};
        std::array<bool, SDL_NUM_SCANCODES> keys_{};
        std::array<bool, 5> mouseButtons_{};
        int lastMouseX_{};
        int lastMouseY_{};
        bool hasMousePosition_{};
        int mouseDeltaX_{};
        int mouseDeltaY_{};
        int mouseWheelDelta_{};
        mutable unsigned cachedExtCount_{};
        mutable const char** cachedExts_{};
    };
}
