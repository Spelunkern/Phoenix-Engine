#include "imgui_impl_opengl3.h"
#include "renderer/gl/gl_loader.h"

#include <cstddef>

namespace
{
    GLuint g_program = 0;
    GLuint g_vao = 0;
    GLuint g_vbo = 0;
    GLuint g_ebo = 0;
    GLuint g_fontTexture = 0;
    GLint g_uniformTex = -1;
    GLint g_uniformProjMtx = -1;
    GLint g_uniformUseTexColor = -1;

    const char* kVertexSrc =
        "#version 450 core\n"
        "layout(location=0) in vec2 aPos;\n"
        "layout(location=1) in vec2 aUv;\n"
        "layout(location=2) in vec4 aColor;\n"
        "uniform mat4 ProjMtx;\n"
        "out vec2 vUv;\n"
        "out vec4 vColor;\n"
        "void main() {\n"
        "    vUv = aUv;\n"
        "    vColor = aColor;\n"
        "    gl_Position = ProjMtx * vec4(aPos, 0.0, 1.0);\n"
        "}\n";

    const char* kFragmentSrc =
        "#version 450 core\n"
        "in vec2 vUv;\n"
        "in vec4 vColor;\n"
        "uniform sampler2D Tex;\n"
        "uniform int uUseTexColor;\n"
        "out vec4 outColor;\n"
        // The font atlas RGB is always white by design (ImGui bakes coverage
        // into alpha only); sampling texel.rgb back out was reading near-black
        // (font atlas upload/sampling only preserves alpha reliably here),
        // making every glyph and filled shape invisible against the dark
        // theme. So for the font atlas we use vColor.rgb and take only
        // coverage from alpha. Custom RGBA images (e.g. perf HUD vendor
        // icons) are real colour textures though, so uUseTexColor (set per
        // draw call based on which texture is bound) switches to sampling
        // the texture's own RGB instead of discarding it.
        "void main() {\n"
        "    vec4 texel = texture(Tex, vUv);\n"
        "    if (uUseTexColor != 0)\n"
        "        outColor = texel * vColor;\n"
        "    else\n"
        "        outColor = vec4(vColor.rgb, vColor.a * texel.a);\n"
        "}\n";

    GLuint compile(GLenum stage, const char* src)
    {
        GLuint s = glCreateShader_(stage);
        glShaderSource_(s, 1, &src, nullptr);
        glCompileShader_(s);
        GLint status = 0;
        glGetShaderiv_(s, GL_COMPILE_STATUS, &status);
        if (!status) { glDeleteShader_(s); return 0; }
        return s;
    }
}

