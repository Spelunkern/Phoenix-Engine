#include "renderer/gl/gl_loader.h"

#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

PFNGLGENBUFFERSPROC glGenBuffers_;
PFNGLDELETEBUFFERSPROC glDeleteBuffers_;
PFNGLBINDBUFFERPROC glBindBuffer_;
PFNGLBINDBUFFERBASEPROC glBindBufferBase_;
PFNGLBUFFERDATAPROC glBufferData_;
PFNGLBUFFERSUBDATAPROC glBufferSubData_;
PFNGLMAPBUFFERPROC glMapBuffer_;
PFNGLMAPBUFFERRANGEPROC glMapBufferRange_;
PFNGLUNMAPBUFFERPROC glUnmapBuffer_;
PFNGLCREATEBUFFERSPROC glCreateBuffers_;
PFNGLNAMEDBUFFERSTORAGEPROC glNamedBufferStorage_;
PFNGLNAMEDBUFFERDATAPROC glNamedBufferData_;
PFNGLNAMEDBUFFERSUBDATAPROC glNamedBufferSubData_;
PFNGLMAPNAMEDBUFFERPROC glMapNamedBuffer_;
PFNGLMAPNAMEDBUFFERRANGEPROC glMapNamedBufferRange_;
PFNGLUNMAPNAMEDBUFFERPROC glUnmapNamedBuffer_;

PFNGLGENVERTEXARRAYSPROC glGenVertexArrays_;
PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays_;
PFNGLBINDVERTEXARRAYPROC glBindVertexArray_;
PFNGLCREATEVERTEXARRAYSPROC glCreateVertexArrays_;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray_;
PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer_;
PFNGLVERTEXATTRIBIPOINTERPROC glVertexAttribIPointer_;
PFNGLVERTEXATTRIBDIVISORPROC glVertexAttribDivisor_;

PFNGLCREATESHADERPROC glCreateShader_;
PFNGLDELETESHADERPROC glDeleteShader_;
PFNGLSHADERSOURCEPROC glShaderSource_;
PFNGLCOMPILESHADERPROC glCompileShader_;
PFNGLGETSHADERIVPROC glGetShaderiv_;
PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog_;
PFNGLCREATEPROGRAMPROC glCreateProgram_;
PFNGLDELETEPROGRAMPROC glDeleteProgram_;
PFNGLATTACHSHADERPROC glAttachShader_;
PFNGLLINKPROGRAMPROC glLinkProgram_;
PFNGLGETPROGRAMIVPROC glGetProgramiv_;
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog_;
PFNGLUSEPROGRAMPROC glUseProgram_;
PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation_;
PFNGLUNIFORM1IPROC glUniform1i_;
PFNGLUNIFORM1FPROC glUniform1f_;
PFNGLUNIFORM1FVPROC glUniform1fv_;
PFNGLUNIFORM4FVPROC glUniform4fv_;
PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv_;
PFNGLGETUNIFORMBLOCKINDEXPROC glGetUniformBlockIndex_;
PFNGLUNIFORMBLOCKBINDINGPROC glUniformBlockBinding_;
PFNGLSHADERSTORAGEBLOCKBINDINGPROC glShaderStorageBlockBinding_;
PFNGLGETPROGRAMRESOURCEINDEXPROC glGetProgramResourceIndex_;

PFNGLGENTEXTURESPROC glGenTextures_;
PFNGLDELETETEXTURESPROC glDeleteTextures_;
PFNGLBINDTEXTUREPROC glBindTexture_;
PFNGLACTIVETEXTUREPROC glActiveTexture_;
PFNGLTEXPARAMETERIPROC glTexParameteri_;
PFNGLTEXPARAMETERFPROC glTexParameterf_;
PFNGLTEXIMAGE2DPROC glTexImage2D_;
PFNGLTEXSTORAGE3DPROC glTexStorage3D_;
PFNGLTEXSUBIMAGE3DPROC glTexSubImage3D_;
PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC glCompressedTexSubImage3D_;
PFNGLGENERATEMIPMAPPROC glGenerateMipmap_;
PFNGLCREATETEXTURESPROC glCreateTextures_;
PFNGLTEXTURESTORAGE3DPROC glTextureStorage3D_;
PFNGLTEXTURESUBIMAGE3DPROC glTextureSubImage3D_;
PFNGLCOMPRESSEDTEXTURESUBIMAGE3DPROC glCompressedTextureSubImage3D_;
PFNGLTEXTUREPARAMETERIPROC glTextureParameteri_;
PFNGLTEXTUREPARAMETERFPROC glTextureParameterf_;
PFNGLGENERATETEXTUREMIPMAPPROC glGenerateTextureMipmap_;
PFNGLBINDTEXTUREUNITPROC glBindTextureUnit_;

PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers_;
PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers_;
PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer_;
PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D_;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus_;
PFNGLDRAWBUFFERPROC glDrawBuffer_;
PFNGLREADBUFFERPROC glReadBuffer_;

