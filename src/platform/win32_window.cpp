#include "platform/win32_window.h"

#include "imgui_impl_win32.h"

#include <windowsx.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace phoenix::platform
{
    bool Win32Window::create(HINSTANCE instance, int width, int height, const std::wstring& title)
    {
        constexpr wchar_t kClassName[] = L"PhoenixEngineWindow";

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Win32Window::window_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClassName;

        RegisterClassExW(&wc);

        RECT rect{0, 0, width, height};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

        hwnd_ = CreateWindowExW(
            0,
            kClassName,
            title.c_str(),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr,
            nullptr,
            instance,
            this);

        if (!hwnd_)
            return false;

        ShowWindow(hwnd_, SW_MAXIMIZE);
        UpdateWindow(hwnd_);
        return true;
    }

    bool Win32Window::pump_messages()
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                return false;

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        return true;
    }

    void Win32Window::set_title(const std::wstring& title)
    {
        if (hwnd_)
            SetWindowTextW(hwnd_, title.c_str());
    }

    bool Win32Window::is_key_down(int virtualKey) const
    {
        if (virtualKey < 0 || virtualKey >= static_cast<int>(keys_.size()))
            return false;

        return keys_[static_cast<std::size_t>(virtualKey)]
            || (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    }

    bool Win32Window::is_mouse_button_down(int button) const
    {
        if (button < 0 || button >= static_cast<int>(mouseButtons_.size()))
            return false;

        const int virtualKeys[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
        return mouseButtons_[static_cast<std::size_t>(button)]
            || (GetAsyncKeyState(virtualKeys[button]) & 0x8000) != 0;
    }

    std::pair<int, int> Win32Window::consume_mouse_delta()
    {
        const auto delta = std::pair<int, int>{ mouseDeltaX_, mouseDeltaY_ };
        mouseDeltaX_ = 0;
        mouseDeltaY_ = 0;
        return delta;
    }

    int Win32Window::consume_mouse_wheel_delta()
    {
        const auto delta = mouseWheelDelta_;
        mouseWheelDelta_ = 0;
        return delta;
    }

    std::pair<int, int> Win32Window::mouse_position() const
    {
        return { static_cast<int>(lastMousePosition_.x), static_cast<int>(lastMousePosition_.y) };
    }

    std::pair<int, int> Win32Window::client_size() const
    {
        RECT rect{};
        if (!hwnd_ || !GetClientRect(hwnd_, &rect))
            return { 0, 0 };

        return {
            static_cast<int>(rect.right - rect.left),
            static_cast<int>(rect.bottom - rect.top),
        };
    }

    LRESULT CALLBACK Win32Window::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam);

        auto* window = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (message)
        {
        case WM_NCCREATE:
        {
            const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
            return TRUE;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (window && wparam < window->keys_.size())
                window->keys_[static_cast<std::size_t>(wparam)] = true;
            if (wparam == VK_ESCAPE)
                DestroyWindow(hwnd);
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (window && wparam < window->keys_.size())
                window->keys_[static_cast<std::size_t>(wparam)] = false;
            return 0;
        case WM_MOUSEMOVE:
            if (window)
            {
                POINT position{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                if (window->hasMousePosition_)
                {
                    window->mouseDeltaX_ += position.x - window->lastMousePosition_.x;
                    window->mouseDeltaY_ += position.y - window->lastMousePosition_.y;
                }
                window->lastMousePosition_ = position;
                window->hasMousePosition_ = true;
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (window)
                window->mouseButtons_[0] = true;
            SetCapture(hwnd);
            return 0;
        case WM_LBUTTONUP:
            if (window)
                window->mouseButtons_[0] = false;
            ReleaseCapture();
            return 0;
        case WM_RBUTTONDOWN:
            if (window)
                window->mouseButtons_[1] = true;
            SetCapture(hwnd);
            return 0;
        case WM_RBUTTONUP:
            if (window)
                window->mouseButtons_[1] = false;
            ReleaseCapture();
            return 0;
        case WM_MOUSEWHEEL:
            if (window)
                window->mouseWheelDelta_ += GET_WHEEL_DELTA_WPARAM(wparam);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }
}

