#pragma once

#include "platform/sdl_window.h"
#include "renderer/opengl_renderer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phoenix::ui::px
{
    struct Vec2
    {
        float x{}, y{};
    };

    struct Color
    {
        float r{}, g{}, b{}, a{ 1.0f };
    };

    enum Condition : int
    {
        Always,
        FirstUseEver,
    };

    enum WindowFlag : std::uint32_t
    {
        NoTitleBar = 1u << 0u,
        NoResize = 1u << 1u,
        NoScrollbar = 1u << 2u,
        NoFocusOnAppearing = 1u << 3u,
        NoMove = 1u << 4u,
        AlwaysAutoResize = 1u << 5u,
    };

    enum StyleColor : int
    {
        ButtonColor,
        ButtonActiveColor,
    };

    void begin_frame(const phoenix::platform::SdlWindow& window, float width, float height);
    std::vector<phoenix::renderer::ScreenUiCommand> end_frame();
    bool wants_mouse();
    bool wants_keyboard();

    void set_window_position(float x, float y);
    Vec2 window_position();

    void SetNextWindowPos(Vec2 position, Condition condition = Always);
    void SetNextWindowSize(Vec2 size, Condition condition = Always);
    void SetNextWindowBgAlpha(float alpha);
    bool Begin(const char* title, bool* open = nullptr, std::uint32_t flags = 0);
    void End();

    void SetNextItemWidth(float width);
    bool Button(const char* label, Vec2 size = {});
    bool Checkbox(const char* label, bool* value);
    bool SliderFloat(const char* label, float* value, float minimum, float maximum,
        const char* format = "%.2f");
    bool InputInt(const char* label, int* value);
    bool DragFloat3(const char* label, float value[3], float speed,
        float minimum = 0.0f, float maximum = 0.0f);
    bool Combo(const char* label, int* current, const char* const items[], int count);
    bool BeginCombo(const char* label, const char* preview);
    bool Selectable(const char* label, bool selected = false);
    void EndCombo();
    void SetItemDefaultFocus();

    void SameLine(float offsetFromStart = 0.0f, float spacing = 5.0f);
    void Separator();
    void Spacing();
    void TextUnformatted(const char* text);
    void Text(const char* format, ...);
    void TextDisabled(const char* format, ...);
    void TextColored(Color color, const char* format, ...);
    void BulletText(const char* format, ...);

    void PushStyleColor(StyleColor index, Color color);
    void PopStyleColor();
    Color GetStyleColor(StyleColor index);

    void background_rect(Vec2 minimum, Vec2 maximum, Color color);
}
