#include "ui/phoenix_ui.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string_view>
#include <unordered_map>

namespace phoenix::ui::px
{
    namespace
    {
        constexpr float kTextHeight = 16.0f;
        constexpr float kItemHeight = 22.0f;
        constexpr float kSpacing = 4.0f;
        constexpr float kPadding = 8.0f;
        constexpr float kTitleHeight = 24.0f;

        struct Rect
        {
            float x{}, y{}, w{}, h{};
        };

        struct WindowState
        {
            float x{ 8.0f }, y{ 8.0f }, width{ 300.0f }, height{ 180.0f };
            float scroll{};
            bool positioned{};
        };

        struct Context
        {
            const phoenix::platform::SdlWindow* window{};
            float surfaceWidth{}, surfaceHeight{};
            float mouseX{}, mouseY{};
            float wheel{};
            bool mouseDown{}, mousePressed{};
            bool captureMouse{}, captureKeyboard{};

            std::vector<phoenix::renderer::ScreenUiCommand> background;
            std::vector<phoenix::renderer::ScreenUiCommand> commands;
            std::vector<phoenix::renderer::ScreenUiCommand> overlay;
            std::vector<phoenix::renderer::ScreenUiCommand>* sink{ &commands };

            std::unordered_map<std::uint64_t, WindowState> windows;
            WindowState* activeWindow{};
            std::uint64_t windowId{};
            Rect contentClip{};
            float contentX{}, cursorY{}, logicalBottom{};
            Rect lastItem{};
            bool sameLine{};
            float sameLineOffset{}, sameLineSpacing{ 5.0f };
            float nextItemWidth{};
            float nextWindowAlpha{ 0.90f };
            Vec2 nextWindowPos{};
            Vec2 nextWindowSize{};
            Condition nextWindowCondition{ FirstUseEver };
            bool hasNextWindowPos{}, hasNextWindowSize{};
            std::size_t windowBackgroundIndex{};

            std::uint64_t hotId{}, activeId{}, openCombo{};
            std::uint64_t draggingWindow{};
            float dragOffsetX{}, dragOffsetY{};
            Color buttonColor{ 0.12f, 0.23f, 0.32f, 0.96f };
            Color pushedButtonColor{};
            bool hasPushedButtonColor{};

            bool comboActive{};
            float comboX{}, comboY{}, comboWidth{};
            int comboRow{};
            int comboScroll{};
            std::uint64_t comboId{};
            std::unordered_map<std::uint64_t, int> comboScrollById;
            std::size_t comboBackgroundIndex{};
            Rect comboClip{};
        };

        Context context;