PFNGLDRAWELEMENTSPROC glDrawElements_;
PFNGLDRAWELEMENTSINSTANCEDPROC glDrawElementsInstanced_;
PFNGLDRAWELEMENTSINSTANCEDBASEINSTANCEPROC glDrawElementsInstancedBaseInstance_;
PFNGLMULTIDRAWELEMENTSINDIRECTPROC glMultiDrawElementsIndirect_;
PFNGLDRAWELEMENTSINDIRECTPROC glDrawElementsIndirect_;
PFNGLDRAWARRAYSPROC glDrawArrays_;
PFNGLDRAWARRAYSINSTANCEDPROC glDrawArraysInstanced_;
PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC glDrawArraysInstancedBaseInstance_;
PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer_;

PFNGLDISPATCHCOMPUTEPROC glDispatchCompute_;
PFNGLMEMORYBARRIERPROC glMemoryBarrier_;

PFNGLGENQUERIESPROC glGenQueries_;
PFNGLDELETEQUERIESPROC glDeleteQueries_;
PFNGLBEGINQUERYPROC glBeginQuery_;
PFNGLENDQUERYPROC glEndQuery_;
PFNGLQUERYCOUNTERPROC glQueryCounter_;
PFNGLGETQUERYOBJECTUI64VPROC glGetQueryObjectui64v_;
PFNGLGETQUERYOBJECTIVPROC glGetQueryObjectiv_;

PFNGLDEBUGMESSAGECALLBACKPROC glDebugMessageCallback_;
PFNGLOBJECTLABELPROC glObjectLabel_;

PFNGLVIEWPORTPROC glViewport_;
PFNGLSCISSORPROC glScissor_;
PFNGLENABLEPROC glEnable_;
PFNGLDISABLEPROC glDisable_;
PFNGLDEPTHFUNCPROC glDepthFunc_;
PFNGLDEPTHMASKPROC glDepthMask_;
PFNGLBLENDFUNCPROC glBlendFunc_;
PFNGLBLENDFUNCSEPARATEPROC glBlendFuncSeparate_;
PFNGLBLENDEQUATIONPROC glBlendEquation_;
PFNGLCLEARPROC glClear_;
PFNGLCLEARCOLORPROC glClearColor_;
PFNGLCLEARDEPTHPROC glClearDepth_;
PFNGLGETSTRINGPROC glGetString_;
PFNGLGETERRORPROC glGetError_;
PFNGLCULLFACEPROC glCullFace_;
PFNGLFRONTFACEPROC glFrontFace_;
PFNGLFLUSHPROC glFlush_;
PFNGLFINISHPROC glFinish_;
PFNGLPIXELSTOREIPROC glPixelStorei_;
PFNGLGETINTEGERVPROC glGetIntegerv_;
PFNGLGETFLOATVPROC glGetFloatv_;

PFNGLCREATESAMPLERSPROC glCreateSamplers_;
PFNGLDELETESAMPLERSPROC glDeleteSamplers_;
PFNGLBINDSAMPLERPROC glBindSampler_;
PFNGLSAMPLERPARAMETERIPROC glSamplerParameteri_;
PFNGLSAMPLERPARAMETERFPROC glSamplerParameterf_;

namespace phoenix::gl
{
    namespace
    {
        bool loaded_ = false;

        template <typename T>
        bool fetch(void* (*getProcAddress)(const char*), const char* name, T& out)
        {
            void* p = getProcAddress(name);
#if defined(_WIN32)
            // wglGetProcAddress is only guaranteed for GL >1.1 entry points;
            // core GL1.0/1.1 functions (glGenTextures, glDrawElements, ...)
            // are direct opengl32.dll exports and must be resolved that way.
            if (!p)
            {
                static HMODULE gl32 = ::GetModuleHandleA("opengl32.dll");
                if (gl32)
                    p = reinterpret_cast<void*>(::GetProcAddress(gl32, name));
            }
#endif
            out = reinterpret_cast<T>(p);
            return out != nullptr;
        }
    }

