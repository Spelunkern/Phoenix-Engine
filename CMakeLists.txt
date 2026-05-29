cmake_minimum_required(VERSION 3.20)
project(PhoenixEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

set(ENGINE_SOURCES
    src/main.cpp
    src/assets/data_index.cpp
    src/audio/audio_system.cpp
    src/character/character_system.cpp
    src/character/weapon_effect.cpp
    src/core/logging.cpp
    src/platform/sdl_window.cpp
    src/renderer/dds_loader.cpp
    src/renderer/vulkan_renderer.cpp
    src/renderer/vulkan_renderer_particles.cpp
    src/runtime/phoenix_runtime.cpp
    src/ui/editor_panel.cpp
    src/ui/perf_hud.cpp
    src/world/actor_scene.cpp
    src/world/character_loader.cpp
    src/world/dg_loader.cpp
    src/world/eft_loader.cpp
    src/world/mani_loader.cpp
    src/world/mon_loader.cpp
    src/world/sdata_loader.cpp
    src/world/smod_loader.cpp
    src/world/svmap_loader.cpp
    src/world/vani_loader.cpp
    src/world/wld_loader.cpp
    external/volk/volk.c
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_sdl2.cpp
    external/imgui/backends/imgui_impl_vulkan.cpp
)

add_executable(PhoenixEngine ${ENGINE_SOURCES})

target_include_directories(PhoenixEngine PRIVATE
    src
    external
    external/imgui
    external/imgui/backends
    external/volk
    external/Vulkan-Headers/include
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(PhoenixEngine PRIVATE
    ${SDL2_LIBRARIES}
    dl
    pthread
)

target_compile_definitions(PhoenixEngine PRIVATE
    VK_NO_PROTOTYPES
)

target_compile_options(PhoenixEngine PRIVATE
    -Wno-missing-field-initializers
)