        std::uint64_t hash_id(std::string_view text, std::uint64_t seed = 1469598103934665603ull)
        {
            auto hash = seed;
            for (const auto character : text)
            {
                hash ^= static_cast<unsigned char>(character);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        std::string_view visible_label(const char* label)
        {
            const std::string_view text = label ? label : "";
            const auto hidden = text.find("##");
            return hidden == std::string_view::npos ? text : text.substr(0, hidden);
        }

        bool contains(const Rect& rect, float x, float y)
        {
            return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
        }

        phoenix::renderer::ScreenUiCommand command(
            phoenix::renderer::ScreenUiCommandKind kind, Rect rect, Color color,
            std::string text = {}, Rect clip = {})
        {
            phoenix::renderer::ScreenUiCommand output{};
            output.kind = kind;
            output.x = rect.x; output.y = rect.y; output.width = rect.w; output.height = rect.h;
            output.color[0] = color.r; output.color[1] = color.g;
            output.color[2] = color.b; output.color[3] = color.a;
            output.text = std::move(text);
            output.clipX = clip.x; output.clipY = clip.y;
            output.clipWidth = clip.w; output.clipHeight = clip.h;
            return output;
        }

        void add_rect(Rect rect, Color color, Rect clip = {})
        {
            context.sink->push_back(command(
                phoenix::renderer::ScreenUiCommandKind::Rectangle, rect, color, {}, clip));
        }

        void add_text(float x, float y, std::string_view text, Color color, Rect clip = {})
        {
            context.sink->push_back(command(
                phoenix::renderer::ScreenUiCommandKind::Text,
                { x, y, 0.0f, kTextHeight }, color, std::string(text), clip));
        }

        std::string formatted(const char* format, va_list args)
        {
            char stack[512]{};
            va_list copy;
            va_copy(copy, args);
            const auto length = std::vsnprintf(stack, sizeof(stack), format, copy);
            va_end(copy);
            if (length < 0)
                return {};
            if (length < static_cast<int>(sizeof(stack)))
                return std::string(stack, static_cast<std::size_t>(length));
            std::string result(static_cast<std::size_t>(length), '\0');
            std::vsnprintf(result.data(), result.size() + 1, format, args);
            return result;
        }

        Rect place_item(float width, float height)
        {
            Rect item{};
            if (context.comboActive)
            {
                item = { context.comboX, context.comboY + context.comboRow * kItemHeight,
                    context.comboWidth, kItemHeight };
                ++context.comboRow;
                return item;
            }

            if (context.sameLine)
            {
                item.x = context.sameLineOffset > 0.0f
                    ? context.contentX + context.sameLineOffset
                    : context.lastItem.x + context.lastItem.w + context.sameLineSpacing;
                item.y = context.lastItem.y;
                context.sameLine = false;
            }
            else
            {
                item.x = context.contentX;
                item.y = context.cursorY;
            }
            item.w = width;
            item.h = height;
            context.cursorY = std::max(context.cursorY, item.y + height + kSpacing);
            context.logicalBottom = std::max(context.logicalBottom, context.cursorY
                + (context.activeWindow ? context.activeWindow->scroll : 0.0f));
            context.lastItem = item;
            return item;
        }

        float item_width(float fallback)
        {
            const auto width = context.nextItemWidth > 0.0f ? context.nextItemWidth : fallback;
            context.nextItemWidth = 0.0f;
            return width;
        }

        std::uint64_t item_id(const char* label)
        {
            return hash_id(label ? label : "", context.windowId);
        }

        bool interact(std::uint64_t id, const Rect& item)
        {
            const bool hovered = contains(item, context.mouseX, context.mouseY);
            if (hovered)
            {
                context.hotId = id;
                context.captureMouse = true;
            }
            if (hovered && context.mousePressed)
            {
                context.activeId = id;
                return true;
            }
            if (!context.mouseDown && context.activeId == id)
                context.activeId = 0;
            return false;
        }

        void text_line(std::string text, Color color, bool bullet)
        {
            const auto item = place_item(
                context.activeWindow ? context.activeWindow->width - kPadding * 2.0f : 240.0f,
                kTextHeight);
            if (bullet)
            {
                add_rect({ item.x + 1.0f, item.y + 6.0f, 4.0f, 4.0f }, color, context.contentClip);
                add_text(item.x + 10.0f, item.y, text, color, context.contentClip);
            }
            else
            {
                add_text(item.x, item.y, text, color, context.contentClip);
            }
        }
    }

    void begin_frame(const phoenix::platform::SdlWindow& window, float width, float height)
    {
        context.window = &window;
        context.surfaceWidth = width;
        context.surfaceHeight = height;
        const auto mouse = window.mouse_position();
        context.mouseX = static_cast<float>(mouse.first);
        context.mouseY = static_cast<float>(mouse.second);
        context.wheel = static_cast<float>(window.mouse_wheel_delta()) / 120.0f;
        context.mouseDown = window.is_mouse_button_down(0);
        context.mousePressed = window.mouse_button_pressed(0);
        context.captureMouse = false;
        context.captureKeyboard = false;
        context.background.clear();
        context.commands.clear();
        context.overlay.clear();
        context.sink = &context.commands;
        context.hotId = 0;
        context.activeWindow = nullptr;
        context.hasNextWindowPos = false;
        context.hasNextWindowSize = false;
        context.nextWindowAlpha = 0.90f;
        for (const auto& [id, state] : context.windows)
        {
            (void)id;
            if (contains({ state.x, state.y, state.width, state.height }, context.mouseX, context.mouseY))
            {
                context.captureMouse = true;
                break;
            }
        }
    }

    std::vector<phoenix::renderer::ScreenUiCommand> end_frame()
    {
        std::vector<phoenix::renderer::ScreenUiCommand> result;
        result.reserve(context.background.size() + context.commands.size() + context.overlay.size());
        result.insert(result.end(), context.background.begin(), context.background.end());
        result.insert(result.end(), context.commands.begin(), context.commands.end());
        result.insert(result.end(), context.overlay.begin(), context.overlay.end());
        return result;
    }

    bool wants_mouse() { return context.captureMouse || context.activeId != 0 || context.draggingWindow != 0; }
    bool wants_keyboard() { return context.captureKeyboard; }

    void set_window_position(float x, float y)
    {
        if (context.windows.empty())
            context.windows[hash_id("Phoenix Engine")].x = x,
            context.windows[hash_id("Phoenix Engine")].y = y;
        else
        {
            auto& state = context.windows[hash_id("Phoenix Engine")];
            state.x = x; state.y = y; state.positioned = true;
        }
    }

    Vec2 window_position()
    {
        const auto found = context.windows.find(hash_id("Phoenix Engine"));
        return found == context.windows.end() ? Vec2{ 8.0f, 8.0f } : Vec2{ found->second.x, found->second.y };
    }

    void SetNextWindowPos(Vec2 position, Condition condition)
    {
        context.nextWindowPos = position;
        context.nextWindowCondition = condition;
        context.hasNextWindowPos = true;
    }

    void SetNextWindowSize(Vec2 size, Condition)
    {
        context.nextWindowSize = size;
        context.hasNextWindowSize = true;
    }

    void SetNextWindowBgAlpha(float alpha) { context.nextWindowAlpha = alpha; }

    bool Begin(const char* title, bool*, std::uint32_t flags)
    {
        context.windowId = hash_id(title ? title : "");
        auto& state = context.windows[context.windowId];
        if (context.hasNextWindowPos && (context.nextWindowCondition == Always || !state.positioned))
        {
            state.x = context.nextWindowPos.x;
            state.y = context.nextWindowPos.y;
            state.positioned = true;
        }
        if (context.hasNextWindowSize)
        {
            if (context.nextWindowSize.x > 0.0f) state.width = context.nextWindowSize.x;
            if (context.nextWindowSize.y > 0.0f) state.height = context.nextWindowSize.y;
        }
        state.width = std::max(120.0f, state.width);
        state.x = std::clamp(state.x, 0.0f, std::max(0.0f, context.surfaceWidth - state.width));
        state.y = std::clamp(state.y, 0.0f, std::max(0.0f, context.surfaceHeight - 40.0f));

        const bool titleBar = (flags & NoTitleBar) == 0;
        const float requestedHeight = context.hasNextWindowSize && context.nextWindowSize.y > 0.0f
            ? context.nextWindowSize.y : state.height;
        const Rect windowRect{ state.x, state.y, state.width, requestedHeight };
        if (contains(windowRect, context.mouseX, context.mouseY))
            context.captureMouse = true;

        if (titleBar && (flags & NoMove) == 0)
        {
            const Rect titleRect{ state.x, state.y, state.width, kTitleHeight };
            if (contains(titleRect, context.mouseX, context.mouseY) && context.mousePressed)
            {
                context.draggingWindow = context.windowId;
                context.dragOffsetX = context.mouseX - state.x;
                context.dragOffsetY = context.mouseY - state.y;
            }
            if (context.draggingWindow == context.windowId)
            {
                if (context.mouseDown)
                {
                    state.x = std::clamp(context.mouseX - context.dragOffsetX, 0.0f,
                        std::max(0.0f, context.surfaceWidth - state.width));
                    state.y = std::clamp(context.mouseY - context.dragOffsetY, 0.0f,
                        std::max(0.0f, context.surfaceHeight - 40.0f));
                }
                else
                {
                    context.draggingWindow = 0;
                }
            }
        }

        context.activeWindow = &state;
        context.windowBackgroundIndex = context.commands.size();
        add_rect({ state.x, state.y, state.width, requestedHeight },
            { 0.035f, 0.045f, 0.055f, context.nextWindowAlpha });
        if (titleBar)
        {
            add_rect({ state.x, state.y, state.width, kTitleHeight }, { 0.055f, 0.075f, 0.09f, 0.96f });
            add_text(state.x + kPadding, state.y + 4.0f, visible_label(title), { 0.94f, 0.96f, 0.98f, 1.0f });
        }

        const float contentTop = state.y + (titleBar ? kTitleHeight : 0.0f) + kPadding;
        context.contentX = state.x + kPadding;
        context.contentClip = { state.x + 1.0f, contentTop - 1.0f,
            state.width - 2.0f, std::max(1.0f, requestedHeight - (contentTop - state.y) - 1.0f) };
        if ((flags & NoScrollbar) == 0 && contains(windowRect, context.mouseX, context.mouseY)
            && context.wheel != 0.0f)
            state.scroll = std::max(0.0f, state.scroll - context.wheel * 28.0f);
        context.cursorY = contentTop - state.scroll;
        context.logicalBottom = contentTop;
        context.lastItem = {};
        context.hasNextWindowPos = false;
        context.hasNextWindowSize = false;
        return true;
    }

    void End()
    {
        if (!context.activeWindow)
            return;
        const float naturalHeight = std::max(42.0f,
            context.logicalBottom - context.activeWindow->y + kPadding);
        context.activeWindow->height = std::min(naturalHeight,
            std::max(60.0f, context.surfaceHeight - context.activeWindow->y - 8.0f));
        auto& background = context.commands[context.windowBackgroundIndex];
        background.height = context.activeWindow->height;
        const float maxScroll = std::max(0.0f, naturalHeight - context.activeWindow->height);
        context.activeWindow->scroll = std::min(context.activeWindow->scroll, maxScroll);
        context.activeWindow = nullptr;
    }

    void SetNextItemWidth(float width) { context.nextItemWidth = width; }

    bool Button(const char* label, Vec2 requested)
    {
        const auto visible = visible_label(label);
        const float width = requested.x > 0.0f ? requested.x
            : item_width(std::max(54.0f, static_cast<float>(visible.size()) * 7.2f + 16.0f));
        const float height = requested.y > 0.0f ? requested.y : kItemHeight;
        const auto item = place_item(width, height);
        const auto id = item_id(label);
        const bool clicked = interact(id, item);
        const bool hovered = contains(item, context.mouseX, context.mouseY);
        const auto base = context.hasPushedButtonColor ? context.pushedButtonColor : context.buttonColor;
        const Color color = clicked || context.activeId == id
            ? Color{ 0.16f, 0.42f, 0.61f, 0.98f }
            : hovered ? Color{ base.r + 0.05f, base.g + 0.06f, base.b + 0.06f, base.a } : base;
        const Rect clip = context.comboActive ? context.comboClip : context.contentClip;
        add_rect(item, color, clip);
        add_rect({ item.x, item.y, item.w, 1.0f }, { 0.42f, 0.56f, 0.66f, 0.72f }, clip);
        add_text(item.x + 8.0f, item.y + 3.0f, visible, { 0.94f, 0.96f, 0.98f, 1.0f }, clip);
        return clicked;
    }

    bool Checkbox(const char* label, bool* value)
    {
        const auto visible = visible_label(label);
        const auto item = place_item(std::max(100.0f, static_cast<float>(visible.size()) * 7.2f + 28.0f), kItemHeight);
        const auto id = item_id(label);
        const bool clicked = interact(id, item);
        if (clicked && value) *value = !*value;
        add_rect({ item.x, item.y + 3.0f, 16.0f, 16.0f }, { 0.08f, 0.11f, 0.13f, 0.96f }, context.contentClip);
        if (value && *value)
            add_rect({ item.x + 3.0f, item.y + 6.0f, 10.0f, 10.0f }, { 0.16f, 0.62f, 0.88f, 1.0f }, context.contentClip);
        add_text(item.x + 23.0f, item.y + 3.0f, visible, { 0.92f, 0.94f, 0.96f, 1.0f }, context.contentClip);
        return clicked;
    }

    bool SliderFloat(const char* label, float* value, float minimum, float maximum, const char* format)
    {
        const float width = item_width(220.0f);
        const auto item = place_item(width, kItemHeight + kTextHeight);
        const Rect track{ item.x, item.y, item.w, kItemHeight };
        const auto id = item_id(label);
        interact(id, track);
        bool changed = false;
        if (value && context.activeId == id && context.mouseDown)
        {
            const auto previous = *value;
            *value = minimum + std::clamp((context.mouseX - track.x) / std::max(1.0f, track.w), 0.0f, 1.0f)
                * (maximum - minimum);
            changed = previous != *value;
        }
        const float fraction = value ? std::clamp((*value - minimum) / std::max(0.0001f, maximum - minimum), 0.0f, 1.0f) : 0.0f;
        add_rect(track, { 0.07f, 0.10f, 0.12f, 0.96f }, context.contentClip);
        add_rect({ track.x, track.y, track.w * fraction, track.h }, { 0.10f, 0.43f, 0.66f, 0.95f }, context.contentClip);
        char valueText[64]{};
        std::snprintf(valueText, sizeof(valueText), format, value ? *value : 0.0f);
        add_text(track.x + 6.0f, track.y + 3.0f, valueText, { 0.96f, 0.97f, 0.98f, 1.0f }, context.contentClip);
        add_text(item.x, item.y + kItemHeight + 1.0f, visible_label(label), { 0.72f, 0.77f, 0.81f, 1.0f }, context.contentClip);
        return changed;
    }

    bool InputInt(const char* label, int* value)
    {
        const float width = item_width(80.0f);
        const auto item = place_item(width + (visible_label(label).empty() ? 0.0f : 92.0f), kItemHeight);
        const Rect field{ item.x, item.y, width, item.h };
        const auto id = item_id(label);
        bool changed = false;
        if (interact(id, field) && value)
        {
            const int step = context.window && context.window->is_key_down(SDLK_LSHIFT) ? 10 : 1;
            *value += context.mouseX < field.x + field.w * 0.5f ? -step : step;
            changed = true;
        }
        if (contains(field, context.mouseX, context.mouseY) && context.wheel != 0.0f && value)
        {
            *value += static_cast<int>(context.wheel);
            changed = true;
        }
        add_rect(field, { 0.07f, 0.10f, 0.12f, 0.96f }, context.contentClip);
        char number[48]{};
        std::snprintf(number, sizeof(number), "-  %d  +", value ? *value : 0);
        add_text(field.x + 5.0f, field.y + 3.0f, number, { 0.94f, 0.96f, 0.98f, 1.0f }, context.contentClip);
        if (!visible_label(label).empty())
            add_text(field.x + width + 7.0f, field.y + 3.0f, visible_label(label), { 0.78f, 0.82f, 0.85f, 1.0f }, context.contentClip);
        return changed;
    }

    bool DragFloat3(const char* label, float value[3], float speed, float minimum, float maximum)
    {
        bool changed = false;
        TextUnformatted(label);
        const float total = item_width(200.0f);
        const float componentWidth = (total - 8.0f) / 3.0f;
        for (int component = 0; component < 3; ++component)
        {
            if (component > 0) SameLine();
            const std::string componentLabel = std::string("##") + label + std::to_string(component);
            SetNextItemWidth(componentWidth);
            const auto item = place_item(componentWidth, kItemHeight);
            const auto id = item_id(componentLabel.c_str());
            interact(id, item);
            static float previousMouseX{};
            if (context.activeId == id && context.mousePressed)
                previousMouseX = context.mouseX;
            if (context.activeId == id && context.mouseDown)
            {
                const float delta = context.mouseX - previousMouseX;
                if (delta != 0.0f)
                {
                    value[component] += delta * speed;
                    if (maximum > minimum)
                        value[component] = std::clamp(value[component], minimum, maximum);
                    previousMouseX = context.mouseX;
                    changed = true;
                }
            }
            add_rect(item, { 0.07f, 0.10f, 0.12f, 0.96f }, context.contentClip);
            char number[32]{};
            std::snprintf(number, sizeof(number), "%.2f", value[component]);
            add_text(item.x + 4.0f, item.y + 3.0f, number, { 0.93f, 0.95f, 0.97f, 1.0f }, context.contentClip);
        }
        return changed;
    }

    bool Combo(const char* label, int* current, const char* const items[], int count)
    {
        if (!current || count <= 0)
            return false;
        *current = std::clamp(*current, 0, count - 1);
        bool changed = false;
        if (BeginCombo(label, items[*current]))
        {
            for (int index = 0; index < count; ++index)
                if (Selectable(items[index], index == *current))
                    *current = index, changed = true;
            EndCombo();
        }
        return changed;
    }

    bool BeginCombo(const char* label, const char* preview)
    {
        const float width = item_width(180.0f);
        const auto item = place_item(width, kItemHeight + kTextHeight);
        const Rect button{ item.x, item.y, width, kItemHeight };
        const auto id = item_id(label);
        if (interact(id, button))
            context.openCombo = context.openCombo == id ? 0 : id;
        add_rect(button, { 0.07f, 0.12f, 0.16f, 0.98f }, context.contentClip);
        add_text(button.x + 6.0f, button.y + 3.0f, preview ? preview : "", { 0.94f, 0.96f, 0.98f, 1.0f }, context.contentClip);
        add_text(button.x + button.w - 14.0f, button.y + 3.0f, "v", { 0.65f, 0.78f, 0.87f, 1.0f }, context.contentClip);
        add_text(item.x, item.y + kItemHeight + 1.0f, visible_label(label), { 0.72f, 0.77f, 0.81f, 1.0f }, context.contentClip);
        if (context.openCombo != id)
            return false;

        context.comboActive = true;
        context.comboX = button.x;
        context.comboY = button.y + button.h;
        context.comboWidth = button.w;
        context.comboRow = 0;
        context.comboId = id;
        context.comboScroll = context.comboScrollById[id];
        context.comboClip = { button.x, button.y + button.h, button.w, 242.0f };
        if (contains(context.comboClip, context.mouseX, context.mouseY) && context.wheel != 0.0f)
            context.comboScroll = std::max(0, context.comboScroll - static_cast<int>(context.wheel * 3.0f));
        context.sink = &context.overlay;
        context.comboBackgroundIndex = context.overlay.size();
        add_rect(context.comboClip, { 0.025f, 0.04f, 0.055f, 0.985f }, context.comboClip);
        return true;
    }

    bool Selectable(const char* label, bool selected)
    {
        const int logicalRow = context.comboRow++;
        const int visibleRow = logicalRow - context.comboScroll;
        if (visibleRow < 0 || visibleRow >= 11)
            return false;
        const Rect item{ context.comboX, context.comboY + visibleRow * kItemHeight,
            context.comboWidth, kItemHeight };
        const auto id = hash_id(std::to_string(logicalRow), item_id(label));
        const bool clicked = interact(id, item);
        const bool hovered = contains(item, context.mouseX, context.mouseY);
        if (selected || hovered)
            add_rect(item, selected ? Color{ 0.10f, 0.35f, 0.52f, 0.96f }
                : Color{ 0.10f, 0.18f, 0.24f, 0.96f }, context.comboClip);
        add_text(item.x + 6.0f, item.y + 3.0f, visible_label(label), { 0.93f, 0.95f, 0.97f, 1.0f }, context.comboClip);
        if (clicked)
            context.openCombo = 0;
        return clicked;
    }

    void EndCombo()
    {
        const int maxScroll = std::max(0, context.comboRow - 11);
        context.comboScroll = std::clamp(context.comboScroll, 0, maxScroll);
        context.comboScrollById[context.comboId] = context.comboScroll;
        const float height = std::min(242.0f, std::min(context.comboRow, 11) * kItemHeight + 2.0f);
        context.overlay[context.comboBackgroundIndex].height = height;
        context.comboActive = false;
        context.sink = &context.commands;
    }

    void SetItemDefaultFocus() {}

    void SameLine(float offsetFromStart, float spacing)
    {
        context.sameLine = true;
        context.sameLineOffset = offsetFromStart;
        context.sameLineSpacing = spacing;
    }

    void Separator()
    {
        const auto item = place_item(context.activeWindow ? context.activeWindow->width - kPadding * 2.0f : 240.0f, 5.0f);
        add_rect({ item.x, item.y + 2.0f, item.w, 1.0f }, { 0.26f, 0.34f, 0.39f, 0.75f }, context.contentClip);
    }

    void Spacing() { place_item(1.0f, 5.0f); }
    void TextUnformatted(const char* text) { text_line(text ? text : "", { 0.94f, 0.96f, 0.98f, 1.0f }, false); }

    void Text(const char* format, ...)
    {
        va_list args; va_start(args, format); auto output = formatted(format, args); va_end(args);
        text_line(std::move(output), { 0.94f, 0.96f, 0.98f, 1.0f }, false);
    }

    void TextDisabled(const char* format, ...)
    {
        va_list args; va_start(args, format); auto output = formatted(format, args); va_end(args);
        text_line(std::move(output), { 0.52f, 0.58f, 0.62f, 1.0f }, false);
    }

    void TextColored(Color color, const char* format, ...)
    {
        va_list args; va_start(args, format); auto output = formatted(format, args); va_end(args);
        text_line(std::move(output), color, false);
    }

    void BulletText(const char* format, ...)
    {
        va_list args; va_start(args, format); auto output = formatted(format, args); va_end(args);
        text_line(std::move(output), { 0.90f, 0.93f, 0.95f, 1.0f }, true);
    }

    void PushStyleColor(StyleColor, Color color)
    {
        context.pushedButtonColor = color;
        context.hasPushedButtonColor = true;
    }

    void PopStyleColor() { context.hasPushedButtonColor = false; }
    Color GetStyleColor(StyleColor index)
    {
        return index == ButtonActiveColor ? Color{ 0.16f, 0.42f, 0.61f, 0.98f } : context.buttonColor;
    }

    void background_rect(Vec2 minimum, Vec2 maximum, Color color)
    {
        context.background.push_back(command(phoenix::renderer::ScreenUiCommandKind::Rectangle,
            { minimum.x, minimum.y, maximum.x - minimum.x, maximum.y - minimum.y }, color));
    }
}
