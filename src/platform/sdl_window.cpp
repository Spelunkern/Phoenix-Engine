#include "platform/sdl_window.h"

#include "imgui_impl_sdl2.h"

namespace phoenix::platform
{
    SdlWindow::~SdlWindow()
    {
        if (window_)
            SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    bool SdlWindow::create(int width, int height, const std::string& title)
    {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
            return false;

        window_ = SDL_CreateWindow(
            title.c_str(),
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            width,
            height,
            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);

        return window_ != nullptr;
    }

    bool SdlWindow::pump_messages()
    {
        SDL_Event event{};
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);

            switch (event.type)
            {
            case SDL_QUIT:
                return false;

            case SDL_KEYDOWN:
                if (event.key.keysym.scancode < static_cast<int>(keys_.size()))
                    keys_[event.key.keysym.scancode] = true;
                if (event.key.keysym.sym == SDLK_ESCAPE)
                {
                    SDL_Event quit{};
                    quit.type = SDL_QUIT;
                    SDL_PushEvent(&quit);
                }
                break;

            case SDL_KEYUP:
                if (event.key.keysym.scancode < static_cast<int>(keys_.size()))
                    keys_[event.key.keysym.scancode] = false;
                break;

            case SDL_MOUSEMOTION:
                if (hasMousePosition_)
                {
                    mouseDeltaX_ += event.motion.x - lastMouseX_;
                    mouseDeltaY_ += event.motion.y - lastMouseY_;
                }
                lastMouseX_ = event.motion.x;
                lastMouseY_ = event.motion.y;
                hasMousePosition_ = true;
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT)   mouseButtons_[0] = true;
                if (event.button.button == SDL_BUTTON_RIGHT)  mouseButtons_[1] = true;
                if (event.button.button == SDL_BUTTON_MIDDLE) mouseButtons_[2] = true;
                break;

            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT)   mouseButtons_[0] = false;
                if (event.button.button == SDL_BUTTON_RIGHT)  mouseButtons_[1] = false;
                if (event.button.button == SDL_BUTTON_MIDDLE) mouseButtons_[2] = false;
                break;

            case SDL_MOUSEWHEEL:
                mouseWheelDelta_ += event.wheel.y * 120;
                break;
            }
        }
        return true;
    }

    void SdlWindow::set_title(const std::string& title)
    {
        if (window_)
            SDL_SetWindowTitle(window_, title.c_str());
    }

    bool SdlWindow::is_key_down(int key) const
    {
        const auto scancode = SDL_GetScancodeFromKey(key);
        if (scancode >= 0 && scancode < static_cast<int>(keys_.size()))
            return keys_[scancode];
        return false;
    }

    bool SdlWindow::is_mouse_button_down(int button) const
    {
        if (button < 0 || button >= static_cast<int>(mouseButtons_.size()))
            return false;
        return mouseButtons_[static_cast<std::size_t>(button)];
    }

    std::pair<int, int> SdlWindow::consume_mouse_delta()
    {
        const auto delta = std::pair<int, int>{ mouseDeltaX_, mouseDeltaY_ };
        mouseDeltaX_ = 0;
        mouseDeltaY_ = 0;
        return delta;
    }

    int SdlWindow::consume_mouse_wheel_delta()
    {
        const auto delta = mouseWheelDelta_;
        mouseWheelDelta_ = 0;
        return delta;
    }

    std::pair<int, int> SdlWindow::mouse_position() const
    {
        return { lastMouseX_, lastMouseY_ };
    }

    std::pair<int, int> SdlWindow::client_size() const
    {
        int w = 0, h = 0;
        if (window_)
            SDL_Vulkan_GetDrawableSize(window_, &w, &h);
        return { w, h };
    }

    unsigned SdlWindow::vulkan_extension_count() const
    {
        if (!cachedExtCount_ && window_)
            SDL_Vulkan_GetInstanceExtensions(window_, &cachedExtCount_, nullptr);
        return cachedExtCount_;
    }

    const char* const* SdlWindow::vulkan_extensions() const
    {
        if (!cachedExts_ && window_)
        {
            unsigned count = 0;
            SDL_Vulkan_GetInstanceExtensions(window_, &count, nullptr);
            cachedExts_ = new const char*[count];
            SDL_Vulkan_GetInstanceExtensions(window_, &count, cachedExts_);
            cachedExtCount_ = count;
        }
        return cachedExts_;
    }

    bool SdlWindow::create_vulkan_surface(void* vkInstance, void* vkSurfaceOut) const
    {
        return window_ && SDL_Vulkan_CreateSurface(
            window_,
            static_cast<VkInstance>(vkInstance),
            static_cast<VkSurfaceKHR*>(vkSurfaceOut)) == SDL_TRUE;
    }
}
