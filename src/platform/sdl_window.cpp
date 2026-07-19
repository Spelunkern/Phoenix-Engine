#include "platform/sdl_window.h"

#include "imgui_impl_sdl2.h"

namespace phoenix::platform
{
    SdlWindow::~SdlWindow()
    {
        if (glContext_)
            SDL_GL_DeleteContext(glContext_);
        if (window_)
            SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    bool SdlWindow::create(int width, int height, const std::string& title)
    {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
            return false;

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        // Requested unconditionally so the default framebuffer always has
        // sample buffers to enable/disable at runtime (see
        // OpenGLRenderer::set_antialiasing_enabled) — GL_MULTISAMPLE can
        // only be toggled live if the context was created with multisample
        // support in the first place; it can't be added after the fact.
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
#if !defined(NDEBUG)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif

        window_ = SDL_CreateWindow(
            title.c_str(),
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            width,
            height,
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
        if (!window_)
            return false;

        glContext_ = SDL_GL_CreateContext(window_);
        if (!glContext_)
        {
            // Some older/integrated drivers reject the 4x multisample
            // request outright — retry once without it rather than failing
            // to start entirely.
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
            glContext_ = SDL_GL_CreateContext(window_);
            if (!glContext_)
                return false;
            hasMultisampleContext_ = false;
        }
        else
        {
            int actualSamples = 0;
            SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &actualSamples);
            hasMultisampleContext_ = actualSamples > 0;
        }

        // 4.5 core may not be available on older/integrated drivers; retry at
        // 4.3 (still covers compute shaders) before giving up entirely.
        SDL_GL_MakeCurrent(window_, glContext_);

        // The Vulkan renderer preferred VK_PRESENT_MODE_MAILBOX_KHR (present
        // without blocking the loop on the display's refresh) and only fell
        // back to FIFO (vsync-blocking) when mailbox wasn't available. Mirror
        // that by leaving swap interval off here too: the render loop's own
        // FPS Cap combo (perf_hud.cpp, up to 360) already paces frames via a
        // sleep after render_frame(), same as the original design. Forcing
        // SwapInterval(1) would block every SwapWindow() on vsync and cap
        // everything at the monitor's refresh rate regardless of that combo.
        SDL_GL_SetSwapInterval(0);
        return true;
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

            case SDL_WINDOWEVENT:
                switch (event.window.event)
                {
                case SDL_WINDOWEVENT_MINIMIZED:
                case SDL_WINDOWEVENT_HIDDEN:
                    minimized_ = true;
                    break;
                case SDL_WINDOWEVENT_RESTORED:
                case SDL_WINDOWEVENT_SHOWN:
                case SDL_WINDOWEVENT_MAXIMIZED:
                    minimized_ = false;
                    restoredEvent_ = true;
                    break;
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    if (event.window.data1 > 0 && event.window.data2 > 0)
                    {
                        minimized_ = false;
                        restoredEvent_ = true;
                    }
                    else
                    {
                        minimized_ = true;
                    }
                    break;
                default:
                    break;
                }
                break;

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

    void SdlWindow::hide()
    {
        if (window_)
            SDL_HideWindow(window_);
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
            SDL_GL_GetDrawableSize(window_, &w, &h);
        return { w, h };
    }

    bool SdlWindow::consume_restore_event()
    {
        const bool value = restoredEvent_;
        restoredEvent_ = false;
        return value;
    }
}