bool ImGui_ImplOpenGL3_Init(const char*)
{
    GLuint vs = compile(GL_VERTEX_SHADER, kVertexSrc);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentSrc);
    if (!vs || !fs)
        return false;

    g_program = glCreateProgram_();
    glAttachShader_(g_program, vs);
    glAttachShader_(g_program, fs);
    glLinkProgram_(g_program);
    GLint linkStatus = 0;
    glGetProgramiv_(g_program, GL_LINK_STATUS, &linkStatus);
    glDeleteShader_(vs);
    glDeleteShader_(fs);
    if (!linkStatus)
        return false;

    g_uniformTex = glGetUniformLocation_(g_program, "Tex");
    g_uniformProjMtx = glGetUniformLocation_(g_program, "ProjMtx");
    g_uniformUseTexColor = glGetUniformLocation_(g_program, "uUseTexColor");

    glCreateVertexArrays_(1, &g_vao);
    glCreateBuffers_(1, &g_vbo);
    glCreateBuffers_(1, &g_ebo);

    glBindVertexArray_(g_vao);
    glBindBuffer_(GL_ARRAY_BUFFER, g_vbo);
    glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, g_ebo);
    const GLsizei stride = sizeof(ImDrawVert);
    glEnableVertexAttribArray_(0);
    glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ImDrawVert, pos));
    glEnableVertexAttribArray_(1);
    glVertexAttribPointer_(1, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ImDrawVert, uv));
    glEnableVertexAttribArray_(2);
    glVertexAttribPointer_(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (const void*)offsetof(ImDrawVert, col));
    glBindVertexArray_(0);

    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    glCreateTextures_(GL_TEXTURE_2D, 1, &g_fontTexture);
    glTextureParameteri_(g_fontTexture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri_(g_fontTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri_(g_fontTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri_(g_fontTexture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture_(GL_TEXTURE_2D, g_fontTexture);
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture_(GL_TEXTURE_2D, 0);
    io.Fonts->SetTexID(static_cast<ImTextureID>(static_cast<std::uintptr_t>(g_fontTexture)));

    return true;
}

void ImGui_ImplOpenGL3_Shutdown()
{
    if (g_vbo) glDeleteBuffers_(1, &g_vbo);
    if (g_ebo) glDeleteBuffers_(1, &g_ebo);
    if (g_vao) glDeleteVertexArrays_(1, &g_vao);
    if (g_fontTexture) glDeleteTextures_(1, &g_fontTexture);
    if (g_program) glDeleteProgram_(g_program);
    g_vbo = g_ebo = g_vao = g_fontTexture = g_program = 0;
}

void ImGui_ImplOpenGL3_NewFrame()
{
    // Nothing to do per-frame; font atlas is uploaded once at Init.
}

void ImGui_ImplOpenGL3_RenderDrawData(ImDrawData* draw_data)
{
    if (!draw_data || draw_data->CmdListsCount == 0)
        return;

    int fbWidth = static_cast<int>(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
    int fbHeight = static_cast<int>(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
    if (fbWidth <= 0 || fbHeight <= 0)
        return;

    glEnable_(GL_BLEND);
    glBlendEquation_(GL_FUNC_ADD);
    glBlendFunc_(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable_(GL_CULL_FACE);
    glDisable_(GL_DEPTH_TEST);
    glEnable_(GL_SCISSOR_TEST);
    glViewport_(0, 0, fbWidth, fbHeight);

    float L = draw_data->DisplayPos.x;
    float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
    float T = draw_data->DisplayPos.y;
    float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
    float ortho[16] = {
        2.0f / (R - L), 0, 0, 0,
        0, 2.0f / (T - B), 0, 0,
        0, 0, -1.0f, 0,
        (R + L) / (L - R), (T + B) / (B - T), 0, 1.0f,
    };

    glUseProgram_(g_program);
    glUniform1i_(g_uniformTex, 0);
    glUniformMatrix4fv_(g_uniformProjMtx, 1, GL_FALSE, ortho);
    glActiveTexture_(GL_TEXTURE0);
    // The 3D scene leaves a mipmapped sampler object bound to unit 0
    // (terrain array sampler); a bound sampler object overrides a texture's
    // own parameters, and our single-level font atlas has no mips, so under
    // that sampler it was "incomplete" and sampled as flat opaque white —
    // every glyph rendered as a solid block instead of its real coverage.
    // Detach it so the font texture's own (non-mipmapped) parameters apply.
    glBindSampler_(0, 0);
    glBindVertexArray_(g_vao);

    ImVec2 clipOff = draw_data->DisplayPos;
    ImVec2 clipScale = draw_data->FramebufferScale;

    for (int n = 0; n < draw_data->CmdListsCount; ++n)
    {
        const ImDrawList* cmdList = draw_data->CmdLists[n];

        // Always orphan+respecify (glBufferData, never SubData) even when the
        // new data fits the existing allocation: within one frame every
        // ImDrawList shares this single VBO/EBO, and SubData into a buffer a
        // prior cmdList's draw call may still be reading (GPU work is async)
        // corrupts that in-flight draw. BufferData with STREAM_DRAW lets the
        // driver rename/orphan the storage instead of racing it.
        const std::size_t vtxBytes = static_cast<std::size_t>(cmdList->VtxBuffer.Size) * sizeof(ImDrawVert);
        const std::size_t idxBytes = static_cast<std::size_t>(cmdList->IdxBuffer.Size) * sizeof(ImDrawIdx);
        glBindBuffer_(GL_ARRAY_BUFFER, g_vbo);
        glBufferData_(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vtxBytes), cmdList->VtxBuffer.Data, GL_STREAM_DRAW);
        glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, g_ebo);
        glBufferData_(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(idxBytes), cmdList->IdxBuffer.Data, GL_STREAM_DRAW);

        for (int cmdIdx = 0; cmdIdx < cmdList->CmdBuffer.Size; ++cmdIdx)
        {
            const ImDrawCmd& cmd = cmdList->CmdBuffer[cmdIdx];
            if (cmd.UserCallback)
            {
                cmd.UserCallback(cmdList, &cmd);
                continue;
            }

            ImVec2 clipMin((cmd.ClipRect.x - clipOff.x) * clipScale.x, (cmd.ClipRect.y - clipOff.y) * clipScale.y);
            ImVec2 clipMax((cmd.ClipRect.z - clipOff.x) * clipScale.x, (cmd.ClipRect.w - clipOff.y) * clipScale.y);
            if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y)
                continue;

            glScissor_(static_cast<GLint>(clipMin.x), static_cast<GLint>(fbHeight - clipMax.y),
                static_cast<GLsizei>(clipMax.x - clipMin.x), static_cast<GLsizei>(clipMax.y - clipMin.y));

            GLuint tex = static_cast<GLuint>(static_cast<std::uintptr_t>(cmd.GetTexID()));
            glBindTexture_(GL_TEXTURE_2D, tex ? tex : g_fontTexture);
            glUniform1i_(g_uniformUseTexColor, (tex && tex != g_fontTexture) ? 1 : 0);

            const void* idxOffset = reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(cmd.IdxOffset * sizeof(ImDrawIdx)));
            glDrawElements_(GL_TRIANGLES, static_cast<GLsizei>(cmd.ElemCount),
                sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, idxOffset);
        }
    }

    glDisable_(GL_SCISSOR_TEST);
    glBindVertexArray_(0);
    glUseProgram_(0);
}
