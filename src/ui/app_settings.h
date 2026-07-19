#pragma once

namespace phoenix::ui
{
    // Persists small app-level settings (character shadow toggle, perf HUD fps
    // cap) as a custom section inside the shared imgui.ini instead of separate
    // display.ini / perf_hud.ini files. Call once, right after
    // OpenGLRenderer::initialize_imgui() and before anything reads these
    // values (the initial character mesh build happens during the loading
    // sequence, well before the first ImGui frame) — this loads the ini
    // immediately rather than waiting for the automatic on-first-NewFrame load.
    void register_app_settings(bool& characterShadow, int& fpsCapIndex, bool& antialiasingEnabled);

    // Forces an immediate write to imgui.ini. Needed on hard-exit paths
    // (std::_Exit after closing mid-load, or on normal quit) that skip
    // ImGui::DestroyContext() and its automatic on-shutdown save.
    void flush_app_settings();
}
