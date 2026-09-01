#include "renderer/opengl_renderer.h"
#include "renderer/opengl_renderer_internal.h"
#include "renderer/dds_loader.h"
#include "platform/sdl_window.h"
#include "ui/cpu_profiler.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif

namespace phoenix::renderer
{
    namespace
    {
        struct WorldLabelVertex
        {
            float position[2]{};
            float uv[2]{};
            float color[4]{};
            float textured{ 1.0f };
        };

        void* sdl_gl_get_proc(const char* name)
        {
            return SDL_GL_GetProcAddress(name);
        }

        GLuint compile_shader(GLenum stage, const std::string& source)
        {
            GLuint shader = glCreateShader_(stage);
            const char* src = source.c_str();
            GLint len = static_cast<GLint>(source.size());
            glShaderSource_(shader, 1, &src, &len);
            glCompileShader_(shader);
            GLint status = 0;
            glGetShaderiv_(shader, GL_COMPILE_STATUS, &status);
            if (!status)
            {
                glDeleteShader_(shader);
                return 0;
            }
            return shader;
        }

        // Textual "include": every GLSL source below has a CAMERA_BLOCK marker
        // line replaced with the shared uniform-block declaration, since this
        // hand-rolled loader has no shader preprocessor include support.
        std::string inject_camera_block(const std::string& source, const std::string& block)
        {
            std::string result = source;
            const std::string marker = "CAMERA_BLOCK";
            auto pos = result.find(marker);
            if (pos != std::string::npos)
                result.replace(pos, marker.size(), block);
            return result;
        }

        std::string inject_environment_functions(const std::string& source, const std::string& functions)
        {
            std::string result = source;
            const std::string marker = "ENVIRONMENT_FUNCTIONS";
            const auto pos = result.find(marker);
            if (pos != std::string::npos)
                result.replace(pos, marker.size(), functions);
            return result;
        }

        bool read_text_file(const std::filesystem::path& relativePath, std::string& out)
        {
            const auto cwd = std::filesystem::current_path();
            const auto exeDir = executable_dir();
            std::vector<std::filesystem::path> candidates;
            candidates.reserve(12);
            const auto append_ancestors = [&](std::filesystem::path directory) {
                for (int depth = 0; depth < 6 && !directory.empty(); ++depth)
                {
                    candidates.push_back(directory / relativePath);
                    const auto parent = directory.parent_path();
                    if (parent == directory)
                        break;
                    directory = parent;
                }
            };
            // File-manager launches inherit an arbitrary working directory.
            // Search both it and the executable's ancestry: development
            // binaries live under source/build/<configuration>, while a
            // distributed binary has shaders/ directly beside it.
            append_ancestors(exeDir);
            append_ancestors(cwd);
            for (const auto& candidate : candidates)
            {
                std::ifstream input(candidate, std::ios::binary | std::ios::ate);
                if (!input)
                    continue;
                const auto size = input.tellg();
                input.seekg(0, std::ios::beg);
                out.resize(static_cast<std::size_t>(size));
                input.read(out.data(), static_cast<std::streamsize>(out.size()));
                return true;
            }
            return false;
        }

        GLuint build_program(const char* vertRelPath, const char* fragRelPath)
        {
            std::string vertSrc, fragSrc, cameraBlock, environmentFunctions;
            if (!read_text_file(vertRelPath, vertSrc) || !read_text_file(fragRelPath, fragSrc))
                return 0;
            if (!read_text_file("shaders/gl/common_camera.glsl", cameraBlock))
                return 0;

            vertSrc = inject_camera_block(vertSrc, cameraBlock);
            fragSrc = inject_camera_block(fragSrc, cameraBlock);
            if (vertSrc.find("ENVIRONMENT_FUNCTIONS") != std::string::npos
                || fragSrc.find("ENVIRONMENT_FUNCTIONS") != std::string::npos)
            {
                if (!read_text_file("shaders/gl/common_environment.glsl", environmentFunctions))
                    return 0;
                vertSrc = inject_environment_functions(vertSrc, environmentFunctions);
                fragSrc = inject_environment_functions(fragSrc, environmentFunctions);
            }

            GLuint vs = compile_shader(GL_VERTEX_SHADER, vertSrc);
            GLuint fs = vs ? compile_shader(GL_FRAGMENT_SHADER, fragSrc) : 0;
            if (!vs || !fs)
            {
                if (vs) glDeleteShader_(vs);
                if (fs) glDeleteShader_(fs);
                return 0;
            }

            GLuint program = glCreateProgram_();
            glAttachShader_(program, vs);
            glAttachShader_(program, fs);
            glLinkProgram_(program);
            GLint status = 0;
            glGetProgramiv_(program, GL_LINK_STATUS, &status);
            glDeleteShader_(vs);
            glDeleteShader_(fs);
            if (!status)
            {
                glDeleteProgram_(program);
                return 0;
            }

            GLuint blockIndex = glGetUniformBlockIndex_(program, "CameraConstants");
            if (blockIndex != 0xFFFFFFFFu)
                glUniformBlockBinding_(program, blockIndex, 0);
            return program;
        }

        constexpr std::size_t kCameraConstantFloatCount = 144;

        void build_directional_shadow_matrix(float (&matrix)[16],
            const float* camera, const float* lightDirection, float farPlane)
        {
            const auto normalize3 = [](float (&value)[3]) {
                const auto length = std::sqrt(value[0] * value[0] + value[1] * value[1]
                    + value[2] * value[2]);
                const auto inverse = length > 0.00001f ? 1.0f / length : 1.0f;
                value[0] *= inverse; value[1] *= inverse; value[2] *= inverse;
            };
            const auto cross3 = [](const float* a, const float* b, float (&result)[3]) {
                result[0] = a[1] * b[2] - a[2] * b[1];
                result[1] = a[2] * b[0] - a[0] * b[2];
                result[2] = a[0] * b[1] - a[1] * b[0];
            };
            const auto dot3 = [](const float* a, const float* b) {
                return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            };

            float forward[3]{ -lightDirection[0], -lightDirection[1], -lightDirection[2] };
            normalize3(forward);
            float worldUp[3]{ 0.0f, 1.0f, 0.0f };
            float right[3]{};
            cross3(forward, worldUp, right);
            if (std::abs(right[0]) + std::abs(right[1]) + std::abs(right[2]) < 0.001f)
            {
                worldUp[0] = 0.0f; worldUp[1] = 0.0f; worldUp[2] = 1.0f;
                cross3(forward, worldUp, right);
            }
            normalize3(right);
            float up[3]{};
            cross3(right, forward, up);
            normalize3(up);

            const auto radius = std::clamp(farPlane * 0.58f, 220.0f, 850.0f);
            const auto depth = radius * 2.4f;
            float center[3]{
                camera[0] + camera[9] * radius * 0.28f,
                camera[1] - 35.0f,
                camera[2] + camera[8] * radius * 0.28f,
            };
            // Snap the light-space center to texel increments. This prevents
            // the directional map from swimming while the camera moves.
            const auto worldUnitsPerTexel = radius * 2.0f / 2048.0f;
            const auto rightCenter = std::round(dot3(right, center) / worldUnitsPerTexel)
                * worldUnitsPerTexel;
            const auto upCenter = std::round(dot3(up, center) / worldUnitsPerTexel)
                * worldUnitsPerTexel;
            const auto forwardCenter = dot3(forward, center);

            std::fill(matrix, matrix + 16, 0.0f);
            matrix[0] = right[0] / radius;
            matrix[4] = right[1] / radius;
            matrix[8] = right[2] / radius;
            matrix[12] = -rightCenter / radius;
            matrix[1] = up[0] / radius;
            matrix[5] = up[1] / radius;
            matrix[9] = up[2] / radius;
            matrix[13] = -upCenter / radius;
            matrix[2] = forward[0] / depth;
            matrix[6] = forward[1] / depth;
            matrix[10] = forward[2] / depth;
            matrix[14] = -forwardCenter / depth;
            matrix[15] = 1.0f;
        }

        std::uint32_t hash_grid(std::uint32_t x, std::uint32_t y, std::uint32_t seed)
        {
            std::uint32_t value = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
            value ^= value >> 13u;
            value *= 0x85ebca6bu;
            value ^= value >> 16u;
            return value;
        }

        float periodic_value_noise(float x, float y, std::uint32_t period, std::uint32_t seed)
        {
            const auto x0 = static_cast<std::uint32_t>(std::floor(x)) % period;
            const auto y0 = static_cast<std::uint32_t>(std::floor(y)) % period;
            const auto x1 = (x0 + 1u) % period;
            const auto y1 = (y0 + 1u) % period;
            auto random01 = [seed](std::uint32_t gx, std::uint32_t gy) {
                return static_cast<float>(hash_grid(gx, gy, seed) & 0x00ffffffu)
                    / static_cast<float>(0x00ffffffu);
            };
            auto tx = x - std::floor(x);
            auto ty = y - std::floor(y);
            tx = tx * tx * (3.0f - 2.0f * tx);
            ty = ty * ty * (3.0f - 2.0f * ty);
            const auto top = random01(x0, y0) + (random01(x1, y0) - random01(x0, y0)) * tx;
            const auto bottom = random01(x0, y1) + (random01(x1, y1) - random01(x0, y1)) * tx;
            return top + (bottom - top) * ty;
        }