    bool load(void* (*getProcAddress)(const char*))
    {
        bool ok = true;
#define GLLOAD(name) ok = fetch(getProcAddress, #name, name##_) && ok

        GLLOAD(glGenBuffers);
        GLLOAD(glDeleteBuffers);
        GLLOAD(glBindBuffer);
        GLLOAD(glBindBufferBase);
        GLLOAD(glBufferData);
        GLLOAD(glBufferSubData);
        GLLOAD(glMapBuffer);
        GLLOAD(glMapBufferRange);
        GLLOAD(glUnmapBuffer);
        GLLOAD(glCreateBuffers);
        GLLOAD(glNamedBufferStorage);
        GLLOAD(glNamedBufferData);
        GLLOAD(glNamedBufferSubData);
        GLLOAD(glMapNamedBuffer);
        GLLOAD(glMapNamedBufferRange);
        GLLOAD(glUnmapNamedBuffer);

        GLLOAD(glGenVertexArrays);
        GLLOAD(glDeleteVertexArrays);
        GLLOAD(glBindVertexArray);
        GLLOAD(glCreateVertexArrays);
        GLLOAD(glEnableVertexAttribArray);
        GLLOAD(glVertexAttribPointer);
        GLLOAD(glVertexAttribIPointer);
        GLLOAD(glVertexAttribDivisor);

        GLLOAD(glCreateShader);
        GLLOAD(glDeleteShader);
        GLLOAD(glShaderSource);
        GLLOAD(glCompileShader);
        GLLOAD(glGetShaderiv);
        GLLOAD(glGetShaderInfoLog);
        GLLOAD(glCreateProgram);
        GLLOAD(glDeleteProgram);
        GLLOAD(glAttachShader);
        GLLOAD(glLinkProgram);
        GLLOAD(glGetProgramiv);
        GLLOAD(glGetProgramInfoLog);
        GLLOAD(glUseProgram);
        GLLOAD(glGetUniformLocation);
        GLLOAD(glUniform1i);
        GLLOAD(glUniform1f);
        GLLOAD(glUniform1fv);
        GLLOAD(glUniform4fv);
        GLLOAD(glUniformMatrix4fv);
        GLLOAD(glGetUniformBlockIndex);
        GLLOAD(glUniformBlockBinding);
        GLLOAD(glShaderStorageBlockBinding);
        GLLOAD(glGetProgramResourceIndex);

        GLLOAD(glGenTextures);
        GLLOAD(glDeleteTextures);
        GLLOAD(glBindTexture);
        GLLOAD(glActiveTexture);
        GLLOAD(glTexParameteri);
        GLLOAD(glTexParameterf);
        GLLOAD(glTexImage2D);
        GLLOAD(glTexStorage3D);
        GLLOAD(glTexSubImage3D);
        GLLOAD(glCompressedTexSubImage3D);
        GLLOAD(glGenerateMipmap);
        GLLOAD(glCreateTextures);
        GLLOAD(glTextureStorage3D);
        GLLOAD(glTextureSubImage3D);
        GLLOAD(glCompressedTextureSubImage3D);
        GLLOAD(glTextureParameteri);
        GLLOAD(glTextureParameterf);
        GLLOAD(glGenerateTextureMipmap);
        GLLOAD(glBindTextureUnit);

        GLLOAD(glGenFramebuffers);
        GLLOAD(glDeleteFramebuffers);
        GLLOAD(glBindFramebuffer);
        GLLOAD(glFramebufferTexture2D);
        GLLOAD(glCheckFramebufferStatus);
        GLLOAD(glDrawBuffer);
        GLLOAD(glReadBuffer);

        GLLOAD(glDrawElements);
        GLLOAD(glDrawElementsInstanced);
        GLLOAD(glDrawElementsInstancedBaseInstance);
        GLLOAD(glMultiDrawElementsIndirect);
        GLLOAD(glDrawElementsIndirect);
        GLLOAD(glDrawArrays);
        GLLOAD(glDrawArraysInstanced);
        GLLOAD(glDrawArraysInstancedBaseInstance);
        GLLOAD(glBlitFramebuffer);

        GLLOAD(glDispatchCompute);
        GLLOAD(glMemoryBarrier);

        GLLOAD(glGenQueries);
        GLLOAD(glDeleteQueries);
        GLLOAD(glBeginQuery);
        GLLOAD(glEndQuery);
        GLLOAD(glQueryCounter);
        GLLOAD(glGetQueryObjectui64v);
        GLLOAD(glGetQueryObjectiv);

        GLLOAD(glDebugMessageCallback);
        GLLOAD(glObjectLabel);

        GLLOAD(glViewport);
        GLLOAD(glScissor);
        GLLOAD(glEnable);
        GLLOAD(glDisable);
        GLLOAD(glDepthFunc);
        GLLOAD(glDepthMask);
        GLLOAD(glBlendFunc);
        GLLOAD(glBlendFuncSeparate);
        GLLOAD(glBlendEquation);
        GLLOAD(glClear);
        GLLOAD(glClearColor);
        GLLOAD(glClearDepth);
        GLLOAD(glGetString);
        GLLOAD(glGetError);
        GLLOAD(glCullFace);
        GLLOAD(glFrontFace);
        GLLOAD(glFlush);
        GLLOAD(glFinish);
        GLLOAD(glPixelStorei);
        GLLOAD(glGetIntegerv);
        GLLOAD(glGetFloatv);

        GLLOAD(glCreateSamplers);
        GLLOAD(glDeleteSamplers);
        GLLOAD(glBindSampler);
        GLLOAD(glSamplerParameteri);
        GLLOAD(glSamplerParameterf);

#undef GLLOAD
        loaded_ = ok;
        return ok;
    }

    bool is_loaded() { return loaded_; }
}

// glDrawElements/glDrawArrays/glBindTexture/glTexImage2D/glTexParameteri/
// glGenTextures/glDeleteTextures/glActiveTexture technically live in
// opengl32.dll directly (GL 1.0-1.3), but wglGetProcAddress is documented to
// work for them on current Windows drivers too; the fetch() calls above are
// left as-is and this is verified at load() call time via the return value.
