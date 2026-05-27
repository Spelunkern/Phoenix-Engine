#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <string>
#include <utility>

namespace phoenix::platform
{
    class Win32Window
    {
    public:
        Win32Window() = default;
        Win32Window(const Win32Window&) = delete;
        Win32Window& operator=(const Win32Window&) = delete;

        bool create(HINSTANCE instance, int width, int height, const std::wstring& title);
        bool pump_messages();
        void set_title(const std::wstring& title);
        bool is_key_down(int virtualKey) const;
        bool is_mouse_button_down(int button) const;
        std::pair<int, int> consume_mouse_delta();
        int consume_mouse_wheel_delta();
        std::pair<int, int> mouse_position() const;
        std::pair<int, int> client_size() const;
        HWND handle() const { return hwnd_; }

    private:
        static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

        HWND hwnd_{};
        std::array<bool, 256> keys_{};
        std::array<bool, 5> mouseButtons_{};
        POINT lastMousePosition_{};
        bool hasMousePosition_{};
        int mouseDeltaX_{};
        int mouseDeltaY_{};
        int mouseWheelDelta_{};
    };
}
