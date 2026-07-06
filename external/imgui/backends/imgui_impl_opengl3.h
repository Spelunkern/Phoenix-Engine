#pragma once

// Minimal hand-written Dear ImGui OpenGL3 backend (core profile 4.5), written
// for this project since the vendored imgui/backends tree only ships the
// Vulkan/DX12/Win32/SDL2 backends. Uses the project's own gl_loader.h function
// pointers (renderer/gl/gl_loader.h) rather than a second GL loader.

#include "imgui.h"

IMGUI_IMPL_API bool ImGui_ImplOpenGL3_Init(const char* glsl_version = nullptr);
IMGUI_IMPL_API void ImGui_ImplOpenGL3_Shutdown();
IMGUI_IMPL_API void ImGui_ImplOpenGL3_NewFrame();
IMGUI_IMPL_API void ImGui_ImplOpenGL3_RenderDrawData(ImDrawData* draw_data);