        std::vector<std::uint8_t> make_environment_normal_noise(std::uint32_t side)
        {
            std::vector<float> height(static_cast<std::size_t>(side) * side);
            for (std::uint32_t y = 0; y < side; ++y)
            {
                for (std::uint32_t x = 0; x < side; ++x)
                {
                    float value = 0.0f;
                    float amplitude = 0.5f;
                    float totalAmplitude = 0.0f;
                    std::uint32_t period = 4u;
                    for (std::uint32_t octave = 0; octave < 4u; ++octave)
                    {
                        const auto fx = static_cast<float>(x) * static_cast<float>(period) / static_cast<float>(side);
                        const auto fy = static_cast<float>(y) * static_cast<float>(period) / static_cast<float>(side);
                        value += periodic_value_noise(fx, fy, period, 0x50484f45u + octave * 977u) * amplitude;
                        totalAmplitude += amplitude;
                        amplitude *= 0.5f;
                        period *= 2u;
                    }
                    height[static_cast<std::size_t>(y) * side + x] = value / totalAmplitude;
                }
            }

            std::vector<std::uint8_t> rgba(static_cast<std::size_t>(side) * side * 4u);
            const auto wrap = [side](int value) {
                value %= static_cast<int>(side);
                return static_cast<std::uint32_t>(value < 0 ? value + static_cast<int>(side) : value);
            };
            for (std::uint32_t y = 0; y < side; ++y)
            {
                for (std::uint32_t x = 0; x < side; ++x)
                {
                    const auto sample = [&](int sx, int sy) {
                        return height[static_cast<std::size_t>(wrap(sy)) * side + wrap(sx)];
                    };
                    const auto dx = sample(static_cast<int>(x) + 1, static_cast<int>(y))
                        - sample(static_cast<int>(x) - 1, static_cast<int>(y));
                    const auto dz = sample(static_cast<int>(x), static_cast<int>(y) + 1)
                        - sample(static_cast<int>(x), static_cast<int>(y) - 1);
                    float nx = -dx * 6.0f;
                    float ny = 1.0f;
                    float nz = -dz * 6.0f;
                    const auto inverseLength = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
                    nx *= inverseLength;
                    ny *= inverseLength;
                    nz *= inverseLength;
                    const auto offset = (static_cast<std::size_t>(y) * side + x) * 4u;
                    rgba[offset + 0u] = static_cast<std::uint8_t>(std::clamp(nx * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
                    rgba[offset + 1u] = static_cast<std::uint8_t>(std::clamp(nz * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
                    rgba[offset + 2u] = static_cast<std::uint8_t>(std::clamp(ny * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
                    rgba[offset + 3u] = static_cast<std::uint8_t>(std::clamp(height[static_cast<std::size_t>(y) * side + x], 0.0f, 1.0f) * 255.0f);
                }
            }
            return rgba;
        }

        void set_camera_ubo(GLuint ubo, const float* constants)
        {
            glNamedBufferSubData_(ubo, 0,
                sizeof(float) * kCameraConstantFloatCount, constants);
        }
    }

    OpenGLRenderer::~OpenGLRenderer()
    {
        shutdown();
    }

    bool OpenGLRenderer::initialize(SDL_Window* window, std::uint32_t width, std::uint32_t height)
    {
        impl_ = new Impl{};
        impl_->window = window;
        impl_->glContext = SDL_GL_GetCurrentContext();

        if (!phoenix::gl::load(sdl_gl_get_proc))
        {
            log_line("GL: failed to load required entry points");
            shutdown();
            return false;
        }

        impl_->surfaceWidth = width;
        impl_->surfaceHeight = height;

        const char* renderer = reinterpret_cast<const char*>(glGetString_(GL_RENDERER));
        impl_->adapterName = renderer ? renderer : "Unknown GL device";
        adapterName_ = impl_->adapterName;
        GLint maxArrayLayers = 0;
        glGetIntegerv_(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxArrayLayers);
        if (maxArrayLayers > 0)
            impl_->maxImageArrayLayers = static_cast<std::uint32_t>(maxArrayLayers);

        glEnable_(GL_DEPTH_TEST);
        glDepthFunc_(GL_LESS);
        glEnable_(GL_BLEND);
        glBlendEquation_(GL_FUNC_ADD);
        glBlendFunc_(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glViewport_(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));

        glCreateBuffers_(1, &impl_->cameraUbo);
        glNamedBufferData_(impl_->cameraUbo,
            sizeof(float) * kCameraConstantFloatCount, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase_(GL_UNIFORM_BUFFER, 0, impl_->cameraUbo);

        glCreateVertexArrays_(1, &impl_->emptyVao);

        if (!create_terrain_pipeline())
        {
            log_line("GL: terrain pipeline failed");
            shutdown();
            return false;
        }
        if (!create_static_object_pipeline())
        {
            log_line("GL: static object pipeline failed");
            shutdown();
            return false;
        }
        if (!create_skinned_character_pipeline())
            log_line("GL: skinned character pipeline unavailable (non-fatal)");
        if (!create_effect_particle_pipeline())
            log_line("GL: effect particle pipeline unavailable (non-fatal)");
        impl_->worldLabelProgram = build_program("shaders/gl/world_label.vert", "shaders/gl/world_label.frag");
        if (impl_->worldLabelProgram)
        {
            static constexpr int atlasWidth = 512;
            static constexpr int atlasHeight = 512;
            std::vector<std::uint8_t> fontBytes;
            const auto exeDir = executable_dir();
            const auto cwd = std::filesystem::current_path();
            for (const auto& fontPath : {
                exeDir / "assets/fonts/NotoSans-Regular.ttf",
                cwd / "assets/fonts/NotoSans-Regular.ttf",
                std::filesystem::path("C:\\Windows\\Fonts\\arial.ttf"),
                std::filesystem::path("C:\\Windows\\Fonts\\ARIAL.TTF"),
                std::filesystem::path("/usr/share/fonts/noto/NotoSans-Regular.ttf"),
                std::filesystem::path("/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"),
                std::filesystem::path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf") })
            {
                std::ifstream input(fontPath, std::ios::binary | std::ios::ate);
                if (!input)
                    continue;
                const auto size = input.tellg();
                if (size <= 0)
                    continue;
                fontBytes.resize(static_cast<std::size_t>(size));
                input.seekg(0, std::ios::beg);
                input.read(reinterpret_cast<char*>(fontBytes.data()), static_cast<std::streamsize>(fontBytes.size()));
                if (input)
                    break;
                fontBytes.clear();
            }

            std::vector<std::uint8_t> atlas(static_cast<std::size_t>(atlasWidth) * atlasHeight);
            std::array<stbtt_bakedchar, 224> baked{};
            if (!fontBytes.empty()
                && stbtt_BakeFontBitmap(fontBytes.data(), 0, 14.0f, atlas.data(),
                    atlasWidth, atlasHeight, 32, static_cast<int>(baked.size()), baked.data()) > 0)
            {
                for (std::size_t i = 0; i < baked.size(); ++i)
                {
                    const auto& source = baked[i];
                    auto& glyph = impl_->worldLabelGlyphs[i];
                    glyph.x0 = source.xoff;
                    glyph.y0 = source.yoff;
                    glyph.x1 = source.xoff + static_cast<float>(source.x1 - source.x0);
                    glyph.y1 = source.yoff + static_cast<float>(source.y1 - source.y0);
                    glyph.u0 = static_cast<float>(source.x0) / atlasWidth;
                    glyph.v0 = static_cast<float>(source.y0) / atlasHeight;
                    glyph.u1 = static_cast<float>(source.x1) / atlasWidth;
                    glyph.v1 = static_cast<float>(source.y1) / atlasHeight;
                    glyph.advance = source.xadvance;
                }

                glGenTextures_(1, &impl_->worldLabelTexture);
                glBindTexture_(GL_TEXTURE_2D, impl_->worldLabelTexture);
                glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D_(GL_TEXTURE_2D, 0, GL_R8, atlasWidth, atlasHeight, 0,
                    GL_RED, GL_UNSIGNED_BYTE, atlas.data());
                glBindTexture_(GL_TEXTURE_2D, 0);

                glCreateBuffers_(1, &impl_->worldLabelVertexBuffer.id);
                glNamedBufferData_(impl_->worldLabelVertexBuffer.id, 1, nullptr, GL_DYNAMIC_DRAW);
                impl_->worldLabelVertexBuffer.byteSize = 1;
                glCreateVertexArrays_(1, &impl_->worldLabelVao);
                glBindVertexArray_(impl_->worldLabelVao);
                glBindBuffer_(GL_ARRAY_BUFFER, impl_->worldLabelVertexBuffer.id);
                const auto stride = static_cast<GLsizei>(sizeof(WorldLabelVertex));
                glEnableVertexAttribArray_(0);
                glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, stride,
                    reinterpret_cast<const void*>(offsetof(WorldLabelVertex, position)));
                glEnableVertexAttribArray_(1);
                glVertexAttribPointer_(1, 2, GL_FLOAT, GL_FALSE, stride,
                    reinterpret_cast<const void*>(offsetof(WorldLabelVertex, uv)));
                glEnableVertexAttribArray_(2);
                glVertexAttribPointer_(2, 4, GL_FLOAT, GL_FALSE, stride,
                    reinterpret_cast<const void*>(offsetof(WorldLabelVertex, color)));
                glEnableVertexAttribArray_(3);
                glVertexAttribPointer_(3, 1, GL_FLOAT, GL_FALSE, stride,
                    reinterpret_cast<const void*>(offsetof(WorldLabelVertex, textured)));
                glBindVertexArray_(0);
                impl_->worldLabelsReady = true;
            }
            else
            {
                glDeleteProgram_(impl_->worldLabelProgram);
                impl_->worldLabelProgram = 0;
                log_line("GL: native world-label font unavailable");
            }
        }
        else
        {
            log_line("GL: native world-label shaders unavailable");
        }
        if (!create_sky_pipeline())
            log_line("GL: sky pipeline unavailable (non-fatal)");
        impl_->shadowTerrainProgram = build_program("shaders/gl/shadow_terrain.vert", "shaders/gl/shadow_depth.frag");
        impl_->shadowStaticProgram = build_program("shaders/gl/shadow_static.vert", "shaders/gl/shadow_depth.frag");
        impl_->shadowSkinnedProgram = build_program("shaders/gl/shadow_skinned.vert", "shaders/gl/shadow_depth.frag");
        create_descriptor_resources();

        if (impl_->shadowTerrainProgram && impl_->shadowStaticProgram && impl_->shadowSkinnedProgram)
        {
            glGenTextures_(1, &impl_->shadowDepthTexture);
            glBindTexture_(GL_TEXTURE_2D, impl_->shadowDepthTexture);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D_(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
                static_cast<GLsizei>(Impl::shadowResolution), static_cast<GLsizei>(Impl::shadowResolution),
                0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            glBindTexture_(GL_TEXTURE_2D, 0);

            glGenFramebuffers_(1, &impl_->shadowFramebuffer);
            glBindFramebuffer_(GL_FRAMEBUFFER, impl_->shadowFramebuffer);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                impl_->shadowDepthTexture, 0);
            glDrawBuffer_(GL_NONE);
            glReadBuffer_(GL_NONE);
            impl_->shadowReady = glCheckFramebufferStatus_(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            glBindFramebuffer_(GL_FRAMEBUFFER, 0);

            glCreateSamplers_(1, &impl_->shadowSampler);
            glSamplerParameteri_(impl_->shadowSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glSamplerParameteri_(impl_->shadowSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glSamplerParameteri_(impl_->shadowSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri_(impl_->shadowSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if (!impl_->shadowReady)
                log_line("GL: directional shadow framebuffer unavailable");
        }
        else
        {
            log_line("GL: directional shadow shaders unavailable");
        }

        ready_ = true;
        log_line("GL: initialized");
        return true;
    }

    void OpenGLRenderer::set_world_labels(std::vector<ScreenLabel> labels)
    {
        if (!impl_)
            return;
        impl_->worldLabels = std::move(labels);
    }

    void OpenGLRenderer::set_screen_ui(std::vector<ScreenUiCommand> commands)
    {
        if (!impl_)
            return;
        impl_->screenUi = std::move(commands);
    }

    bool OpenGLRenderer::native_ui_available() const
    {
        return impl_ && impl_->worldLabelsReady && impl_->worldLabelProgram;
    }

    std::uint32_t OpenGLRenderer::surface_width() const
    {
        return impl_ ? impl_->surfaceWidth : 0;
    }

    std::uint32_t OpenGLRenderer::surface_height() const
    {
        return impl_ ? impl_->surfaceHeight : 0;
    }

    std::uint32_t OpenGLRenderer::max_texture_array_layers() const
    {
        return impl_ ? impl_->maxImageArrayLayers : 0;
    }

    std::uint64_t OpenGLRenderer::vram_total_bytes() const
    {
        return 0; // not queryable in a vendor-neutral way via core GL
    }

    std::uint64_t OpenGLRenderer::vram_used_bytes() const
    {
        return 0;
    }

    OpenGLRenderer::GpuMetrics OpenGLRenderer::gpu_metrics() const
    {
        GpuMetrics m{};
        if (impl_ && impl_->gpuQueriesReady)
        {
            m.frameTimeMs = impl_->gpuFrameTimeMs;
            m.fragmentInvocations = impl_->fragmentInvocations;
            m.vertexInvocations = impl_->vertexInvocations;
            m.available = true;
        }
        return m;
    }

    bool OpenGLRenderer::resize(std::uint32_t width, std::uint32_t height)
    {
        if (!ready_ || !impl_)
            return false;
        if (width == 0 || height == 0)
            return false;
        if (impl_->surfaceWidth == width && impl_->surfaceHeight == height)
            return true;
        impl_->surfaceWidth = width;
        impl_->surfaceHeight = height;
        glViewport_(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        return true;
    }

    bool OpenGLRenderer::recreate_swapchain()
    {
        if (!ready_ || !impl_ || !impl_->window)
            return false;
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(impl_->window, &w, &h);
        if (w <= 0 || h <= 0)
            return false;
        return resize(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    }

    void OpenGLRenderer::wait_for_frame()
    {
        // No explicit fence-wait step needed for this single-context path;
        // vsync (context creation time) provides frame pacing.
    }

    bool OpenGLRenderer::create_descriptor_resources()
    {
        glCreateSamplers_(1, &impl_->terrainSampler);
        glSamplerParameteri_(impl_->terrainSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glSamplerParameteri_(impl_->terrainSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri_(impl_->terrainSampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glSamplerParameteri_(impl_->terrainSampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glSamplerParameteri_(impl_->terrainSampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        GLfloat maxAniso = 1.0f;
        glGetFloatv_(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
        glSamplerParameterf_(impl_->terrainSampler, GL_TEXTURE_MAX_ANISOTROPY, std::min(16.0f, maxAniso > 0.0f ? maxAniso : 1.0f));

        // Godot's standard world-asset materials use ordinary trilinear
        // mipmapping.  Keep terrain anisotropy independent: forcing its 16x
        // sampler onto nearby props preserves excessive high-frequency detail
        // at grazing angles and makes their textures shimmer while moving.
        glCreateSamplers_(1, &impl_->assetSampler);
        glSamplerParameteri_(impl_->assetSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glSamplerParameteri_(impl_->assetSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri_(impl_->assetSampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glSamplerParameteri_(impl_->assetSampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glSamplerParameteri_(impl_->assetSampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glSamplerParameterf_(impl_->assetSampler, GL_TEXTURE_MAX_ANISOTROPY, 1.0f);

        // One tiny procedural texture supplies both the seamless cloud noise
        // (alpha) and the generated water normal map (rgb). It mirrors the
        // Godot implementation without adding an external runtime asset.
        constexpr std::uint32_t environmentNoiseSide = 256u;
        const auto environmentNoise = make_environment_normal_noise(environmentNoiseSide);
        glGenTextures_(1, &impl_->environmentNoiseTexture);
        glBindTexture_(GL_TEXTURE_2D, impl_->environmentNoiseTexture);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8,
            static_cast<GLsizei>(environmentNoiseSide), static_cast<GLsizei>(environmentNoiseSide),
            0, GL_RGBA, GL_UNSIGNED_BYTE, environmentNoise.data());
        glGenerateMipmap_(GL_TEXTURE_2D);
        glBindTexture_(GL_TEXTURE_2D, 0);

        // No mipmaps: the debug effect array is always uploaded with a single
        // level (see upload_debug_effect_textures), so MIN_FILTER must not
        // request one — LINEAR_MIPMAP_LINEAR against a 1-level texture makes
        // it "mipmap incomplete" and GL samples opaque black instead.
        glCreateSamplers_(1, &impl_->debugEffectSampler);
        glSamplerParameteri_(impl_->debugEffectSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri_(impl_->debugEffectSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri_(impl_->debugEffectSampler, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
        glSamplerParameteri_(impl_->debugEffectSampler, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);

        glCreateSamplers_(1, &impl_->lightmapSampler);
        glSamplerParameteri_(impl_->lightmapSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri_(impl_->lightmapSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri_(impl_->lightmapSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri_(impl_->lightmapSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri_(impl_->lightmapSampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        // Dummy 1-layer terrain map SSBO so binding 1 is always valid pre-upload.
        const std::uint32_t dummy[2]{ 0xFFFFFFFFu, 0 };
        glCreateBuffers_(1, &impl_->terrainMapBuffer.id);
        glNamedBufferData_(impl_->terrainMapBuffer.id, sizeof(dummy), dummy, GL_STATIC_DRAW);
        impl_->terrainMapBuffer.byteSize = sizeof(dummy);
        glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, 1, impl_->terrainMapBuffer.id);

        // Dummy 1x1 white lightmap array so binding 2 is always valid.
        glCreateTextures_(GL_TEXTURE_2D_ARRAY, 1, &impl_->lightmapTexture.id);
        glTextureStorage3D_(impl_->lightmapTexture.id, 1, GL_RGBA8, 1, 1, 1);
        const std::uint8_t white[4]{ 255, 255, 255, 255 };
        glTextureSubImage3D_(impl_->lightmapTexture.id, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white);

        return true;
    }

    bool OpenGLRenderer::create_terrain_pipeline()
    {
        impl_->terrainProgram = build_program("shaders/gl/terrain.vert", "shaders/gl/terrain.frag");
        if (!impl_->terrainProgram)
        {
            log_line("GL: could not build terrain program");
            return false;
        }

        glCreateVertexArrays_(1, &impl_->terrainVao);
        return true;
    }

    bool OpenGLRenderer::create_static_object_pipeline()
    {
        impl_->staticObjectProgram = build_program("shaders/gl/static_object.vert", "shaders/gl/static_object.frag");
        if (!impl_->staticObjectProgram)
        {
            log_line("GL: could not build static object program");
            return false;
        }
        glCreateVertexArrays_(1, &impl_->objectVao);
        glCreateVertexArrays_(1, &impl_->animatedObjectVao);
        glCreateVertexArrays_(1, &impl_->characterVao);
        glCreateVertexArrays_(1, &impl_->botCharacterVao);
        glCreateVertexArrays_(1, &impl_->waterVao);
        return true;
    }

    bool OpenGLRenderer::create_skinned_character_pipeline()
    {
        impl_->skinnedCharacterProgram = build_program("shaders/gl/skinned_character.vert", "shaders/gl/skinned_character.frag");
        if (!impl_->skinnedCharacterProgram)
        {
            log_line("GL: skinned character shaders not found");
            return true; // non-fatal, matches original
        }
        glCreateVertexArrays_(1, &impl_->monsterCharacterVao);
        glCreateVertexArrays_(1, &impl_->npcCharacterVao);
        return true;
    }

    bool OpenGLRenderer::create_effect_particle_pipeline()
    {
        impl_->effectParticleProgram = build_program("shaders/gl/effect_particle.vert", "shaders/gl/effect_particle.frag");
        if (!impl_->effectParticleProgram)
        {
            log_line("GL: effect particle shaders not found");
            return true; // non-fatal, matches the skinned-character pipeline's fallback
        }
        glCreateVertexArrays_(1, &impl_->effectParticleVao);
        return true;
    }

    bool OpenGLRenderer::create_sky_pipeline()
    {
        impl_->skyProgram = build_program("shaders/gl/sky.vert", "shaders/gl/sky.frag");
        impl_->skyReady = impl_->skyProgram != 0;
        if (!impl_->skyReady)
            log_line("GL: sky shaders not found, sky will be clear color only");
        return true; // non-fatal either way, matches original
    }

    namespace
    {
        void destroy_gl_buffer(GlBuffer& buf)
        {
            if (buf.id)
                glDeleteBuffers_(1, &buf.id);
            buf = {};
        }

        GlBuffer make_static_buffer(const void* data, std::size_t byteSize, GLenum usage = GL_STATIC_DRAW)
        {
            GlBuffer buf{};
            if (byteSize == 0)
                return buf;
            glCreateBuffers_(1, &buf.id);
            glNamedBufferData_(buf.id, static_cast<GLsizeiptr>(byteSize), data, usage);
            buf.byteSize = byteSize;
            return buf;
        }

        struct DrawElementsIndirectCommand
        {
            std::uint32_t count{};
            std::uint32_t instanceCount{};
            std::uint32_t firstIndex{};
            std::int32_t baseVertex{};
            std::uint32_t baseInstance{};
        };
        static_assert(sizeof(DrawElementsIndirectCommand) == 20);

        void update_object_indirect_buffer(GlBuffer& buffer, std::uint32_t& drawCount,
            const std::vector<ObjectBatch>& batches)
        {
            std::vector<DrawElementsIndirectCommand> commands;
            commands.reserve(batches.size());
            for (const auto& batch : batches)
            {
                if (batch.instanceCount == 0 || batch.indexCount == 0)
                    continue;
                commands.push_back({ batch.indexCount, batch.instanceCount,
                    batch.firstIndex, 0, batch.firstInstance });
            }

            drawCount = static_cast<std::uint32_t>(commands.size());
            if (commands.empty())
                return;

            const auto requiredBytes = commands.size() * sizeof(DrawElementsIndirectCommand);
            if (!buffer.id)
                glCreateBuffers_(1, &buffer.id);
            if (requiredBytes > buffer.byteSize)
            {
                glNamedBufferData_(buffer.id, static_cast<GLsizeiptr>(requiredBytes),
                    commands.data(), GL_DYNAMIC_DRAW);
                buffer.byteSize = requiredBytes;
            }
            else
            {
                glNamedBufferSubData_(buffer.id, 0, static_cast<GLsizeiptr>(requiredBytes),
                    commands.data());
            }
        }

        void setup_terrain_vao_attribs(GLuint vao, GLuint vbo)
        {
            glBindVertexArray_(vao);
            glBindBuffer_(GL_ARRAY_BUFFER, vbo);
            const GLsizei stride = sizeof(TerrainVertex);
            glEnableVertexAttribArray_(0);
            glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(TerrainVertex, position));
            glEnableVertexAttribArray_(1);
            glVertexAttribPointer_(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(TerrainVertex, color));
            glEnableVertexAttribArray_(2);
            glVertexAttribPointer_(2, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(TerrainVertex, normal));
            glEnableVertexAttribArray_(3);
            glVertexAttribPointer_(3, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(TerrainVertex, uv));
            glEnableVertexAttribArray_(4);
            glVertexAttribIPointer_(4, 1, GL_UNSIGNED_INT, stride, (const void*)offsetof(TerrainVertex, textureLayer));
            glBindVertexArray_(0);
        }

        // TerrainVertex (locations 0-4) + ObjectInstance per-instance (locations 5-8).
        void setup_object_vao_attribs(GLuint vao, GLuint vbo, GLuint indexBuf, GLuint instanceBuf)
        {
            glBindVertexArray_(vao);
            glBindBuffer_(GL_ARRAY_BUFFER, vbo);
            const GLsizei stride = sizeof(TerrainVertex);
            glEnableVertexAttribArray_(0);
            glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(TerrainVertex, position));
            glEnableVertexAttribArray_(1);
            glVertexAttribPointer_(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(TerrainVertex, color));
            glEnableVertexAttribArray_(2);
            glVertexAttribPointer_(2, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(TerrainVertex, normal));
            glEnableVertexAttribArray_(3);
            glVertexAttribPointer_(3, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(TerrainVertex, uv));
            glEnableVertexAttribArray_(4);
            glVertexAttribIPointer_(4, 1, GL_UNSIGNED_INT, stride, (const void*)offsetof(TerrainVertex, textureLayer));

            if (instanceBuf)
            {
                glBindBuffer_(GL_ARRAY_BUFFER, instanceBuf);
                const GLsizei istride = sizeof(ObjectInstance);
                glEnableVertexAttribArray_(5);
                glVertexAttribPointer_(5, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(ObjectInstance, right));
                glVertexAttribDivisor_(5, 1);
                glEnableVertexAttribArray_(6);
                glVertexAttribPointer_(6, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(ObjectInstance, up));
                glVertexAttribDivisor_(6, 1);
                glEnableVertexAttribArray_(7);
                glVertexAttribPointer_(7, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(ObjectInstance, forward));
                glVertexAttribDivisor_(7, 1);
                glEnableVertexAttribArray_(8);
                glVertexAttribPointer_(8, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(ObjectInstance, position));
                glVertexAttribDivisor_(8, 1);
            }
            if (indexBuf)
                glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, indexBuf);
            glBindVertexArray_(0);
        }

        // TerrainVertex (locations 0-4) + EffectParticleInstance (locations 5-8).
        void setup_effect_particle_vao_attribs(GLuint vao, GLuint vbo, GLuint indexBuf, GLuint instanceBuf)
        {
            glBindVertexArray_(vao);
            glBindBuffer_(GL_ARRAY_BUFFER, vbo);
            const GLsizei stride = sizeof(TerrainVertex);
            glEnableVertexAttribArray_(0);
            glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(TerrainVertex, position));
            glEnableVertexAttribArray_(1);
            glVertexAttribPointer_(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(TerrainVertex, color));
            glEnableVertexAttribArray_(2);
            glVertexAttribPointer_(2, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(TerrainVertex, normal));
            glEnableVertexAttribArray_(3);
            glVertexAttribPointer_(3, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(TerrainVertex, uv));
            glEnableVertexAttribArray_(4);
            glVertexAttribIPointer_(4, 1, GL_UNSIGNED_INT, stride, (const void*)offsetof(TerrainVertex, textureLayer));

            if (instanceBuf)
            {
                glBindBuffer_(GL_ARRAY_BUFFER, instanceBuf);
                const GLsizei istride = sizeof(EffectParticleInstance);
                glEnableVertexAttribArray_(5);
                glVertexAttribPointer_(5, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(EffectParticleInstance, right));
                glVertexAttribDivisor_(5, 1);
                glEnableVertexAttribArray_(6);
                glVertexAttribPointer_(6, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(EffectParticleInstance, up));
                glVertexAttribDivisor_(6, 1);
                glEnableVertexAttribArray_(7);
                glVertexAttribPointer_(7, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(EffectParticleInstance, forward));
                glVertexAttribDivisor_(7, 1);
                glEnableVertexAttribArray_(8);
                glVertexAttribPointer_(8, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(EffectParticleInstance, position));
                glVertexAttribDivisor_(8, 1);
                glEnableVertexAttribArray_(9);
                glVertexAttribPointer_(9, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(EffectParticleInstance, color));
                glVertexAttribDivisor_(9, 1);
            }
            if (indexBuf)
                glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, indexBuf);
            glBindVertexArray_(0);
        }

        // Maps a D3DBLEND-style enum value straight from the .EFT file to a GL
        // blend factor. Mirrors effect-renderer's threeBlendFactor(), which was
        // validated against the retail client's ps0198 blend table (game.exe
        // 0x7138BC): that table starts one entry after D3DBLEND_ZERO, so the
        // stored value is already `blend + 1` relative to the D3D enum.
        GLenum d3d_blend_to_gl(std::int32_t blend, bool sourceRole)
        {
            switch (blend)
            {
                case 0: return GL_ZERO;
                case 1: return GL_ONE;
                case 2: return GL_SRC_COLOR;
                case 3: return GL_ONE_MINUS_SRC_COLOR;
                case 4: return GL_SRC_ALPHA;
                case 5: return GL_ONE_MINUS_SRC_ALPHA;
                case 6: return GL_DST_ALPHA;
                case 7: return GL_ONE_MINUS_DST_ALPHA;
                case 8: return GL_DST_COLOR;
                case 9: return GL_ONE_MINUS_DST_COLOR;
                case 10: return sourceRole ? GL_SRC_ALPHA_SATURATE : GL_ONE;
                case 11: return sourceRole ? GL_SRC_ALPHA : GL_ONE;
                default: return GL_ONE;
            }
        }

        // SkinnedVertex (locations 0-6) + ObjectInstance (locations 7-10).
        void setup_skinned_vao_attribs(GLuint vao, GLuint vbo, GLuint indexBuf, GLuint instanceBuf)
        {
            glBindVertexArray_(vao);
            glBindBuffer_(GL_ARRAY_BUFFER, vbo);
            const GLsizei stride = sizeof(SkinnedVertex);
            glEnableVertexAttribArray_(0);
            glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(SkinnedVertex, position));
            glEnableVertexAttribArray_(1);
            glVertexAttribPointer_(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(SkinnedVertex, color));
            glEnableVertexAttribArray_(2);
            glVertexAttribPointer_(2, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(SkinnedVertex, normal));
            glEnableVertexAttribArray_(3);
            glVertexAttribPointer_(3, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(SkinnedVertex, uv));
            glEnableVertexAttribArray_(4);
            glVertexAttribIPointer_(4, 1, GL_UNSIGNED_INT, stride, (const void*)offsetof(SkinnedVertex, textureLayer));
            glEnableVertexAttribArray_(5);
            glVertexAttribIPointer_(5, 4, GL_UNSIGNED_INT, stride, (const void*)offsetof(SkinnedVertex, bones));
            glEnableVertexAttribArray_(6);
            glVertexAttribPointer_(6, 4, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(SkinnedVertex, weights));

            if (instanceBuf)
            {
                glBindBuffer_(GL_ARRAY_BUFFER, instanceBuf);
                const GLsizei istride = sizeof(ObjectInstance);
                glEnableVertexAttribArray_(7);
                glVertexAttribPointer_(7, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(ObjectInstance, right));
                glVertexAttribDivisor_(7, 1);
                glEnableVertexAttribArray_(8);
                glVertexAttribPointer_(8, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(ObjectInstance, up));
                glVertexAttribDivisor_(8, 1);
                glEnableVertexAttribArray_(9);
                glVertexAttribPointer_(9, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(ObjectInstance, forward));
                glVertexAttribDivisor_(9, 1);
                glEnableVertexAttribArray_(10);
                glVertexAttribPointer_(10, 4, GL_FLOAT, GL_FALSE, istride, (const void*)offsetof(ObjectInstance, position));
                glVertexAttribDivisor_(10, 1);
            }
            if (indexBuf)
                glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, indexBuf);
            glBindVertexArray_(0);
        }
    }

    bool OpenGLRenderer::set_terrain_mesh(
        const std::vector<TerrainVertex>& vertices,
        const std::vector<std::uint32_t>& indices)
    {
        if (!ready_) return false;
        destroy_gl_buffer(impl_->terrainVertexBuffer);
        destroy_gl_buffer(impl_->terrainIndexBuffer);
        impl_->terrainReady = false;
        impl_->terrainIndexCount = 0;
        impl_->terrainDrawRanges.clear();
        if (vertices.empty() || indices.empty())
            return false;

        impl_->terrainVertexBuffer = make_static_buffer(vertices.data(), vertices.size() * sizeof(TerrainVertex));
        impl_->terrainIndexBuffer = make_static_buffer(indices.data(), indices.size() * sizeof(std::uint32_t));
        setup_terrain_vao_attribs(impl_->terrainVao, impl_->terrainVertexBuffer.id);
        glBindVertexArray_(impl_->terrainVao);
        glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, impl_->terrainIndexBuffer.id);
        glBindVertexArray_(0);

        impl_->terrainIndexCount = static_cast<std::uint32_t>(indices.size());
        impl_->terrainVertexBytes = vertices.size() * sizeof(TerrainVertex);
        impl_->terrainReady = true;
        return true;
    }

    bool OpenGLRenderer::set_water_mesh(
        const std::vector<TerrainVertex>& vertices,
        const std::vector<std::uint32_t>& indices)
    {
        if (!ready_) return false;
        destroy_gl_buffer(impl_->waterVertexBuffer);
        destroy_gl_buffer(impl_->waterIndexBuffer);
        impl_->waterIndexCount = 0;
        impl_->waterReady = false;
        if (vertices.empty() || indices.empty())
            return false;

        impl_->waterVertexBuffer = make_static_buffer(vertices.data(), vertices.size() * sizeof(TerrainVertex));
        impl_->waterIndexBuffer = make_static_buffer(indices.data(), indices.size() * sizeof(std::uint32_t));
        setup_terrain_vao_attribs(impl_->waterVao, impl_->waterVertexBuffer.id);
        glBindVertexArray_(impl_->waterVao);
        glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, impl_->waterIndexBuffer.id);
        glBindVertexArray_(0);

        impl_->waterIndexCount = static_cast<std::uint32_t>(indices.size());
        impl_->waterReady = true;
        return true;
    }

    bool OpenGLRenderer::update_terrain_vertices(const std::vector<TerrainVertex>& vertices)
    {
        if (!ready_ || !impl_->terrainReady || vertices.empty())
            return false;
        const auto vertexBytes = vertices.size() * sizeof(TerrainVertex);
        if (vertexBytes != impl_->terrainVertexBytes)
            return set_terrain_mesh(vertices, {});
        glNamedBufferSubData_(impl_->terrainVertexBuffer.id, 0, static_cast<GLsizeiptr>(vertexBytes), vertices.data());
        return true;
    }

    bool OpenGLRenderer::update_terrain_indices(const std::vector<std::uint32_t>& indices)
    {
        if (!ready_ || indices.empty())
            return false;
        destroy_gl_buffer(impl_->terrainIndexBuffer);
        impl_->terrainIndexBuffer = make_static_buffer(indices.data(), indices.size() * sizeof(std::uint32_t));
        glBindVertexArray_(impl_->terrainVao);
        glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, impl_->terrainIndexBuffer.id);
        glBindVertexArray_(0);
        impl_->terrainIndexCount = static_cast<std::uint32_t>(indices.size());
        impl_->terrainReady = true;
        return true;
    }

    void OpenGLRenderer::set_terrain_draw_ranges(const std::vector<TerrainDrawRange>& ranges)
    {
        if (!impl_) return;
        impl_->terrainDrawRanges = ranges;
    }

    bool OpenGLRenderer::set_static_object_mesh(
        const std::vector<TerrainVertex>& vertices,
        const std::vector<std::uint32_t>& indices,
        const std::vector<ObjectInstance>& instances,
        const std::vector<ObjectBatch>& batches)
    {
        if (!ready_) return false;
        destroy_gl_buffer(impl_->objectVertexBuffer);
        destroy_gl_buffer(impl_->objectIndexBuffer);
        destroy_gl_buffer(impl_->objectInstanceBuffer);
        impl_->objectBatches.clear();
        impl_->objectsReady = false;
        if (vertices.empty() || indices.empty() || instances.empty() || batches.empty())
            return false;

        impl_->objectVertexBuffer = make_static_buffer(vertices.data(), vertices.size() * sizeof(TerrainVertex));
        impl_->objectIndexBuffer = make_static_buffer(indices.data(), indices.size() * sizeof(std::uint32_t));
        impl_->objectInstanceBuffer = make_static_buffer(instances.data(), instances.size() * sizeof(ObjectInstance));
        setup_object_vao_attribs(impl_->objectVao, impl_->objectVertexBuffer.id, impl_->objectIndexBuffer.id, impl_->objectInstanceBuffer.id);

        impl_->objectBatches = batches;
        update_object_indirect_buffer(impl_->objectIndirectBuffer,
            impl_->objectIndirectCount, impl_->objectBatches);
        impl_->objectsReady = true;
        return true;
    }

    void OpenGLRenderer::set_static_object_batches(const std::vector<ObjectBatch>& batches)
    {
        if (!impl_) return;
        impl_->objectBatches = batches;
        update_object_indirect_buffer(impl_->objectIndirectBuffer,
            impl_->objectIndirectCount, impl_->objectBatches);
        impl_->objectsReady = !impl_->objectBatches.empty()
            && impl_->objectVertexBuffer.id && impl_->objectIndexBuffer.id && impl_->objectInstanceBuffer.id;
    }

    void OpenGLRenderer::set_animated_object_batches(const std::vector<ObjectBatch>& batches)
    {
        if (!impl_) return;
        impl_->animatedObjectBatches = batches;
    }

    bool OpenGLRenderer::update_static_object_instances(
        const std::vector<ObjectInstance>& instances,
        const std::vector<ObjectBatch>& batches)
    {
        if (!ready_) return false;
        destroy_gl_buffer(impl_->objectInstanceBuffer);
        impl_->objectBatches.clear();
        impl_->objectsReady = false;
        if (instances.empty() || batches.empty() || !impl_->objectVertexBuffer.id || !impl_->objectIndexBuffer.id)
            return false;

        impl_->objectInstanceBuffer = make_static_buffer(instances.data(), instances.size() * sizeof(ObjectInstance));
        setup_object_vao_attribs(impl_->objectVao, impl_->objectVertexBuffer.id, impl_->objectIndexBuffer.id, impl_->objectInstanceBuffer.id);
        impl_->objectBatches = batches;
        update_object_indirect_buffer(impl_->objectIndirectBuffer,
            impl_->objectIndirectCount, impl_->objectBatches);
        impl_->objectsReady = true;
        return true;
    }

    bool OpenGLRenderer::set_animated_object_mesh(
        const std::vector<TerrainVertex>& vertices,
        const std::vector<std::uint32_t>& indices,
        const std::vector<ObjectInstance>& instances,
        const std::vector<ObjectBatch>& batches)
    {
        if (!ready_) return false;
        destroy_gl_buffer(impl_->animatedObjectVertexBuffer);
        destroy_gl_buffer(impl_->animatedObjectIndexBuffer);
        destroy_gl_buffer(impl_->animatedObjectInstanceBuffer);
        impl_->animatedObjectBatches.clear();
        impl_->animatedObjectsReady = false;
        if (vertices.empty() || indices.empty() || instances.empty() || batches.empty())
            return false;

        impl_->animatedObjectVertexBuffer = make_static_buffer(vertices.data(), vertices.size() * sizeof(TerrainVertex), GL_DYNAMIC_DRAW);
        impl_->animatedObjectIndexBuffer = make_static_buffer(indices.data(), indices.size() * sizeof(std::uint32_t));
        impl_->animatedObjectInstanceBuffer = make_static_buffer(instances.data(), instances.size() * sizeof(ObjectInstance), GL_DYNAMIC_DRAW);
        setup_object_vao_attribs(impl_->animatedObjectVao, impl_->animatedObjectVertexBuffer.id,
            impl_->animatedObjectIndexBuffer.id, impl_->animatedObjectInstanceBuffer.id);

        impl_->animatedObjectVertexBytes = vertices.size() * sizeof(TerrainVertex);
        impl_->animatedObjectInstanceBytes = instances.size() * sizeof(ObjectInstance);
        impl_->animatedObjectBatches = batches;
        impl_->animatedObjectsReady = true;
        return true;
    }

    bool OpenGLRenderer::update_animated_object_vertices_range(
        const TerrainVertex* vertices, std::uint32_t firstVertex, std::uint32_t vertexCount)
    {
        if (!ready_ || !impl_->animatedObjectsReady || !vertices || vertexCount == 0)
            return false;
        const auto byteOffset = static_cast<GLintptr>(firstVertex) * sizeof(TerrainVertex);
        const auto byteSize = static_cast<GLsizeiptr>(vertexCount) * sizeof(TerrainVertex);
        if (static_cast<std::size_t>(byteOffset + byteSize) > impl_->animatedObjectVertexBytes)
            return false;
        glNamedBufferSubData_(impl_->animatedObjectVertexBuffer.id, byteOffset, byteSize, vertices + firstVertex);
        return true;
    }

    bool OpenGLRenderer::update_effect_particles(
        const std::vector<TerrainVertex>& vertices,
        const std::vector<std::uint32_t>& indices,
        const std::vector<EffectParticleInstance>& instances,
        const std::vector<EffectParticleBatch>& batches)
    {
        if (!ready_) return false;
        destroy_gl_buffer(impl_->effectParticleVertexBuffer);
        destroy_gl_buffer(impl_->effectParticleIndexBuffer);
        destroy_gl_buffer(impl_->effectParticleInstanceBuffer);
        impl_->effectParticleBatches.clear();
        impl_->effectParticlesReady = false;
        if (vertices.empty() || indices.empty() || instances.empty() || batches.empty()
            || !impl_->effectParticleProgram)
            return false;

        impl_->effectParticleVertexBuffer = make_static_buffer(vertices.data(), vertices.size() * sizeof(TerrainVertex), GL_DYNAMIC_DRAW);
        impl_->effectParticleIndexBuffer = make_static_buffer(indices.data(), indices.size() * sizeof(std::uint32_t), GL_DYNAMIC_DRAW);
        impl_->effectParticleInstanceBuffer = make_static_buffer(instances.data(), instances.size() * sizeof(EffectParticleInstance), GL_DYNAMIC_DRAW);
        setup_effect_particle_vao_attribs(impl_->effectParticleVao, impl_->effectParticleVertexBuffer.id,
            impl_->effectParticleIndexBuffer.id, impl_->effectParticleInstanceBuffer.id);

        impl_->effectParticleBatches = batches;
        impl_->effectParticlesReady = true;
        return true;
    }

    bool OpenGLRenderer::set_character_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices)
    {
        if (!ready_) return false;
        destroy_gl_buffer(impl_->characterVertexBuffer);
        destroy_gl_buffer(impl_->characterIndexBuffer);
        impl_->characterIndexCount = 0;
        impl_->characterReady = false;
        if (vertices.empty() || indices.empty())
            return false;
        impl_->characterVertexBuffer = make_static_buffer(vertices.data(), vertices.size() * sizeof(TerrainVertex), GL_DYNAMIC_DRAW);
        impl_->characterIndexBuffer = make_static_buffer(indices.data(), indices.size() * sizeof(std::uint32_t), GL_DYNAMIC_DRAW);
        setup_terrain_vao_attribs(impl_->characterVao, impl_->characterVertexBuffer.id);
        glBindVertexArray_(impl_->characterVao);
        glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, impl_->characterIndexBuffer.id);
        glBindVertexArray_(0);
        impl_->characterIndexCount = static_cast<std::uint32_t>(indices.size());
        impl_->characterVertexBytes = vertices.size() * sizeof(TerrainVertex);
        impl_->characterReady = true;
        return true;
    }

    bool OpenGLRenderer::update_character_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices)
    {
        if (!ready_) return false;
        const auto vertexBytes = vertices.size() * sizeof(TerrainVertex);
        const auto indexBytes = indices.size() * sizeof(std::uint32_t);
        if (impl_->characterVertexBuffer.id && vertexBytes <= impl_->characterVertexBuffer.byteSize
            && impl_->characterIndexBuffer.id && indexBytes <= impl_->characterIndexBuffer.byteSize)
        {
            if (!vertices.empty())
                glNamedBufferSubData_(impl_->characterVertexBuffer.id, 0, static_cast<GLsizeiptr>(vertexBytes), vertices.data());
            if (!indices.empty())
                glNamedBufferSubData_(impl_->characterIndexBuffer.id, 0, static_cast<GLsizeiptr>(indexBytes), indices.data());
            impl_->characterIndexCount = static_cast<std::uint32_t>(indices.size());
            impl_->characterVertexBytes = vertexBytes;
            impl_->characterReady = !vertices.empty() && !indices.empty();
            return true;
        }
        return set_character_mesh(vertices, indices);
    }

    bool OpenGLRenderer::update_character_vertices(const std::vector<TerrainVertex>& vertices)
    {
        return update_character_vertices(vertices.data(), vertices.size());
    }

    bool OpenGLRenderer::update_character_vertices(const TerrainVertex* data, std::size_t count)
    {
        if (!ready_ || !impl_->characterReady || !data || count == 0)
            return false;
        const auto byteSize = count * sizeof(TerrainVertex);
        if (byteSize > impl_->characterVertexBuffer.byteSize)
            return false;
        glNamedBufferSubData_(impl_->characterVertexBuffer.id, 0, static_cast<GLsizeiptr>(byteSize), data);
        return true;
    }

    void OpenGLRenderer::set_character_visible(bool visible)
    {
        if (impl_) impl_->characterVisible = visible;
    }

    bool OpenGLRenderer::set_bot_character_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices)
    {
        if (!ready_) return false;
        destroy_gl_buffer(impl_->botCharacterVertexBuffer);
        destroy_gl_buffer(impl_->botCharacterIndexBuffer);
        impl_->botCharacterReady = false;
        if (vertices.empty() || indices.empty())
            return false;
        impl_->botCharacterVertexBuffer = make_static_buffer(vertices.data(), vertices.size() * sizeof(TerrainVertex), GL_DYNAMIC_DRAW);
        impl_->botCharacterIndexBuffer = make_static_buffer(indices.data(), indices.size() * sizeof(std::uint32_t), GL_DYNAMIC_DRAW);
        if (impl_->botCharacterInstanceBuffer.id)
            setup_object_vao_attribs(impl_->botCharacterVao, impl_->botCharacterVertexBuffer.id,
                impl_->botCharacterIndexBuffer.id, impl_->botCharacterInstanceBuffer.id);
        impl_->botCharacterReady = true;
        return true;
    }

    bool OpenGLRenderer::update_bot_character_vertices(const std::vector<TerrainVertex>& vertices)
    {
        if (!ready_ || !impl_->botCharacterReady || vertices.empty())
            return false;
        const auto byteSize = vertices.size() * sizeof(TerrainVertex);
        if (byteSize > impl_->botCharacterVertexBuffer.byteSize)
            return false;
        glNamedBufferSubData_(impl_->botCharacterVertexBuffer.id, 0, static_cast<GLsizeiptr>(byteSize), vertices.data());
        return true;
    }

    bool OpenGLRenderer::update_bot_character_vertices_range(
        const TerrainVertex* vertices, std::uint32_t firstVertex, std::uint32_t vertexCount)
    {
        if (!ready_ || !impl_->botCharacterReady || !vertices || vertexCount == 0)
            return false;
        const auto byteOffset = static_cast<GLintptr>(firstVertex) * sizeof(TerrainVertex);
        const auto byteSize = static_cast<GLsizeiptr>(vertexCount) * sizeof(TerrainVertex);
        if (static_cast<std::size_t>(byteOffset + byteSize) > impl_->botCharacterVertexBuffer.byteSize)
            return false;
        glNamedBufferSubData_(impl_->botCharacterVertexBuffer.id, byteOffset, byteSize, vertices + firstVertex);
        return true;
    }

    bool OpenGLRenderer::update_bot_character_instances(
        const std::vector<ObjectInstance>& instances,
        const std::vector<ObjectBatch>& batches)
    {
        if (!ready_) return false;
        destroy_gl_buffer(impl_->botCharacterInstanceBuffer);
        impl_->botCharacterBatches.clear();
        if (instances.empty() || batches.empty() || !impl_->botCharacterVertexBuffer.id)
            return false;
        impl_->botCharacterInstanceBuffer = make_static_buffer(instances.data(), instances.size() * sizeof(ObjectInstance), GL_DYNAMIC_DRAW);
        setup_object_vao_attribs(impl_->botCharacterVao, impl_->botCharacterVertexBuffer.id,
            impl_->botCharacterIndexBuffer.id, impl_->botCharacterInstanceBuffer.id);
        impl_->botCharacterBatches = batches;
        return true;
    }

    void OpenGLRenderer::set_bot_character_visible(bool visible)
    {
        if (impl_) impl_->botCharacterVisible = visible;
    }

    bool OpenGLRenderer::set_monster_character_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices)
    {
        if (!ready_) return false;
        destroy_gl_buffer(impl_->monsterCharacterVertexBuffer);
        destroy_gl_buffer(impl_->monsterCharacterIndexBuffer);
        impl_->monsterCharacterReady = false;
        impl_->monsterCharacterSkinned = false;
        if (vertices.empty() || indices.empty())
            return false;
        impl_->monsterCharacterVertexBuffer = make_static_buffer(vertices.data(), vertices.size() * sizeof(TerrainVertex), GL_DYNAMIC_DRAW);
        impl_->monsterCharacterIndexBuffer = make_static_buffer(indices.data(), indices.size() * sizeof(std::uint32_t), GL_DYNAMIC_DRAW);
        setup_object_vao_attribs(impl_->objectVao, impl_->monsterCharacterVertexBuffer.id,
            impl_->monsterCharacterIndexBuffer.id, 0);
        impl_->monsterCharacterReady = true;
        return true;
    }

    bool OpenGLRenderer::update_monster_character_vertices(const std::vector<TerrainVertex>& vertices)
    {
        if (!ready_ || !impl_->monsterCharacterReady || vertices.empty())
            return false;
        const auto byteSize = vertices.size() * sizeof(TerrainVertex);
        if (byteSize > impl_->monsterCharacterVertexBuffer.byteSize)
            return false;
        glNamedBufferSubData_(impl_->monsterCharacterVertexBuffer.id, 0, static_cast<GLsizeiptr>(byteSize), vertices.data());
        return true;
    }

    bool OpenGLRenderer::update_monster_character_vertices_range(
        const TerrainVertex* vertices, std::uint32_t firstVertex, std::uint32_t vertexCount)
    {
        if (!ready_ || !impl_->monsterCharacterReady || !vertices || vertexCount == 0)
            return false;
        const auto byteOffset = static_cast<GLintptr>(firstVertex) * sizeof(TerrainVertex);
        const auto byteSize = static_cast<GLsizeiptr>(vertexCount) * sizeof(TerrainVertex);
        if (static_cast<std::size_t>(byteOffset + byteSize) > impl_->monsterCharacterVertexBuffer.byteSize)
            return false;
        glNamedBufferSubData_(impl_->monsterCharacterVertexBuffer.id, byteOffset, byteSize, vertices + firstVertex);
        return true;
    }

    bool OpenGLRenderer::update_monster_character_instances(
        const std::vector<ObjectInstance>& instances,
        const std::vector<ObjectBatch>& batches)
    {
        if (!ready_) return false;
        const auto frame = frameIndex_ % kMaxFramesInFlight;
        destroy_gl_buffer(impl_->monsterCharacterInstanceBuffer[frame]);
        impl_->monsterCharacterBatches.clear();
        if (instances.empty() || batches.empty())
            return false;
        impl_->monsterCharacterInstanceBuffer[frame] = make_static_buffer(instances.data(), instances.size() * sizeof(ObjectInstance), GL_DYNAMIC_DRAW);
        if (impl_->monsterCharacterSkinned && impl_->monsterCharacterVertexBuffer.id)
            setup_skinned_vao_attribs(impl_->monsterCharacterVao, impl_->monsterCharacterVertexBuffer.id,
                impl_->monsterCharacterIndexBuffer.id, impl_->monsterCharacterInstanceBuffer[frame].id);
        else if (impl_->monsterCharacterVertexBuffer.id)
            setup_object_vao_attribs(impl_->objectVao, impl_->monsterCharacterVertexBuffer.id,
                impl_->monsterCharacterIndexBuffer.id, impl_->monsterCharacterInstanceBuffer[frame].id);
        impl_->monsterCharacterBatches = batches;
        return true;
    }

    void OpenGLRenderer::set_monster_character_visible(bool visible)
    {
        if (impl_) impl_->monsterCharacterVisible = visible;
    }

    bool OpenGLRenderer::set_monster_skinned_mesh(const std::vector<SkinnedVertex>& vertices, const std::vector<std::uint32_t>& indices)
    {
        if (!ready_) return false;
        destroy_gl_buffer(impl_->monsterCharacterVertexBuffer);
        destroy_gl_buffer(impl_->monsterCharacterIndexBuffer);
        impl_->monsterCharacterReady = false;
        impl_->monsterCharacterSkinned = false;
        if (vertices.empty() || indices.empty())
            return false;
        impl_->monsterCharacterVertexBuffer = make_static_buffer(vertices.data(), vertices.size() * sizeof(SkinnedVertex));
        impl_->monsterCharacterIndexBuffer = make_static_buffer(indices.data(), indices.size() * sizeof(std::uint32_t));
        impl_->monsterCharacterSkinned = true;
        impl_->monsterCharacterReady = true;
        return true;
    }

    void OpenGLRenderer::update_monster_bone_palette(const float* rows16PerBone, std::size_t floatCount)
    {
        if (!impl_ || !rows16PerBone || floatCount == 0) return;
        const auto frame = frameIndex_ % kMaxFramesInFlight;
        const auto byteSize = floatCount * sizeof(float);
        auto& buf = impl_->monsterPaletteBuffer[frame];
        if (!buf.id || byteSize > buf.byteSize)
        {
            destroy_gl_buffer(buf);
            buf = make_static_buffer(rows16PerBone, byteSize, GL_DYNAMIC_DRAW);
            glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, 3, buf.id);
        }
        else
        {
            glNamedBufferSubData_(buf.id, 0, static_cast<GLsizeiptr>(byteSize), rows16PerBone);
        }
    }

    bool OpenGLRenderer::set_npc_skinned_mesh(const std::vector<SkinnedVertex>& vertices, const std::vector<std::uint32_t>& indices)
    {
        if (!ready_) return false;
        destroy_gl_buffer(impl_->npcCharacterVertexBuffer);
        destroy_gl_buffer(impl_->npcCharacterIndexBuffer);
        impl_->npcCharacterReady = false;
        if (vertices.empty() || indices.empty())
            return false;
        impl_->npcCharacterVertexBuffer = make_static_buffer(vertices.data(), vertices.size() * sizeof(SkinnedVertex));
        impl_->npcCharacterIndexBuffer = make_static_buffer(indices.data(), indices.size() * sizeof(std::uint32_t));
        impl_->npcCharacterReady = true;
        return true;
    }

    void OpenGLRenderer::update_npc_bone_palette(const float* rows16PerBone, std::size_t floatCount)
    {
        if (!impl_ || !rows16PerBone || floatCount == 0) return;
        const auto frame = frameIndex_ % kMaxFramesInFlight;
        const auto byteSize = floatCount * sizeof(float);
        auto& buf = impl_->npcPaletteBuffer[frame];
        if (!buf.id || byteSize > buf.byteSize)
        {
            destroy_gl_buffer(buf);
            buf = make_static_buffer(rows16PerBone, byteSize, GL_DYNAMIC_DRAW);
        }
        else
        {
            glNamedBufferSubData_(buf.id, 0, static_cast<GLsizeiptr>(byteSize), rows16PerBone);
        }
    }

    bool OpenGLRenderer::update_npc_skinned_instances(
        const std::vector<ObjectInstance>& instances,
        const std::vector<ObjectBatch>& batches)
    {
        if (!ready_) return false;
        const auto frame = frameIndex_ % kMaxFramesInFlight;
        destroy_gl_buffer(impl_->npcCharacterInstanceBuffer[frame]);
        impl_->npcCharacterBatches.clear();
        if (instances.empty() || batches.empty() || !impl_->npcCharacterVertexBuffer.id)
            return false;
        impl_->npcCharacterInstanceBuffer[frame] = make_static_buffer(instances.data(), instances.size() * sizeof(ObjectInstance), GL_DYNAMIC_DRAW);
        setup_skinned_vao_attribs(impl_->npcCharacterVao, impl_->npcCharacterVertexBuffer.id,
            impl_->npcCharacterIndexBuffer.id, impl_->npcCharacterInstanceBuffer[frame].id);
        impl_->npcCharacterBatches = batches;
        return true;
    }

    void OpenGLRenderer::set_npc_skinned_visible(bool visible)
    {
        if (impl_) impl_->npcCharacterVisible = visible;
    }

    bool OpenGLRenderer::upload_terrain_textures(const std::vector<DdsTexture>& textures,
        const std::function<void()>& pump,
        std::uint32_t assetFirstLayer,
        std::uint32_t assetLayerCount)
    {
        if (!ready_) return false;

        if (impl_->terrainTextureArray.id)
            glDeleteTextures_(1, &impl_->terrainTextureArray.id);
        if (impl_->assetTextureArray.id)
            glDeleteTextures_(1, &impl_->assetTextureArray.id);
        impl_->terrainTextureArray = {};
        impl_->assetTextureArray = {};
        impl_->terrainTextureLayerCount = 0;
        impl_->terrainTextureWidth = 0;
        impl_->terrainTextureHeight = 0;
        impl_->terrainTextureMipLevels = 0;
        impl_->terrainTextureFormat = 0;
        impl_->terrainTextureCompressed = false;
        impl_->terrainTexturesReady = false;
        impl_->assetTexturesReady = false;

        if (textures.empty())
            return false;

        std::uint32_t texWidth = 0, texHeight = 0;
        for (const auto& tex : textures)
        {
            if (tex.valid) { texWidth = tex.width; texHeight = tex.height; break; }
        }
        if (texWidth == 0 || texHeight == 0)
            return false;

        auto layerCount = static_cast<std::uint32_t>(textures.size());
        if (layerCount > impl_->maxImageArrayLayers)
        {
            log_line("GL: texture array layer count exceeds device limit — truncating");
            layerCount = impl_->maxImageArrayLayers;
        }

        const auto maxDim = std::max(texWidth, texHeight);
        const auto fullMips = static_cast<std::uint32_t>(std::floor(std::log2(static_cast<float>(maxDim)))) + 1u;
        const auto mipLevels = std::min(fullMips, static_cast<std::uint32_t>(std::max(1.0, std::log2(static_cast<double>(maxDim)) - 1.0)));

        std::uint32_t nativeFormat = 0;
        std::uint32_t nativeMips = UINT32_MAX;
        bool canUploadBc = true;
        for (const auto& tex : textures)
        {
            if (!tex.valid) continue;
            if (!tex.compressed || !is_bc_format(tex.vkFormat) || tex.mipData.empty()
                || tex.width != texWidth || tex.height != texHeight)
            {
                canUploadBc = false;
                break;
            }
            if (nativeFormat == 0) nativeFormat = tex.vkFormat;
            else if (tex.vkFormat != nativeFormat) { canUploadBc = false; break; }
            nativeMips = std::min(nativeMips, static_cast<std::uint32_t>(tex.mipData.size()));
        }

        GLuint texId{};
        glCreateTextures_(GL_TEXTURE_2D_ARRAY, 1, &texId);

        if (canUploadBc && nativeFormat != 0 && nativeMips != UINT32_MAX && nativeMips > 0)
        {
            nativeMips = std::min(nativeMips, mipLevels);
            const GLenum internalFormat = bc_gl_internal_format(nativeFormat);
            glTextureStorage3D_(texId, static_cast<GLsizei>(nativeMips), internalFormat,
                static_cast<GLsizei>(texWidth), static_cast<GLsizei>(texHeight), static_cast<GLsizei>(layerCount));

            for (std::uint32_t layer = 0; layer < layerCount; ++layer)
            {
                if (pump && (layer & 63u) == 0u)
                    pump();
                for (std::uint32_t mip = 0; mip < nativeMips; ++mip)
                {
                    const auto mipW = std::max(1u, texWidth >> mip);
                    const auto mipH = std::max(1u, texHeight >> mip);
                    const std::vector<std::uint8_t>* src{};
                    std::vector<std::uint8_t> fallback;
                    if (textures[layer].valid && mip < textures[layer].mipData.size())
                        src = &textures[layer].mipData[mip];
                    else
                    {
                        fallback = make_bc_fallback_mip(nativeFormat, mipW, mipH);
                        src = &fallback;
                    }
                    glCompressedTextureSubImage3D_(texId, static_cast<GLint>(mip),
                        0, 0, static_cast<GLint>(layer), static_cast<GLsizei>(mipW), static_cast<GLsizei>(mipH), 1,
                        internalFormat, static_cast<GLsizei>(src->size()), src->data());
                }
            }
            impl_->terrainTextureCompressed = true;
            impl_->terrainTextureFormat = nativeFormat;
            impl_->terrainTextureMipLevels = nativeMips;
        }
        else
        {
            // RGBA fallback: decode each valid texture to RGBA8 (dds_loader's
            // decode_texture_rgba), pad missing ones with a solid fallback.
            glTextureStorage3D_(texId, static_cast<GLsizei>(mipLevels), GL_RGBA8,
                static_cast<GLsizei>(texWidth), static_cast<GLsizei>(texHeight), static_cast<GLsizei>(layerCount));
            std::vector<std::uint8_t> fallbackRgba(static_cast<std::size_t>(texWidth) * texHeight * 4, 0x40);
            for (std::uint32_t layer = 0; layer < layerCount; ++layer)
            {
                if (pump && (layer & 63u) == 0u)
                    pump();
                const std::uint8_t* pixels = fallbackRgba.data();
                std::vector<std::uint8_t> decoded;
                if (textures[layer].valid)
                {
                    decoded = decode_texture_rgba(textures[layer]);
                    if (!decoded.empty() && textures[layer].width == texWidth && textures[layer].height == texHeight)
                        pixels = decoded.data();
                }
                glTextureSubImage3D_(texId, 0, 0, 0, static_cast<GLint>(layer),
                    static_cast<GLsizei>(texWidth), static_cast<GLsizei>(texHeight), 1,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            }
            if (mipLevels > 1)
                glGenerateTextureMipmap_(texId);
            impl_->terrainTextureCompressed = false;
            impl_->terrainTextureFormat = kFormatR8G8B8A8Unorm;
            impl_->terrainTextureMipLevels = mipLevels;
        }

        glTextureParameteri_(texId, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri_(texId, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri_(texId, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri_(texId, GL_TEXTURE_WRAP_T, GL_REPEAT);

        impl_->terrainTextureArray.id = texId;
        impl_->terrainTextureLayerCount = layerCount;
        impl_->terrainTextureWidth = texWidth;
        impl_->terrainTextureHeight = texHeight;
        impl_->terrainTexturesReady = true;

        // The legacy DDS files contain pre-generated mip chains whose small
        // levels shimmer noticeably as the camera moves. Godot discards those
        // levels and builds a fresh chain from the source image. Reproduce
        // that behaviour only for world assets in a compact secondary array:
        // upload mip 0, let the driver generate all lower BC levels, then bind
        // this array solely while drawing static/animated world props. Terrain,
        // sky, water, effects and character textures retain their original
        // chains in terrainTextureArray.
        const auto assetEndLayer = std::min(layerCount, assetFirstLayer + assetLayerCount);
        if (canUploadBc && nativeFormat != 0 && nativeMips > 1
            && assetFirstLayer < assetEndLayer)
        {
            GLuint assetTexId{};
            glCreateTextures_(GL_TEXTURE_2D_ARRAY, 1, &assetTexId);
            const GLenum internalFormat = bc_gl_internal_format(nativeFormat);
            glTextureStorage3D_(assetTexId, static_cast<GLsizei>(nativeMips), internalFormat,
                static_cast<GLsizei>(texWidth), static_cast<GLsizei>(texHeight),
                static_cast<GLsizei>(assetEndLayer));

            for (std::uint32_t layer = assetFirstLayer; layer < assetEndLayer; ++layer)
            {
                if (pump && (layer & 63u) == 0u)
                    pump();
                const std::vector<std::uint8_t>* src{};
                std::vector<std::uint8_t> fallback;
                if (textures[layer].valid && !textures[layer].mipData.empty())
                    src = &textures[layer].mipData[0];
                else
                {
                    fallback = make_bc_fallback_mip(nativeFormat, texWidth, texHeight);
                    src = &fallback;
                }
                glCompressedTextureSubImage3D_(assetTexId, 0,
                    0, 0, static_cast<GLint>(layer),
                    static_cast<GLsizei>(texWidth), static_cast<GLsizei>(texHeight), 1,
                    internalFormat, static_cast<GLsizei>(src->size()), src->data());
            }

            // Ignore any stale error raised before this isolated operation so
            // unsupported compressed-mipmap generation has a reliable fallback.
            while (glGetError_() != GL_NO_ERROR) {}
            glGenerateTextureMipmap_(assetTexId);
            const auto mipError = glGetError_();
            if (mipError == GL_NO_ERROR)
            {
                glTextureParameteri_(assetTexId, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTextureParameteri_(assetTexId, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTextureParameteri_(assetTexId, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTextureParameteri_(assetTexId, GL_TEXTURE_WRAP_T, GL_REPEAT);
                impl_->assetTextureArray.id = assetTexId;
                impl_->assetTexturesReady = true;
            }
            else
            {
                glDeleteTextures_(1, &assetTexId);
                log_line("GL: compressed world-asset mip regeneration unsupported; using DDS mip chains");
            }
        }
        return true;
    }

    bool OpenGLRenderer::upload_debug_effect_textures(const std::vector<DdsTexture>& textures)
    {
        if (!ready_) return false;

        if (impl_->debugEffectTextureArray.id)
            glDeleteTextures_(1, &impl_->debugEffectTextureArray.id);
        impl_->debugEffectTextureArray = {};
        impl_->debugEffectTextureLayerCount = 0;
        impl_->debugEffectTextureWidth = 0;
        impl_->debugEffectTextureHeight = 0;
        impl_->debugEffectTextureMipLevels = 0;
        impl_->debugEffectTextureFormat = 0;
        impl_->debugEffectTextureCompressed = false;
        impl_->debugEffectTexturesReady = false;

        if (textures.empty())
            return false;

        std::uint32_t texWidth = 0, texHeight = 0;
        for (const auto& tex : textures)
        {
            if (tex.valid) { texWidth = tex.width; texHeight = tex.height; break; }
        }
        if (texWidth == 0 || texHeight == 0)
            return false;

        auto layerCount = static_cast<std::uint32_t>(textures.size());
        if (layerCount > impl_->maxImageArrayLayers)
        {
            log_line("GL: debug effect texture array layer count exceeds device limit — truncating");
            layerCount = impl_->maxImageArrayLayers;
        }

        // Always a single mip level: many effect textures are atlases
        // (packed animation-frame grids), and mipmapping — box-filtering 2x2
        // neighborhoods repeatedly — bleeds neighboring cells together,
        // visible as "squares" of wrong transparency at any mip beyond 0.
        const std::uint32_t mipLevels = 1;

        std::uint32_t nativeFormat = 0;
        std::uint32_t nativeMips = UINT32_MAX;
        bool canUploadBc = true;
        for (const auto& tex : textures)
        {
            if (!tex.valid) continue;
            if (!tex.compressed || !is_bc_format(tex.vkFormat) || tex.mipData.empty()
                || tex.width != texWidth || tex.height != texHeight)
            {
                canUploadBc = false;
                break;
            }
            if (nativeFormat == 0) nativeFormat = tex.vkFormat;
            else if (tex.vkFormat != nativeFormat) { canUploadBc = false; break; }
            nativeMips = std::min(nativeMips, static_cast<std::uint32_t>(tex.mipData.size()));
        }

        GLuint texId{};
        glCreateTextures_(GL_TEXTURE_2D_ARRAY, 1, &texId);

        if (canUploadBc && nativeFormat != 0 && nativeMips != UINT32_MAX && nativeMips > 0)
        {
            nativeMips = std::min(nativeMips, mipLevels);
            const GLenum internalFormat = bc_gl_internal_format(nativeFormat);
            glTextureStorage3D_(texId, static_cast<GLsizei>(nativeMips), internalFormat,
                static_cast<GLsizei>(texWidth), static_cast<GLsizei>(texHeight), static_cast<GLsizei>(layerCount));

            for (std::uint32_t layer = 0; layer < layerCount; ++layer)
            {
                for (std::uint32_t mip = 0; mip < nativeMips; ++mip)
                {
                    const auto mipW = std::max(1u, texWidth >> mip);
                    const auto mipH = std::max(1u, texHeight >> mip);
                    const std::vector<std::uint8_t>* src{};
                    std::vector<std::uint8_t> fallback;
                    if (textures[layer].valid && mip < textures[layer].mipData.size())
                        src = &textures[layer].mipData[mip];
                    else
                    {
                        fallback = make_bc_fallback_mip(nativeFormat, mipW, mipH);
                        src = &fallback;
                    }
                    glCompressedTextureSubImage3D_(texId, static_cast<GLint>(mip),
                        0, 0, static_cast<GLint>(layer), static_cast<GLsizei>(mipW), static_cast<GLsizei>(mipH), 1,
                        internalFormat, static_cast<GLsizei>(src->size()), src->data());
                }
            }
            impl_->debugEffectTextureCompressed = true;
            impl_->debugEffectTextureFormat = nativeFormat;
            impl_->debugEffectTextureMipLevels = nativeMips;
        }
        else
        {
            glTextureStorage3D_(texId, static_cast<GLsizei>(mipLevels), GL_RGBA8,
                static_cast<GLsizei>(texWidth), static_cast<GLsizei>(texHeight), static_cast<GLsizei>(layerCount));
            std::vector<std::uint8_t> fallbackRgba(static_cast<std::size_t>(texWidth) * texHeight * 4, 0x40);
            for (std::uint32_t layer = 0; layer < layerCount; ++layer)
            {
                const std::uint8_t* pixels = fallbackRgba.data();
                std::vector<std::uint8_t> decoded;
                if (textures[layer].valid)
                {
                    decoded = decode_texture_rgba(textures[layer]);
                    if (!decoded.empty() && textures[layer].width == texWidth && textures[layer].height == texHeight)
                        pixels = decoded.data();
                }
                glTextureSubImage3D_(texId, 0, 0, 0, static_cast<GLint>(layer),
                    static_cast<GLsizei>(texWidth), static_cast<GLsizei>(texHeight), 1,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            }
            impl_->debugEffectTextureCompressed = false;
            impl_->debugEffectTextureFormat = kFormatR8G8B8A8Unorm;
            impl_->debugEffectTextureMipLevels = mipLevels;
        }

        // Plain LINEAR (no mipmap chain — see the mipLevels note above).
        // MIRRORED_REPEAT: most debug-panel billboards sample a single
        // sprite at UV 0..1 and never touch the wrap mode at all, but
        // mirrorTexture-flagged components deliberately sample UV -1..2
        // (see effect_particle_system.cpp's mirrorUvs) to tile the sprite
        // 3x with mirrored seams instead of stretching one copy across a
        // large quad — matching the reference renderer's
        // THREE.MirroredRepeatWrapping for mirrorTexture effects.
        glTextureParameteri_(texId, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri_(texId, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri_(texId, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
        glTextureParameteri_(texId, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);

        impl_->debugEffectTextureArray.id = texId;
        impl_->debugEffectTextureLayerCount = layerCount;
        impl_->debugEffectTextureWidth = texWidth;
        impl_->debugEffectTextureHeight = texHeight;
        impl_->debugEffectTexturesReady = true;
        return true;
    }

    bool OpenGLRenderer::upload_terrain_texture_layers(std::uint32_t firstLayer, const std::vector<DdsTexture>& textures)
    {
        if (!ready_ || !impl_->terrainTexturesReady || textures.empty())
            return false;
        if (firstLayer + textures.size() > impl_->terrainTextureLayerCount)
            return false;

        for (std::size_t i = 0; i < textures.size(); ++i)
        {
            const auto& tex = textures[i];
            const auto layer = static_cast<GLint>(firstLayer + i);
            if (impl_->terrainTextureCompressed)
            {
                if (!tex.valid || tex.mipData.empty()) continue;
                const GLenum internalFormat = bc_gl_internal_format(impl_->terrainTextureFormat);
                for (std::uint32_t mip = 0; mip < impl_->terrainTextureMipLevels && mip < tex.mipData.size(); ++mip)
                {
                    const auto mipW = std::max(1u, impl_->terrainTextureWidth >> mip);
                    const auto mipH = std::max(1u, impl_->terrainTextureHeight >> mip);
                    const auto& src = tex.mipData[mip];
                    glCompressedTextureSubImage3D_(impl_->terrainTextureArray.id, static_cast<GLint>(mip),
                        0, 0, layer, static_cast<GLsizei>(mipW), static_cast<GLsizei>(mipH), 1,
                        internalFormat, static_cast<GLsizei>(src.size()), src.data());
                }
            }
            else
            {
                if (!tex.valid) continue;
                auto decoded = decode_texture_rgba(tex);
                if (decoded.empty() || tex.width != impl_->terrainTextureWidth || tex.height != impl_->terrainTextureHeight)
                    continue;
                glTextureSubImage3D_(impl_->terrainTextureArray.id, 0, 0, 0, layer,
                    static_cast<GLsizei>(impl_->terrainTextureWidth), static_cast<GLsizei>(impl_->terrainTextureHeight), 1,
                    GL_RGBA, GL_UNSIGNED_BYTE, decoded.data());
            }
        }
        return true;
    }

    bool OpenGLRenderer::upload_field_lightmaps(const std::vector<DdsTexture>& lightmaps, std::uint32_t sectionCount)
    {
        if (!ready_ || lightmaps.empty())
            return false;

        std::uint32_t lmWidth = 0, lmHeight = 0;
        std::uint32_t lmFormat = 0;
        for (const auto& lm : lightmaps)
        {
            if (lm.valid && !lm.mipData.empty()) { lmWidth = lm.width; lmHeight = lm.height; lmFormat = lm.vkFormat; break; }
        }
        if (lmWidth == 0 || lmHeight == 0)
            return false;

        if (impl_->lightmapTexture.id)
            glDeleteTextures_(1, &impl_->lightmapTexture.id);
        impl_->lightmapTexture = {};
        impl_->lightmapReady = false;

        const auto layerCount = static_cast<std::uint32_t>(lightmaps.size());
        GLuint texId{};
        glCreateTextures_(GL_TEXTURE_2D_ARRAY, 1, &texId);

        if (is_bc_format(lmFormat))
        {
            const GLenum internalFormat = bc_gl_internal_format(lmFormat);
            glTextureStorage3D_(texId, 1, internalFormat, static_cast<GLsizei>(lmWidth), static_cast<GLsizei>(lmHeight), static_cast<GLsizei>(layerCount));
            for (std::uint32_t i = 0; i < layerCount; ++i)
            {
                const auto& lm = lightmaps[i];
                if (!lm.valid || lm.mipData.empty()) continue;
                const auto& mipBytes = lm.mipData[0];
                glCompressedTextureSubImage3D_(texId, 0, 0, 0, static_cast<GLint>(i),
                    static_cast<GLsizei>(lmWidth), static_cast<GLsizei>(lmHeight), 1,
                    internalFormat, static_cast<GLsizei>(mipBytes.size()), mipBytes.data());
            }
        }
        else
        {
            glTextureStorage3D_(texId, 1, GL_RGBA8, static_cast<GLsizei>(lmWidth), static_cast<GLsizei>(lmHeight), static_cast<GLsizei>(layerCount));
            for (std::uint32_t i = 0; i < layerCount; ++i)
            {
                const auto& lm = lightmaps[i];
                if (!lm.valid) continue;
                auto decoded = decode_texture_rgba(lm);
                if (decoded.empty()) continue;
                glTextureSubImage3D_(texId, 0, 0, 0, static_cast<GLint>(i),
                    static_cast<GLsizei>(lmWidth), static_cast<GLsizei>(lmHeight), 1, GL_RGBA, GL_UNSIGNED_BYTE, decoded.data());
            }
        }
        glTextureParameteri_(texId, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri_(texId, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri_(texId, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri_(texId, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        impl_->lightmapTexture.id = texId;
        impl_->lightmapSectionCount = sectionCount;
        impl_->lightmapReady = true;
        impl_->skyConstants[6] = static_cast<float>(sectionCount);
        impl_->skyConstants[7] = 1.0f;
        return true;
    }

    void OpenGLRenderer::disable_field_lightmaps()
    {
        if (!impl_) return;
        impl_->skyConstants[6] = 0.0f;
        impl_->skyConstants[7] = 0.0f;
        impl_->lightmapSectionCount = 0;
        impl_->lightmapReady = false;
    }

    bool OpenGLRenderer::upload_terrain_texture_map(
        const std::vector<std::uint8_t>& data,
        std::uint32_t side,
        float mapSize,
        const float* tileSizes,
        std::uint32_t tileSizeCount,
        std::uint32_t alphaMaskLayerFlags,
        std::uint32_t splatLayerCount)
    {
        if (!impl_ || data.empty() || side == 0)
            return false;

        destroy_gl_buffer(impl_->terrainMapBuffer);
        impl_->terrainMapReady = false;

        constexpr std::uint32_t kMaxTileSizes = 16;
        const auto mapBytes = data.size();
        const auto mapBytesPadded = (mapBytes + 3u) & ~3u;
        const auto tileSizeBytes = kMaxTileSizes * sizeof(float);
        const auto totalBytes = mapBytesPadded + tileSizeBytes + 2 * sizeof(std::uint32_t);

        std::vector<std::uint8_t> buffer(totalBytes, 0);
        std::memcpy(buffer.data(), data.data(), mapBytes);

        auto* tileSizeDst = reinterpret_cast<float*>(buffer.data() + mapBytesPadded);
        for (std::uint32_t i = 0; i < kMaxTileSizes; ++i)
            tileSizeDst[i] = (i < tileSizeCount && tileSizes) ? std::max(1.0f, tileSizes[i]) : 8.0f;

        auto* splatDst = reinterpret_cast<std::uint32_t*>(buffer.data() + mapBytesPadded + tileSizeBytes);
        splatDst[0] = alphaMaskLayerFlags;
        splatDst[1] = std::min(splatLayerCount, kMaxTileSizes);

        impl_->terrainMapBuffer = make_static_buffer(buffer.data(), totalBytes);
        glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, 1, impl_->terrainMapBuffer.id);

        impl_->skyConstants[10] = mapSize;
        impl_->skyConstants[11] = static_cast<float>(side);
        impl_->terrainMapReady = true;
        return true;
    }

    bool OpenGLRenderer::create_preview_buffer(std::size_t) { return true; }
    bool OpenGLRenderer::create_preview_image(std::uint32_t, std::uint32_t) { return true; }

    bool OpenGLRenderer::set_preview_image(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& bgraPixels)
    {
        if (!ready_ || width == 0 || height == 0 || bgraPixels.size() < static_cast<std::size_t>(width) * height * 4)
            return false;

        // Convert BGRA -> RGBA for GL sampling.
        std::vector<std::uint8_t> rgba(bgraPixels.size());
        for (std::size_t i = 0; i + 4 <= bgraPixels.size(); i += 4)
        {
            rgba[i + 0] = bgraPixels[i + 2];
            rgba[i + 1] = bgraPixels[i + 1];
            rgba[i + 2] = bgraPixels[i + 0];
            rgba[i + 3] = bgraPixels[i + 3];
        }

        if (impl_->previewTexture && (impl_->previewWidth != width || impl_->previewHeight != height))
        {
            glDeleteTextures_(1, &impl_->previewTexture);
            impl_->previewTexture = 0;
        }
        if (!impl_->previewTexture)
        {
            glCreateTextures_(GL_TEXTURE_2D, 1, &impl_->previewTexture);
            glTextureParameteri_(impl_->previewTexture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri_(impl_->previewTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri_(impl_->previewTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri_(impl_->previewTexture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture_(GL_TEXTURE_2D, impl_->previewTexture);
        glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

        impl_->previewWidth = width;
        impl_->previewHeight = height;
        impl_->previewReady = true;
        return true;
    }

    void OpenGLRenderer::enter_loading_mode()
    {
        if (!impl_) return;
        impl_->terrainReady = false;
        impl_->objectsReady = false;
    }

    void OpenGLRenderer::set_camera(float x, float y, float z, float yaw, float pitch, float aspect, float farPlane)
    {
        if (!impl_) return;
        impl_->cameraConstants[0] = x;
        impl_->cameraConstants[1] = y;
        impl_->cameraConstants[2] = z;
        impl_->cameraConstants[3] = yaw;
        impl_->cameraConstants[4] = pitch;
        impl_->cameraConstants[5] = std::max(0.1f, aspect);
        impl_->cameraConstants[6] = 0.7002f;
        impl_->cameraConstants[7] = std::max(100.0f, farPlane);
        impl_->cameraConstants[8] = static_cast<float>(std::cos(static_cast<double>(yaw)));
        impl_->cameraConstants[9] = static_cast<float>(std::sin(static_cast<double>(yaw)));
        impl_->cameraConstants[10] = static_cast<float>(std::cos(static_cast<double>(pitch)));
        impl_->cameraConstants[11] = static_cast<float>(std::sin(static_cast<double>(pitch)));
    }

    void OpenGLRenderer::set_sky_settings(const float* fogColor, float fogStartDistance, float fogEndDistance, bool hasWorldSky)
    {
        if (!impl_ || !fogColor) return;
        impl_->skyConstants[0] = std::clamp(fogColor[0], 0.0f, 1.0f);
        impl_->skyConstants[1] = std::clamp(fogColor[1], 0.0f, 1.0f);
        impl_->skyConstants[2] = std::clamp(fogColor[2], 0.0f, 1.0f);
        impl_->skyConstants[3] = hasWorldSky ? 1.0f : 0.0f;
        impl_->skyConstants[4] = std::max(1.0f, fogStartDistance);
        impl_->skyConstants[5] = std::max(impl_->skyConstants[4] + 1.0f, fogEndDistance);
    }

    void OpenGLRenderer::set_sky_texture_layers(std::uint32_t skyLayer, std::uint32_t primaryCloudLayer, std::uint32_t secondaryCloudLayer)
    {
        if (!impl_) return;
        impl_->skyConstants[3] = (skyLayer != UINT32_MAX || primaryCloudLayer != UINT32_MAX || secondaryCloudLayer != UINT32_MAX) ? 1.0f : 0.0f;
        impl_->skyConstants[6] = skyLayer != UINT32_MAX ? static_cast<float>(skyLayer) : -1.0f;
        impl_->skyConstants[7] = primaryCloudLayer != UINT32_MAX ? static_cast<float>(primaryCloudLayer) : -1.0f;
        impl_->skyConstants[8] = secondaryCloudLayer != UINT32_MAX ? static_cast<float>(secondaryCloudLayer) : -1.0f;
    }

    void OpenGLRenderer::set_environment_style(const EnvironmentStyle& style)
    {
        if (!impl_) return;
        impl_->environmentStyle = style;
    }

    void OpenGLRenderer::set_shadows_enabled(bool enabled)
    {
        if (!impl_) return;
        impl_->shadowsEnabled = enabled;
    }

    bool OpenGLRenderer::set_vsync_enabled(bool enabled)
    {
        if (!impl_ || !ready_)
            return false;
        return SDL_GL_SetSwapInterval(enabled ? 1 : 0) == 0;
    }

    void OpenGLRenderer::set_antialiasing_enabled(bool enabled)
    {
        if (!impl_ || !ready_) return;
        impl_->antialiasingEnabled = enabled;
        if (enabled)
        {
            glEnable_(GL_MULTISAMPLE);
            glEnable_(GL_SAMPLE_ALPHA_TO_COVERAGE);
        }
        else
        {
            glDisable_(GL_MULTISAMPLE);
            glDisable_(GL_SAMPLE_ALPHA_TO_COVERAGE);
        }
    }

    void OpenGLRenderer::set_water_layer(std::uint32_t waterLayer)
    {
        if (!impl_) return;
        impl_->skyConstants[9] = waterLayer != UINT32_MAX ? static_cast<float>(waterLayer) : static_cast<float>(0xFFFFFFFFu);
    }

    void OpenGLRenderer::set_water_animation(std::uint32_t baseLayer, std::uint32_t frameCount, float tileSize)
    {
        if (!impl_) return;
        impl_->skyConstants[12] = static_cast<float>(baseLayer);
        impl_->skyConstants[13] = static_cast<float>(frameCount);
        impl_->skyConstants[15] = tileSize;
    }

    void OpenGLRenderer::update_water_time(float totalTime)
    {
        if (!impl_) return;
        impl_->skyConstants[14] = totalTime;
    }

    void OpenGLRenderer::render_frame()
    {
        if (!ready_) return;
        if (impl_->surfaceWidth == 0 || impl_->surfaceHeight == 0) return;

        const bool hasScene = impl_->terrainReady || impl_->objectsReady;
        const bool uiOnlyFrame = !impl_->screenUi.empty() && !hasScene;

        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        glViewport_(0, 0, static_cast<GLsizei>(impl_->surfaceWidth), static_cast<GLsizei>(impl_->surfaceHeight));

        if (hasScene || uiOnlyFrame)
        {
            glClearColor_(impl_->skyConstants[0], impl_->skyConstants[1], impl_->skyConstants[2], 1.0f);
            glClearDepth_(1.0);
            glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable_(GL_DEPTH_TEST);

            float constants[kCameraConstantFloatCount]{};
            std::memcpy(constants, impl_->cameraConstants, sizeof(impl_->cameraConstants));
            std::memcpy(constants + 12, impl_->skyConstants, sizeof(impl_->skyConstants));
            static_assert(sizeof(EnvironmentStyle) == sizeof(float) * 96u);
            std::memcpy(constants + 28, &impl_->environmentStyle, sizeof(impl_->environmentStyle));
            build_directional_shadow_matrix(impl_->shadowMatrix, impl_->cameraConstants,
                impl_->environmentStyle.lightDirectionEnergy, impl_->cameraConstants[7]);
            impl_->shadowInfo[0] = impl_->shadowReady && impl_->shadowsEnabled
                && impl_->environmentStyle.lightDirectionEnergy[3] > 0.01f ? 1.0f : 0.0f;
            std::memcpy(constants + 124, impl_->shadowMatrix, sizeof(impl_->shadowMatrix));
            std::memcpy(constants + 140, impl_->shadowInfo, sizeof(impl_->shadowInfo));

            // Every draw call below re-specified its program and re-uploaded the
            // camera UBO even when back-to-back draws share both (e.g. monster then
            // NPC both use skinnedCharacterProgram + `constants`). Track the last
            // bound program and camera upload so repeats become no-ops instead of
            // redundant glUseProgram/glNamedBufferSubData calls.
            GLuint boundProgram = 0;
            const float* uploadedConstants = nullptr;
            const auto useProgram = [&](GLuint program) {
                if (program != boundProgram)
                {
                    glUseProgram_(program);
                    boundProgram = program;
                }
            };
            const auto useCamera = [&](const float* values) {
                if (values != uploadedConstants)
                {
                    set_camera_ubo(impl_->cameraUbo, values);
                    uploadedConstants = values;
                }
            };

            const auto frame = frameIndex_ % kMaxFramesInFlight;

            if (impl_->shadowInfo[0] > 0.5f)
            {
                if (impl_->terrainTexturesReady)
                {
                    glBindTextureUnit_(0, impl_->terrainTextureArray.id);
                    glBindSampler_(0, impl_->terrainSampler);
                }
                glBindFramebuffer_(GL_FRAMEBUFFER, impl_->shadowFramebuffer);
                glViewport_(0, 0, static_cast<GLsizei>(Impl::shadowResolution),
                    static_cast<GLsizei>(Impl::shadowResolution));
                glClearDepth_(1.0);
                glClear_(GL_DEPTH_BUFFER_BIT);
                glEnable_(GL_DEPTH_TEST);
                glDepthMask_(GL_TRUE);
                glDisable_(GL_BLEND);
                glDisable_(GL_MULTISAMPLE);
                glDisable_(GL_SAMPLE_ALPHA_TO_COVERAGE);
                set_camera_ubo(impl_->cameraUbo, constants);

                const auto drawRanges = [&](GLuint vao, const std::vector<ObjectBatch>& batches,
                    GLuint program) {
                    if (!program || batches.empty()) return;
                    glUseProgram_(program);
                    glBindVertexArray_(vao);
                    for (const auto& batch : batches)
                    {
                        if (batch.instanceCount == 0 || batch.indexCount == 0) continue;
                        glDrawElementsInstancedBaseInstance_(GL_TRIANGLES,
                            static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT,
                            (const void*)(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t)),
                            static_cast<GLsizei>(batch.instanceCount), batch.firstInstance);
                    }
                };
                const auto drawIndirect = [&](GLuint vao, const GlBuffer& commands,
                    std::uint32_t drawCount, GLuint program) {
                    if (!program || !commands.id || drawCount == 0) return;
                    glUseProgram_(program);
                    glBindVertexArray_(vao);
                    glBindBuffer_(GL_DRAW_INDIRECT_BUFFER, commands.id);
                    glMultiDrawElementsIndirect_(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr,
                        static_cast<GLsizei>(drawCount), sizeof(DrawElementsIndirectCommand));
                    glBindBuffer_(GL_DRAW_INDIRECT_BUFFER, 0);
                };

                if (impl_->terrainReady)
                {
                    glUseProgram_(impl_->shadowTerrainProgram);
                    glBindVertexArray_(impl_->terrainVao);
                    if (impl_->terrainDrawRanges.empty())
                        glDrawElements_(GL_TRIANGLES, static_cast<GLsizei>(impl_->terrainIndexCount), GL_UNSIGNED_INT, nullptr);
                    else
                        for (const auto& range : impl_->terrainDrawRanges)
                            if (range.indexCount > 0)
                                glDrawElements_(GL_TRIANGLES, static_cast<GLsizei>(range.indexCount), GL_UNSIGNED_INT,
                                    (const void*)(static_cast<std::uintptr_t>(range.firstIndex) * sizeof(std::uint32_t)));
                }
                if (impl_->objectsReady)
                    drawIndirect(impl_->objectVao, impl_->objectIndirectBuffer,
                        impl_->objectIndirectCount, impl_->shadowStaticProgram);
                if (impl_->animatedObjectsReady)
                    drawRanges(impl_->animatedObjectVao, impl_->animatedObjectBatches, impl_->shadowStaticProgram);
                if (impl_->characterVisible && impl_->characterReady)
                {
                    glUseProgram_(impl_->shadowTerrainProgram);
                    glBindVertexArray_(impl_->characterVao);
                    glDrawElements_(GL_TRIANGLES, static_cast<GLsizei>(impl_->characterIndexCount), GL_UNSIGNED_INT, nullptr);
                }
                if (impl_->botCharacterVisible && impl_->botCharacterReady)
                    drawRanges(impl_->botCharacterVao, impl_->botCharacterBatches, impl_->shadowStaticProgram);
                if (impl_->monsterCharacterVisible && impl_->monsterCharacterReady
                    && impl_->monsterPaletteBuffer[frame].id)
                {
                    glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, 3, impl_->monsterPaletteBuffer[frame].id);
                    drawRanges(impl_->monsterCharacterVao, impl_->monsterCharacterBatches,
                        impl_->shadowSkinnedProgram);
                }
                if (impl_->npcCharacterVisible && impl_->npcCharacterReady
                    && impl_->npcPaletteBuffer[frame].id)
                {
                    glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, 3, impl_->npcPaletteBuffer[frame].id);
                    drawRanges(impl_->npcCharacterVao, impl_->npcCharacterBatches,
                        impl_->shadowSkinnedProgram);
                }

                glBindFramebuffer_(GL_FRAMEBUFFER, 0);
                glViewport_(0, 0, static_cast<GLsizei>(impl_->surfaceWidth),
                    static_cast<GLsizei>(impl_->surfaceHeight));
                glEnable_(GL_BLEND);
                if (impl_->antialiasingEnabled)
                {
                    glEnable_(GL_MULTISAMPLE);
                    glEnable_(GL_SAMPLE_ALPHA_TO_COVERAGE);
                }
                boundProgram = 0;
                uploadedConstants = nullptr;
            }

            if (impl_->terrainTexturesReady)
            {
                glBindTextureUnit_(0, impl_->terrainTextureArray.id);
                glBindSampler_(0, impl_->terrainSampler);
            }
            glBindTextureUnit_(2, impl_->lightmapTexture.id);
            glBindSampler_(2, impl_->lightmapSampler);
            if (impl_->debugEffectTexturesReady)
            {
                glBindTextureUnit_(3, impl_->debugEffectTextureArray.id);
                glBindSampler_(3, impl_->debugEffectSampler);
            }
            glBindTextureUnit_(4, impl_->environmentNoiseTexture);
            glBindSampler_(4, impl_->terrainSampler);
            glBindTextureUnit_(5, impl_->shadowDepthTexture);
            glBindSampler_(5, impl_->shadowSampler);
            glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, 1, impl_->terrainMapBuffer.id);

            // sky
            {
                const bool blackFog = impl_->skyConstants[0] < 0.01f && impl_->skyConstants[1] < 0.01f && impl_->skyConstants[2] < 0.01f;
                if (impl_->skyReady && !blackFog)
                {
                    glDisable_(GL_DEPTH_TEST);
                    useProgram(impl_->skyProgram);
                    useCamera(constants);
                    glBindVertexArray_(impl_->emptyVao);
                    glDrawArrays_(GL_TRIANGLES, 0, 3);
                    glEnable_(GL_DEPTH_TEST);
                }
            }

            if (impl_->terrainReady)
            {
                useProgram(impl_->terrainProgram);
                useCamera(constants);
                glBindVertexArray_(impl_->terrainVao);
                if (impl_->terrainDrawRanges.empty())
                {
                    glDrawElements_(GL_TRIANGLES, static_cast<GLsizei>(impl_->terrainIndexCount), GL_UNSIGNED_INT, nullptr);
                }
                else
                {
                    for (const auto& range : impl_->terrainDrawRanges)
                    {
                        if (range.indexCount > 0)
                            glDrawElements_(GL_TRIANGLES, static_cast<GLsizei>(range.indexCount), GL_UNSIGNED_INT,
                                (const void*)(static_cast<std::uintptr_t>(range.firstIndex) * sizeof(std::uint32_t)));
                    }
                }
            }

            auto drawStaticBatches = [&](GLuint vao, const std::vector<ObjectBatch>& batches,
                const float* push, const GlBuffer* indirect = nullptr,
                std::uint32_t indirectCount = 0)
            {
                if (batches.empty()) return;
                useProgram(impl_->staticObjectProgram);
                useCamera(push);
                glBindVertexArray_(vao);
                if (indirect && indirect->id && indirectCount > 0)
                {
                    glBindBuffer_(GL_DRAW_INDIRECT_BUFFER, indirect->id);
                    glMultiDrawElementsIndirect_(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr,
                        static_cast<GLsizei>(indirectCount), sizeof(DrawElementsIndirectCommand));
                    glBindBuffer_(GL_DRAW_INDIRECT_BUFFER, 0);
                    return;
                }
                for (const auto& batch : batches)
                {
                    if (batch.instanceCount == 0 || batch.indexCount == 0) continue;
                    glDrawElementsInstancedBaseInstance_(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT,
                        (const void*)(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t)),
                        static_cast<GLsizei>(batch.instanceCount), batch.firstInstance);
                }
            };

            // World props intentionally use their own Godot-equivalent
            // trilinear sampler. Terrain keeps its sharper anisotropic sampler.
            if (impl_->assetTexturesReady)
                glBindTextureUnit_(0, impl_->assetTextureArray.id);
            glBindSampler_(0, impl_->assetSampler);
            if (impl_->objectsReady && impl_->staticObjectProgram)
                drawStaticBatches(impl_->objectVao, impl_->objectBatches, constants,
                    &impl_->objectIndirectBuffer, impl_->objectIndirectCount);
            if (impl_->animatedObjectsReady && impl_->staticObjectProgram)
                drawStaticBatches(impl_->animatedObjectVao, impl_->animatedObjectBatches, constants);
            if (impl_->assetTexturesReady)
                glBindTextureUnit_(0, impl_->terrainTextureArray.id);
            glBindSampler_(0, impl_->terrainSampler);

            if (impl_->characterVisible && impl_->characterReady)
            {
                useProgram(impl_->terrainProgram);
                useCamera(constants);
                glBindVertexArray_(impl_->characterVao);
                glDrawElements_(GL_TRIANGLES, static_cast<GLsizei>(impl_->characterIndexCount), GL_UNSIGNED_INT, nullptr);
            }
            if (impl_->botCharacterVisible && impl_->botCharacterReady
                && impl_->botCharacterInstanceBuffer.id && !impl_->botCharacterBatches.empty()
                && impl_->staticObjectProgram)
            {
                drawStaticBatches(impl_->botCharacterVao, impl_->botCharacterBatches, constants);
            }
            if (impl_->monsterCharacterVisible && impl_->monsterCharacterReady
                && impl_->monsterCharacterInstanceBuffer[frame].id && !impl_->monsterCharacterBatches.empty()
                && impl_->monsterCharacterSkinned && impl_->skinnedCharacterProgram
                && impl_->monsterPaletteBuffer[frame].id)
            {
                useProgram(impl_->skinnedCharacterProgram);
                useCamera(constants);
                glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, 3, impl_->monsterPaletteBuffer[frame].id);
                glBindVertexArray_(impl_->monsterCharacterVao);
                for (const auto& batch : impl_->monsterCharacterBatches)
                {
                    if (batch.instanceCount == 0 || batch.indexCount == 0) continue;
                    glDrawElementsInstancedBaseInstance_(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT,
                        (const void*)(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t)),
                        static_cast<GLsizei>(batch.instanceCount), batch.firstInstance);
                }
            }
            if (impl_->npcCharacterVisible && impl_->npcCharacterReady
                && impl_->npcCharacterInstanceBuffer[frame].id && !impl_->npcCharacterBatches.empty()
                && impl_->skinnedCharacterProgram && impl_->npcPaletteBuffer[frame].id)
            {
                useProgram(impl_->skinnedCharacterProgram);
                useCamera(constants);
                glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, 3, impl_->npcPaletteBuffer[frame].id);
                glBindVertexArray_(impl_->npcCharacterVao);
                for (const auto& batch : impl_->npcCharacterBatches)
                {
                    if (batch.instanceCount == 0 || batch.indexCount == 0) continue;
                    glDrawElementsInstancedBaseInstance_(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT,
                        (const void*)(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t)),
                        static_cast<GLsizei>(batch.instanceCount), batch.firstInstance);
                }
            }
            if (impl_->waterReady && impl_->waterIndexCount > 0)
            {
                // Same reasoning as the effect-particle pass below: MSAA/
                // alpha-to-coverage on the water surface's blended,
                // constantly-animated edges reads wrong, so it's excluded
                // too, restored right after.
                glDisable_(GL_MULTISAMPLE);
                glDisable_(GL_SAMPLE_ALPHA_TO_COVERAGE);
                useProgram(impl_->terrainProgram);
                useCamera(constants);
                glBindVertexArray_(impl_->waterVao);
                glDrawElements_(GL_TRIANGLES, static_cast<GLsizei>(impl_->waterIndexCount), GL_UNSIGNED_INT, nullptr);
                if (impl_->antialiasingEnabled)
                {
                    glEnable_(GL_MULTISAMPLE);
                    glEnable_(GL_SAMPLE_ALPHA_TO_COVERAGE);
                }
            }

            if (impl_->effectParticlesReady && impl_->effectParticleProgram && !impl_->effectParticleBatches.empty())
            {
                useProgram(impl_->effectParticleProgram);
                useCamera(constants);
                glBindVertexArray_(impl_->effectParticleVao);
                // Effects must not be touched by anti-aliasing: MSAA softens
                // billboard/quad edges in a way that looks wrong against
                // additive/alpha-blended sprite art, and GL_SAMPLE_ALPHA_TO_
                // COVERAGE (meant for alpha-tested cutouts) actively
                // dithers/mis-renders the soft alpha edges these particles
                // rely on (effect_particle.frag alpha-discards, so it reads
                // as a cutout to the coverage logic even though it's really
                // smoothly blended). Disabled just for this pass, restored
                // to whatever the user's toggle is set to right after.
                glDisable_(GL_MULTISAMPLE);
                glDisable_(GL_SAMPLE_ALPHA_TO_COVERAGE);
                // Particles don't occlude each other or write depth, but still
                // test against opaque scene depth so they hide behind terrain/buildings.
                glDepthMask_(GL_FALSE);
                GLenum boundSrc = GL_SRC_ALPHA;
                GLenum boundDst = GL_ONE_MINUS_SRC_ALPHA;
                for (const auto& batch : impl_->effectParticleBatches)
                {
                    if (batch.instanceCount == 0 || batch.indexCount == 0) continue;
                    const auto src = d3d_blend_to_gl(batch.sourceBlend, true);
                    const auto dst = d3d_blend_to_gl(batch.destinationBlend, false);
                    if (src != boundSrc || dst != boundDst)
                    {
                        glBlendFunc_(src, dst);
                        boundSrc = src;
                        boundDst = dst;
                    }
                    glDrawElementsInstancedBaseInstance_(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT,
                        (const void*)(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t)),
                        static_cast<GLsizei>(batch.instanceCount), batch.firstInstance);
                }
                glDepthMask_(GL_TRUE);
                glBlendFunc_(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                if (impl_->antialiasingEnabled)
                {
                    glEnable_(GL_MULTISAMPLE);
                    glEnable_(GL_SAMPLE_ALPHA_TO_COVERAGE);
                }
            }
            if (impl_->worldLabelsReady && impl_->worldLabelProgram
                && (!impl_->worldLabels.empty() || !impl_->screenUi.empty()))
            {
                std::vector<WorldLabelVertex> vertices;
                vertices.reserve((impl_->worldLabels.size() + impl_->screenUi.size()) * 6u * 32u);

                const auto decode = [](const std::string& text) {
                    std::vector<std::uint32_t> codepoints;
                    codepoints.reserve(text.size());
                    for (std::size_t i = 0; i < text.size();)
                    {
                        const auto first = static_cast<std::uint8_t>(text[i++]);
                        std::uint32_t codepoint = first;
                        int continuationCount = 0;
                        if ((first & 0xE0u) == 0xC0u) { codepoint = first & 0x1Fu; continuationCount = 1; }
                        else if ((first & 0xF0u) == 0xE0u) { codepoint = first & 0x0Fu; continuationCount = 2; }
                        else if ((first & 0xF8u) == 0xF0u) { codepoint = first & 0x07u; continuationCount = 3; }
                        for (int j = 0; j < continuationCount && i < text.size(); ++j)
                        {
                            const auto next = static_cast<std::uint8_t>(text[i++]);
                            if ((next & 0xC0u) != 0x80u) { codepoint = '?'; break; }
                            codepoint = (codepoint << 6u) | (next & 0x3Fu);
                        }
                        codepoints.push_back(codepoint >= 32u && codepoint <= 255u ? codepoint : '?');
                    }
                    return codepoints;
                };
                const auto appendText = [&](const ScreenLabel& label, float offsetX, float offsetY,
                    const float (&color)[4]) {
                    const auto codepoints = decode(label.text);
                    float width = 0.0f;
                    for (const auto codepoint : codepoints)
                        width += impl_->worldLabelGlyphs[codepoint - 32u].advance;
                    float cursorX = std::floor(label.centerX - width * 0.5f + offsetX);
                    const float baseline = std::floor(label.topY + 13.0f + offsetY);
                    for (const auto codepoint : codepoints)
                    {
                        const auto& glyph = impl_->worldLabelGlyphs[codepoint - 32u];
                        const float x0 = cursorX + glyph.x0;
                        const float y0 = baseline + glyph.y0;
                        const float x1 = cursorX + glyph.x1;
                        const float y1 = baseline + glyph.y1;
                        const WorldLabelVertex quad[6]{
                            { { x0, y0 }, { glyph.u0, glyph.v0 }, { color[0], color[1], color[2], color[3] } },
                            { { x1, y0 }, { glyph.u1, glyph.v0 }, { color[0], color[1], color[2], color[3] } },
                            { { x1, y1 }, { glyph.u1, glyph.v1 }, { color[0], color[1], color[2], color[3] } },
                            { { x0, y0 }, { glyph.u0, glyph.v0 }, { color[0], color[1], color[2], color[3] } },
                            { { x1, y1 }, { glyph.u1, glyph.v1 }, { color[0], color[1], color[2], color[3] } },
                            { { x0, y1 }, { glyph.u0, glyph.v1 }, { color[0], color[1], color[2], color[3] } },
                        };
                        vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
                        cursorX += glyph.advance;
                    }
                };

                constexpr float outline[4]{ 0.0f, 0.0f, 0.0f, 0.92f };
                static constexpr float outlineOffsets[4][2]{
                    { -1.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, -1.0f }, { 0.0f, 1.0f }
                };
                for (const auto& label : impl_->worldLabels)
                {
                    for (const auto& offset : outlineOffsets)
                        appendText(label, offset[0], offset[1], outline);
                    const float color[4]{
                        label.color[0] / 255.0f, label.color[1] / 255.0f,
                        label.color[2] / 255.0f, label.color[3] / 255.0f
                    };
                    appendText(label, 0.0f, 0.0f, color);
                }

                // PhoenixUI shares this atlas and vertex buffer with native
                // world labels. Rectangles bypass the atlas; text samples it.
                // Commands remain in submission order, so later widgets and
                // combo overlays naturally cover earlier panel contents.
                const auto commandClip = [&](const ScreenUiCommand& item) {
                    if (item.clipWidth <= 0.0f || item.clipHeight <= 0.0f)
                        return std::array<float, 4>{ 0.0f, 0.0f,
                            static_cast<float>(impl_->surfaceWidth), static_cast<float>(impl_->surfaceHeight) };
                    return std::array<float, 4>{ item.clipX, item.clipY,
                        item.clipX + item.clipWidth, item.clipY + item.clipHeight };
                };
                const auto appendRectangle = [&](const ScreenUiCommand& item) {
                    const auto clip = commandClip(item);
                    const float x0 = std::max(item.x, clip[0]);
                    const float y0 = std::max(item.y, clip[1]);
                    const float x1 = std::min(item.x + item.width, clip[2]);
                    const float y1 = std::min(item.y + item.height, clip[3]);
                    if (x1 <= x0 || y1 <= y0)
                        return;
                    const WorldLabelVertex quad[6]{
                        { { x0, y0 }, {}, { item.color[0], item.color[1], item.color[2], item.color[3] }, 0.0f },
                        { { x1, y0 }, {}, { item.color[0], item.color[1], item.color[2], item.color[3] }, 0.0f },
                        { { x1, y1 }, {}, { item.color[0], item.color[1], item.color[2], item.color[3] }, 0.0f },
                        { { x0, y0 }, {}, { item.color[0], item.color[1], item.color[2], item.color[3] }, 0.0f },
                        { { x1, y1 }, {}, { item.color[0], item.color[1], item.color[2], item.color[3] }, 0.0f },
                        { { x0, y1 }, {}, { item.color[0], item.color[1], item.color[2], item.color[3] }, 0.0f },
                    };
                    vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
                };
                const auto appendUiText = [&](const ScreenUiCommand& item) {
                    const auto clip = commandClip(item);
                    const auto codepoints = decode(item.text);
                    float cursorX = std::floor(item.x);
                    const float baseline = std::floor(item.y + 13.0f);
                    for (const auto codepoint : codepoints)
                    {
                        const auto& glyph = impl_->worldLabelGlyphs[codepoint - 32u];
                        const float originalX0 = cursorX + glyph.x0;
                        const float originalY0 = baseline + glyph.y0;
                        const float originalX1 = cursorX + glyph.x1;
                        const float originalY1 = baseline + glyph.y1;
                        const float x0 = std::max(originalX0, clip[0]);
                        const float y0 = std::max(originalY0, clip[1]);
                        const float x1 = std::min(originalX1, clip[2]);
                        const float y1 = std::min(originalY1, clip[3]);
                        if (x1 > x0 && y1 > y0)
                        {
                            const float width = std::max(0.0001f, originalX1 - originalX0);
                            const float height = std::max(0.0001f, originalY1 - originalY0);
                            const float u0 = glyph.u0 + (glyph.u1 - glyph.u0) * ((x0 - originalX0) / width);
                            const float v0 = glyph.v0 + (glyph.v1 - glyph.v0) * ((y0 - originalY0) / height);
                            const float u1 = glyph.u0 + (glyph.u1 - glyph.u0) * ((x1 - originalX0) / width);
                            const float v1 = glyph.v0 + (glyph.v1 - glyph.v0) * ((y1 - originalY0) / height);
                            const WorldLabelVertex quad[6]{
                                { { x0, y0 }, { u0, v0 }, { item.color[0], item.color[1], item.color[2], item.color[3] } },
                                { { x1, y0 }, { u1, v0 }, { item.color[0], item.color[1], item.color[2], item.color[3] } },
                                { { x1, y1 }, { u1, v1 }, { item.color[0], item.color[1], item.color[2], item.color[3] } },
                                { { x0, y0 }, { u0, v0 }, { item.color[0], item.color[1], item.color[2], item.color[3] } },
                                { { x1, y1 }, { u1, v1 }, { item.color[0], item.color[1], item.color[2], item.color[3] } },
                                { { x0, y1 }, { u0, v1 }, { item.color[0], item.color[1], item.color[2], item.color[3] } },
                            };
                            vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
                        }
                        cursorX += glyph.advance;
                    }
                };
                for (const auto& item : impl_->screenUi)
                {
                    if (item.kind == ScreenUiCommandKind::Rectangle)
                        appendRectangle(item);
                    else
                        appendUiText(item);
                }

                if (!vertices.empty())
                {
                    const auto bytes = vertices.size() * sizeof(WorldLabelVertex);
                    if (bytes > impl_->worldLabelVertexBuffer.byteSize)
                    {
                        glNamedBufferData_(impl_->worldLabelVertexBuffer.id,
                            static_cast<GLsizeiptr>(bytes), vertices.data(), GL_DYNAMIC_DRAW);
                        impl_->worldLabelVertexBuffer.byteSize = bytes;
                    }
                    else
                    {
                        glNamedBufferSubData_(impl_->worldLabelVertexBuffer.id, 0,
                            static_cast<GLsizeiptr>(bytes), vertices.data());
                    }
                    glDisable_(GL_DEPTH_TEST);
                    glDepthMask_(GL_FALSE);
                    glDisable_(GL_MULTISAMPLE);
                    glDisable_(GL_SAMPLE_ALPHA_TO_COVERAGE);
                    glBlendFunc_(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    useProgram(impl_->worldLabelProgram);
                    const float viewport[4]{
                        static_cast<float>(impl_->surfaceWidth), static_cast<float>(impl_->surfaceHeight), 0.0f, 0.0f
                    };
                    glUniform4fv_(0, 1, viewport);
                    glBindTextureUnit_(0, impl_->worldLabelTexture);
                    // Unit 0 normally carries the world's mipmapped terrain
                    // sampler. The label atlas has a single level and must use
                    // its own texture filtering state or it becomes incomplete.
                    glBindSampler_(0, 0);
                    glBindVertexArray_(impl_->worldLabelVao);
                    glDrawArrays_(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
                    glDepthMask_(GL_TRUE);
                    glEnable_(GL_DEPTH_TEST);
                    if (impl_->antialiasingEnabled)
                    {
                        glEnable_(GL_MULTISAMPLE);
                        glEnable_(GL_SAMPLE_ALPHA_TO_COVERAGE);
                    }
                }
            }
        }
        else
        {
            if (impl_->previewReady && impl_->previewTexture)
            {
                // Blit the preview texture to the default framebuffer at the
                // window size using a fixed-function-free full screen pass
                // through the sky program's fullscreen-triangle trick would
                // need a dedicated blit shader; instead use glBlitNamedFramebuffer
                // by attaching the preview texture to a temp read FBO.
                GLuint fbo{};
                glGenFramebuffers_(1, &fbo);
                glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, impl_->previewTexture, 0);
                glBindFramebuffer_(GL_READ_FRAMEBUFFER, fbo);
                glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, 0);
                glBlitFramebuffer_(0, 0, static_cast<GLint>(impl_->previewWidth), static_cast<GLint>(impl_->previewHeight),
                    0, static_cast<GLint>(impl_->surfaceHeight), static_cast<GLint>(impl_->surfaceWidth), 0,
                    GL_COLOR_BUFFER_BIT, GL_LINEAR);
                glBindFramebuffer_(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers_(1, &fbo);
            }
            else
            {
                glClearColor_(0.09f, 0.11f, 0.13f, 1.0f);
                glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            }
        }

        SDL_GL_SwapWindow(impl_->window);
        ++frameIndex_;
    }

    void OpenGLRenderer::shutdown()
    {
        if (!impl_) return;
        ready_ = false;

        auto destroyBuf = [](GlBuffer& b) { if (b.id) glDeleteBuffers_(1, &b.id); b = {}; };
        destroyBuf(impl_->terrainVertexBuffer);
        destroyBuf(impl_->terrainIndexBuffer);
        destroyBuf(impl_->waterVertexBuffer);
        destroyBuf(impl_->waterIndexBuffer);
        destroyBuf(impl_->objectVertexBuffer);
        destroyBuf(impl_->objectIndexBuffer);
        destroyBuf(impl_->objectInstanceBuffer);
        destroyBuf(impl_->objectIndirectBuffer);
        destroyBuf(impl_->animatedObjectVertexBuffer);
        destroyBuf(impl_->animatedObjectIndexBuffer);
        destroyBuf(impl_->animatedObjectInstanceBuffer);
        destroyBuf(impl_->characterVertexBuffer);
        destroyBuf(impl_->characterIndexBuffer);
        destroyBuf(impl_->botCharacterVertexBuffer);
        destroyBuf(impl_->botCharacterIndexBuffer);
        destroyBuf(impl_->botCharacterInstanceBuffer);
        destroyBuf(impl_->monsterCharacterVertexBuffer);
        destroyBuf(impl_->monsterCharacterIndexBuffer);
        destroyBuf(impl_->npcCharacterVertexBuffer);
        destroyBuf(impl_->npcCharacterIndexBuffer);
        destroyBuf(impl_->terrainMapBuffer);
        destroyBuf(impl_->effectParticleVertexBuffer);
        destroyBuf(impl_->effectParticleIndexBuffer);
        destroyBuf(impl_->effectParticleInstanceBuffer);
        destroyBuf(impl_->worldLabelVertexBuffer);
        for (auto& b : impl_->monsterCharacterInstanceBuffer) destroyBuf(b);
        for (auto& b : impl_->monsterPaletteBuffer) destroyBuf(b);
        for (auto& b : impl_->npcCharacterInstanceBuffer) destroyBuf(b);
        for (auto& b : impl_->npcPaletteBuffer) destroyBuf(b);

        auto destroyVao = [](GLuint& v) { if (v) glDeleteVertexArrays_(1, &v); v = 0; };
        destroyVao(impl_->terrainVao);
        destroyVao(impl_->waterVao);
        destroyVao(impl_->objectVao);
        destroyVao(impl_->animatedObjectVao);
        destroyVao(impl_->characterVao);
        destroyVao(impl_->botCharacterVao);
        destroyVao(impl_->monsterCharacterVao);
        destroyVao(impl_->npcCharacterVao);
        destroyVao(impl_->effectParticleVao);
        destroyVao(impl_->worldLabelVao);
        destroyVao(impl_->emptyVao);

        if (impl_->cameraUbo) glDeleteBuffers_(1, &impl_->cameraUbo);

        if (impl_->terrainTextureArray.id) glDeleteTextures_(1, &impl_->terrainTextureArray.id);
        if (impl_->assetTextureArray.id) glDeleteTextures_(1, &impl_->assetTextureArray.id);
        if (impl_->debugEffectTextureArray.id) glDeleteTextures_(1, &impl_->debugEffectTextureArray.id);
        if (impl_->lightmapTexture.id) glDeleteTextures_(1, &impl_->lightmapTexture.id);
        if (impl_->previewTexture) glDeleteTextures_(1, &impl_->previewTexture);
        if (impl_->environmentNoiseTexture) glDeleteTextures_(1, &impl_->environmentNoiseTexture);
        if (impl_->shadowDepthTexture) glDeleteTextures_(1, &impl_->shadowDepthTexture);
        if (impl_->worldLabelTexture) glDeleteTextures_(1, &impl_->worldLabelTexture);
        if (impl_->terrainSampler) glDeleteSamplers_(1, &impl_->terrainSampler);
        if (impl_->assetSampler) glDeleteSamplers_(1, &impl_->assetSampler);
        if (impl_->debugEffectSampler) glDeleteSamplers_(1, &impl_->debugEffectSampler);
        if (impl_->lightmapSampler) glDeleteSamplers_(1, &impl_->lightmapSampler);
        if (impl_->shadowSampler) glDeleteSamplers_(1, &impl_->shadowSampler);
        if (impl_->shadowFramebuffer) glDeleteFramebuffers_(1, &impl_->shadowFramebuffer);

        auto destroyProg = [](GLuint& p) { if (p) glDeleteProgram_(p); p = 0; };
        destroyProg(impl_->terrainProgram);
        destroyProg(impl_->staticObjectProgram);
        destroyProg(impl_->skinnedCharacterProgram);
        destroyProg(impl_->effectParticleProgram);
        destroyProg(impl_->worldLabelProgram);
        destroyProg(impl_->skyProgram);
        destroyProg(impl_->shadowTerrainProgram);
        destroyProg(impl_->shadowStaticProgram);
        destroyProg(impl_->shadowSkinnedProgram);

        delete impl_;
        impl_ = nullptr;
    }

    void OpenGLRenderer::destroy_swapchain() {}
}
