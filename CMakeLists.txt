cmake_minimum_required(VERSION 3.20)
project(PhoenixEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

option(PHOENIX_COPY_RUNTIME_DLLS "Copy runtime DLLs next to the executable on Windows" ON)

set(ENGINE_SOURCES
    src/main.cpp
    src/assets/data_index.cpp
    src/audio/audio_system.cpp
    src/character/character_system.cpp
    src/character/weapon_bone_map.cpp
    src/character/weapon_effect.cpp
    src/core/logging.cpp
    src/effects/effect_system.cpp
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

if(WIN32)
    list(APPEND ENGINE_SOURCES res/phoenix.rc)
endif()

add_executable(PhoenixEngine ${ENGINE_SOURCES})

target_include_directories(PhoenixEngine PRIVATE
    src
    external
    external/imgui
    external/imgui/backends
    external/volk
    external/Vulkan-Headers/include
)

target_compile_definitions(PhoenixEngine PRIVATE VK_NO_PROTOTYPES)

# --- Platform-specific configuration ---

if(WIN32)
    # SDL2 from vendored copy in external/SDL2/.
    set(SDL2_DIR "${CMAKE_SOURCE_DIR}/external/SDL2/cmake")
    find_package(SDL2 REQUIRED CONFIG)

    target_include_directories(PhoenixEngine PRIVATE
        "${CMAKE_SOURCE_DIR}/external/SDL2/include"
    )
    target_link_directories(PhoenixEngine PRIVATE
        "${CMAKE_SOURCE_DIR}/external/SDL2/lib/x64"
    )
    target_link_libraries(PhoenixEngine PRIVATE
        SDL2::SDL2
        SDL2::SDL2main
        user32
        gdi32
    )

    target_compile_definitions(PhoenixEngine PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        IMGUI_IMPL_VULKAN_USE_VOLK
    )

    # Match the vcxproj warning level and suppression.
    if(MSVC)
        target_compile_options(PhoenixEngine PRIVATE /W4 /wd4505)
    endif()

    # Subsystem: Windows (no console window).
    if(MSVC)
        set_target_properties(PhoenixEngine PROPERTIES
            WIN32_EXECUTABLE TRUE
        )
    endif()

    # Output layout matching the vcxproj: bin/<Platform>/<Configuration>/
    set_target_properties(PhoenixEngine PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/x64/$<CONFIG>"
    )

    # Copy SDL2.dll next to the executable.
    if(PHOENIX_COPY_RUNTIME_DLLS)
        add_custom_command(TARGET PhoenixEngine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/external/SDL2/lib/x64/SDL2.dll"
                "$<TARGET_FILE_DIR:PhoenixEngine>/SDL2.dll"
        )
    endif()

else()
    # Linux / other POSIX: SDL2 via pkg-config.
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

    target_link_libraries(PhoenixEngine PRIVATE
        PkgConfig::SDL2
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )

    target_compile_options(PhoenixEngine PRIVATE -Wno-missing-field-initializers)
endif()
