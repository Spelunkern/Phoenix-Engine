#pragma once

// Hand-written GL 4.5 core + GL_ARB_compute_shader/DSA function loader.
// No GLAD/GLEW available offline; entry points are resolved at runtime via
// wglGetProcAddress (falls back to GetProcAddress("opengl32.dll") for the
// pre-1.1 functions wglGetProcAddress is not guaranteed to return).

#include <cstdint>

#if defined(_WIN32)
#define APIENTRYGL __stdcall
#else
#define APIENTRYGL
#endif

typedef char GLchar;
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
#if defined(_WIN64)
typedef long long int GLsizeiptr;
typedef long long int GLintptr;
#else
typedef long int GLsizeiptr;
typedef long int GLintptr;
#endif
typedef char GLcharARB;
typedef unsigned short GLhalf;
typedef uint64_t GLuint64;
typedef int64_t GLint64;
typedef void (APIENTRYGL *PFNGLDRAWBUFFERPROC)(GLenum mode);
typedef void (APIENTRYGL *PFNGLREADBUFFERPROC)(GLenum mode);

// --- Core GL 1.0-1.5 constants ---
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TRIANGLES 0x0004
#define GL_LINES 0x0001
#define GL_DEPTH_TEST 0x0B71
#define GL_LEQUAL 0x0203
#define GL_LESS 0x0201
#define GL_CULL_FACE 0x0B44
#define GL_BACK 0x0405
#define GL_FRONT 0x0404
#define GL_CCW 0x0901
#define GL_CW 0x0900
#define GL_BLEND 0x0BE2
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_ONE 1
#define GL_ZERO 0
#define GL_FUNC_ADD 0x8006
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_BYTE 0x1400
#define GL_SHORT 0x1402
#define GL_INT 0x1404
#define GL_RGBA 0x1908
#define GL_RGB 0x1907
#define GL_RGBA8 0x8058
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_NEAREST 0x2600
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5

// --- Shaders / programs ---
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPUTE_SHADER 0x91B9
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84

// --- Buffers ---
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#define GL_DRAW_INDIRECT_BUFFER 0x8F3F
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_DRAW 0x88E0
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_PERSISTENT_BIT 0x0040
#define GL_MAP_COHERENT_BIT 0x0080
#define GL_DYNAMIC_STORAGE_BIT 0x0100
#define GL_CLIENT_STORAGE_BIT 0x0200

// --- Framebuffers ---
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_DEPTH_COMPONENT 0x1902
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_NONE 0

// --- Compressed textures (S3TC / DXT via GL_EXT_texture_compression_s3tc) ---
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3

// --- Queries ---
#define GL_TIME_ELAPSED 0x88BF
#define GL_TIMESTAMP 0x8E28
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_PRIMITIVES_GENERATED 0x8C87
#define GL_FRAGMENT_SHADER_INVOCATIONS 0x82F4
#define GL_VERTICES_SUBMITTED 0x82EE

// --- Debug output ---
#define GL_DEBUG_OUTPUT 0x92E0
#define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x8242
#define GL_DEBUG_SEVERITY_HIGH 0x9146
#define GL_DEBUG_SEVERITY_MEDIUM 0x9147
#define GL_DEBUG_SEVERITY_LOW 0x9148
#define GL_DEBUG_SEVERITY_NOTIFICATION 0x826B

// --- Misc ---
#define GL_MAX_ARRAY_TEXTURE_LAYERS 0x88FF
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_SHADER_STORAGE_BARRIER_BIT 0x2000
#define GL_COMMAND_BARRIER_BIT 0x0040
#define GL_ALL_BARRIER_BITS 0xFFFFFFFF
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_MULTISAMPLE 0x809D
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_SCISSOR_TEST 0x0C11
#define GL_NO_ERROR 0
#define GL_SAMPLER_2D_ARRAY 0x8DC1

namespace phoenix::gl
{
    bool load(void* (*getProcAddress)(const char*));
    bool is_loaded();
}

// --- Function pointer typedefs + extern globals ---
// Only the entry points this renderer actually calls are declared, matching
// the codebase's terse style (no full gl.h reproduction).

typedef void (APIENTRYGL *PFNGLGENBUFFERSPROC)(GLsizei n, GLuint* buffers);
typedef void (APIENTRYGL *PFNGLDELETEBUFFERSPROC)(GLsizei n, const GLuint* buffers);
typedef void (APIENTRYGL *PFNGLBINDBUFFERPROC)(GLenum target, GLuint buffer);
typedef void (APIENTRYGL *PFNGLBINDBUFFERBASEPROC)(GLenum target, GLuint index, GLuint buffer);
typedef void (APIENTRYGL *PFNGLBUFFERDATAPROC)(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
typedef void (APIENTRYGL *PFNGLBUFFERSUBDATAPROC)(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
typedef void* (APIENTRYGL *PFNGLMAPBUFFERPROC)(GLenum target, GLenum access);
typedef void* (APIENTRYGL *PFNGLMAPBUFFERRANGEPROC)(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
typedef GLboolean (APIENTRYGL *PFNGLUNMAPBUFFERPROC)(GLenum target);
typedef void (APIENTRYGL *PFNGLCREATEBUFFERSPROC)(GLsizei n, GLuint* buffers);
typedef void (APIENTRYGL *PFNGLNAMEDBUFFERSTORAGEPROC)(GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags);
typedef void (APIENTRYGL *PFNGLNAMEDBUFFERDATAPROC)(GLuint buffer, GLsizeiptr size, const void* data, GLenum usage);
typedef void (APIENTRYGL *PFNGLNAMEDBUFFERSUBDATAPROC)(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data);
typedef void* (APIENTRYGL *PFNGLMAPNAMEDBUFFERPROC)(GLuint buffer, GLenum access);
typedef void* (APIENTRYGL *PFNGLMAPNAMEDBUFFERRANGEPROC)(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access);
typedef GLboolean (APIENTRYGL *PFNGLUNMAPNAMEDBUFFERPROC)(GLuint buffer);

typedef void (APIENTRYGL *PFNGLGENVERTEXARRAYSPROC)(GLsizei n, GLuint* arrays);
typedef void (APIENTRYGL *PFNGLDELETEVERTEXARRAYSPROC)(GLsizei n, const GLuint* arrays);
typedef void (APIENTRYGL *PFNGLBINDVERTEXARRAYPROC)(GLuint array);
typedef void (APIENTRYGL *PFNGLCREATEVERTEXARRAYSPROC)(GLsizei n, GLuint* arrays);
typedef void (APIENTRYGL *PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint index);
typedef void (APIENTRYGL *PFNGLVERTEXATTRIBPOINTERPROC)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
typedef void (APIENTRYGL *PFNGLVERTEXATTRIBIPOINTERPROC)(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer);
typedef void (APIENTRYGL *PFNGLVERTEXATTRIBDIVISORPROC)(GLuint index, GLuint divisor);

typedef GLuint (APIENTRYGL *PFNGLCREATESHADERPROC)(GLenum type);
typedef void (APIENTRYGL *PFNGLDELETESHADERPROC)(GLuint shader);
typedef void (APIENTRYGL *PFNGLSHADERSOURCEPROC)(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
typedef void (APIENTRYGL *PFNGLCOMPILESHADERPROC)(GLuint shader);
typedef void (APIENTRYGL *PFNGLGETSHADERIVPROC)(GLuint shader, GLenum pname, GLint* params);
typedef void (APIENTRYGL *PFNGLGETSHADERINFOLOGPROC)(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef GLuint (APIENTRYGL *PFNGLCREATEPROGRAMPROC)(void);
typedef void (APIENTRYGL *PFNGLDELETEPROGRAMPROC)(GLuint program);
typedef void (APIENTRYGL *PFNGLATTACHSHADERPROC)(GLuint program, GLuint shader);
typedef void (APIENTRYGL *PFNGLLINKPROGRAMPROC)(GLuint program);
typedef void (APIENTRYGL *PFNGLGETPROGRAMIVPROC)(GLuint program, GLenum pname, GLint* params);
typedef void (APIENTRYGL *PFNGLGETPROGRAMINFOLOGPROC)(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef void (APIENTRYGL *PFNGLUSEPROGRAMPROC)(GLuint program);
typedef GLint (APIENTRYGL *PFNGLGETUNIFORMLOCATIONPROC)(GLuint program, const GLchar* name);
typedef void (APIENTRYGL *PFNGLUNIFORM1IPROC)(GLint location, GLint v0);
typedef void (APIENTRYGL *PFNGLUNIFORM1FPROC)(GLint location, GLfloat v0);
typedef void (APIENTRYGL *PFNGLUNIFORM1FVPROC)(GLint location, GLsizei count, const GLfloat* value);
typedef void (APIENTRYGL *PFNGLUNIFORM4FVPROC)(GLint location, GLsizei count, const GLfloat* value);
typedef void (APIENTRYGL *PFNGLUNIFORMMATRIX4FVPROC)(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
typedef GLuint (APIENTRYGL *PFNGLGETUNIFORMBLOCKINDEXPROC)(GLuint program, const GLchar* uniformBlockName);
typedef void (APIENTRYGL *PFNGLUNIFORMBLOCKBINDINGPROC)(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding);
typedef void (APIENTRYGL *PFNGLSHADERSTORAGEBLOCKBINDINGPROC)(GLuint program, GLuint storageBlockIndex, GLuint storageBlockBinding);
typedef GLuint (APIENTRYGL *PFNGLGETPROGRAMRESOURCEINDEXPROC)(GLuint program, GLenum programInterface, const GLchar* name);

typedef void (APIENTRYGL *PFNGLGENTEXTURESPROC)(GLsizei n, GLuint* textures);
typedef void (APIENTRYGL *PFNGLDELETETEXTURESPROC)(GLsizei n, const GLuint* textures);
typedef void (APIENTRYGL *PFNGLBINDTEXTUREPROC)(GLenum target, GLuint texture);
typedef void (APIENTRYGL *PFNGLACTIVETEXTUREPROC)(GLenum texture);
typedef void (APIENTRYGL *PFNGLTEXPARAMETERIPROC)(GLenum target, GLenum pname, GLint param);
typedef void (APIENTRYGL *PFNGLTEXPARAMETERFPROC)(GLenum target, GLenum pname, GLfloat param);
typedef void (APIENTRYGL *PFNGLTEXIMAGE2DPROC)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels);
typedef void (APIENTRYGL *PFNGLTEXSTORAGE3DPROC)(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth);
typedef void (APIENTRYGL *PFNGLTEXSUBIMAGE3DPROC)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels);
typedef void (APIENTRYGL *PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data);
typedef void (APIENTRYGL *PFNGLGENERATEMIPMAPPROC)(GLenum target);
typedef void (APIENTRYGL *PFNGLCREATETEXTURESPROC)(GLenum target, GLsizei n, GLuint* textures);
typedef void (APIENTRYGL *PFNGLTEXTURESTORAGE3DPROC)(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth);
typedef void (APIENTRYGL *PFNGLTEXTURESUBIMAGE3DPROC)(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels);
typedef void (APIENTRYGL *PFNGLCOMPRESSEDTEXTURESUBIMAGE3DPROC)(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data);
typedef void (APIENTRYGL *PFNGLTEXTUREPARAMETERIPROC)(GLuint texture, GLenum pname, GLint param);
typedef void (APIENTRYGL *PFNGLTEXTUREPARAMETERFPROC)(GLuint texture, GLenum pname, GLfloat param);
typedef void (APIENTRYGL *PFNGLGENERATETEXTUREMIPMAPPROC)(GLuint texture);
typedef void (APIENTRYGL *PFNGLBINDTEXTUREUNITPROC)(GLuint unit, GLuint texture);
typedef GLuint64 (APIENTRYGL *PFNGLGETTEXTUREHANDLEARBPROC)(GLuint texture);

typedef void (APIENTRYGL *PFNGLGENFRAMEBUFFERSPROC)(GLsizei n, GLuint* framebuffers);
typedef void (APIENTRYGL *PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei n, const GLuint* framebuffers);
typedef void (APIENTRYGL *PFNGLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
typedef void (APIENTRYGL *PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef GLenum (APIENTRYGL *PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum target);

typedef void (APIENTRYGL *PFNGLDRAWELEMENTSPROC)(GLenum mode, GLsizei count, GLenum type, const void* indices);
typedef void (APIENTRYGL *PFNGLDRAWELEMENTSINSTANCEDPROC)(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount);
typedef void (APIENTRYGL *PFNGLDRAWELEMENTSINSTANCEDBASEINSTANCEPROC)(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount, GLuint baseinstance);
typedef void (APIENTRYGL *PFNGLMULTIDRAWELEMENTSINDIRECTPROC)(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride);
typedef void (APIENTRYGL *PFNGLDRAWELEMENTSINDIRECTPROC)(GLenum mode, GLenum type, const void* indirect);
typedef void (APIENTRYGL *PFNGLDRAWARRAYSPROC)(GLenum mode, GLint first, GLsizei count);
typedef void (APIENTRYGL *PFNGLDRAWARRAYSINSTANCEDPROC)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
typedef void (APIENTRYGL *PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount, GLuint baseinstance);
typedef void (APIENTRYGL *PFNGLBLITFRAMEBUFFERPROC)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);

typedef void (APIENTRYGL *PFNGLDISPATCHCOMPUTEPROC)(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z);
typedef void (APIENTRYGL *PFNGLMEMORYBARRIERPROC)(GLbitfield barriers);

typedef void (APIENTRYGL *PFNGLGENQUERIESPROC)(GLsizei n, GLuint* ids);
typedef void (APIENTRYGL *PFNGLDELETEQUERIESPROC)(GLsizei n, const GLuint* ids);
typedef void (APIENTRYGL *PFNGLBEGINQUERYPROC)(GLenum target, GLuint id);
typedef void (APIENTRYGL *PFNGLENDQUERYPROC)(GLenum target);
typedef void (APIENTRYGL *PFNGLQUERYCOUNTERPROC)(GLuint id, GLenum target);
typedef void (APIENTRYGL *PFNGLGETQUERYOBJECTUI64VPROC)(GLuint id, GLenum pname, GLuint64* params);
typedef void (APIENTRYGL *PFNGLGETQUERYOBJECTIVPROC)(GLuint id, GLenum pname, GLint* params);

typedef void (APIENTRYGL *PFNGLDEBUGMESSAGECALLBACKPROC)(void* callback, const void* userParam);
typedef void (APIENTRYGL *PFNGLOBJECTLABELPROC)(GLenum identifier, GLuint name, GLsizei length, const GLchar* label);

typedef void (APIENTRYGL *PFNGLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
typedef void (APIENTRYGL *PFNGLSCISSORPROC)(GLint x, GLint y, GLsizei width, GLsizei height);

typedef void (APIENTRYGL *PFNGLENABLEPROC)(GLenum cap);
typedef void (APIENTRYGL *PFNGLDISABLEPROC)(GLenum cap);
typedef void (APIENTRYGL *PFNGLDEPTHFUNCPROC)(GLenum func);
typedef void (APIENTRYGL *PFNGLDEPTHMASKPROC)(GLboolean flag);
typedef void (APIENTRYGL *PFNGLBLENDFUNCPROC)(GLenum sfactor, GLenum dfactor);
typedef void (APIENTRYGL *PFNGLBLENDFUNCSEPARATEPROC)(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
typedef void (APIENTRYGL *PFNGLBLENDEQUATIONPROC)(GLenum mode);
typedef void (APIENTRYGL *PFNGLCLEARPROC)(GLbitfield mask);
typedef void (APIENTRYGL *PFNGLCLEARCOLORPROC)(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
typedef void (APIENTRYGL *PFNGLCLEARDEPTHPROC)(GLdouble depth);
typedef const GLubyte* (APIENTRYGL *PFNGLGETSTRINGPROC)(GLenum name);
typedef GLenum (APIENTRYGL *PFNGLGETERRORPROC)(void);
typedef void (APIENTRYGL *PFNGLCULLFACEPROC)(GLenum mode);
typedef void (APIENTRYGL *PFNGLFRONTFACEPROC)(GLenum mode);
typedef void (APIENTRYGL *PFNGLFLUSHPROC)(void);
typedef void (APIENTRYGL *PFNGLFINISHPROC)(void);
typedef void (APIENTRYGL *PFNGLPIXELSTOREIPROC)(GLenum pname, GLint param);
typedef void (APIENTRYGL *PFNGLGETINTEGERVPROC)(GLenum pname, GLint* data);
typedef void (APIENTRYGL *PFNGLGETFLOATVPROC)(GLenum pname, GLfloat* data);

typedef void (APIENTRYGL *PFNGLCREATESAMPLERSPROC)(GLsizei n, GLuint* samplers);
typedef void (APIENTRYGL *PFNGLDELETESAMPLERSPROC)(GLsizei n, const GLuint* samplers);
typedef void (APIENTRYGL *PFNGLBINDSAMPLERPROC)(GLuint unit, GLuint sampler);
typedef void (APIENTRYGL *PFNGLSAMPLERPARAMETERIPROC)(GLuint sampler, GLenum pname, GLint param);
typedef void (APIENTRYGL *PFNGLSAMPLERPARAMETERFPROC)(GLuint sampler, GLenum pname, GLfloat param);

extern PFNGLGENBUFFERSPROC glGenBuffers_;
extern PFNGLDELETEBUFFERSPROC glDeleteBuffers_;
extern PFNGLBINDBUFFERPROC glBindBuffer_;
extern PFNGLBINDBUFFERBASEPROC glBindBufferBase_;
extern PFNGLBUFFERDATAPROC glBufferData_;
extern PFNGLBUFFERSUBDATAPROC glBufferSubData_;
extern PFNGLMAPBUFFERPROC glMapBuffer_;
extern PFNGLMAPBUFFERRANGEPROC glMapBufferRange_;
extern PFNGLUNMAPBUFFERPROC glUnmapBuffer_;
extern PFNGLCREATEBUFFERSPROC glCreateBuffers_;
extern PFNGLNAMEDBUFFERSTORAGEPROC glNamedBufferStorage_;
extern PFNGLNAMEDBUFFERDATAPROC glNamedBufferData_;
extern PFNGLNAMEDBUFFERSUBDATAPROC glNamedBufferSubData_;
extern PFNGLMAPNAMEDBUFFERPROC glMapNamedBuffer_;
extern PFNGLMAPNAMEDBUFFERRANGEPROC glMapNamedBufferRange_;
extern PFNGLUNMAPNAMEDBUFFERPROC glUnmapNamedBuffer_;

extern PFNGLGENVERTEXARRAYSPROC glGenVertexArrays_;
extern PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays_;
extern PFNGLBINDVERTEXARRAYPROC glBindVertexArray_;
extern PFNGLCREATEVERTEXARRAYSPROC glCreateVertexArrays_;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray_;
extern PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer_;
extern PFNGLVERTEXATTRIBIPOINTERPROC glVertexAttribIPointer_;
extern PFNGLVERTEXATTRIBDIVISORPROC glVertexAttribDivisor_;

extern PFNGLCREATESHADERPROC glCreateShader_;
extern PFNGLDELETESHADERPROC glDeleteShader_;
extern PFNGLSHADERSOURCEPROC glShaderSource_;
extern PFNGLCOMPILESHADERPROC glCompileShader_;
extern PFNGLGETSHADERIVPROC glGetShaderiv_;
extern PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog_;
extern PFNGLCREATEPROGRAMPROC glCreateProgram_;
extern PFNGLDELETEPROGRAMPROC glDeleteProgram_;
extern PFNGLATTACHSHADERPROC glAttachShader_;
extern PFNGLLINKPROGRAMPROC glLinkProgram_;
extern PFNGLGETPROGRAMIVPROC glGetProgramiv_;
extern PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog_;
extern PFNGLUSEPROGRAMPROC glUseProgram_;
extern PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation_;
extern PFNGLUNIFORM1IPROC glUniform1i_;
extern PFNGLUNIFORM1FPROC glUniform1f_;
extern PFNGLUNIFORM1FVPROC glUniform1fv_;
extern PFNGLUNIFORM4FVPROC glUniform4fv_;
extern PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv_;
extern PFNGLGETUNIFORMBLOCKINDEXPROC glGetUniformBlockIndex_;
extern PFNGLUNIFORMBLOCKBINDINGPROC glUniformBlockBinding_;
extern PFNGLSHADERSTORAGEBLOCKBINDINGPROC glShaderStorageBlockBinding_;
extern PFNGLGETPROGRAMRESOURCEINDEXPROC glGetProgramResourceIndex_;

extern PFNGLGENTEXTURESPROC glGenTextures_;
extern PFNGLDELETETEXTURESPROC glDeleteTextures_;
extern PFNGLBINDTEXTUREPROC glBindTexture_;
extern PFNGLACTIVETEXTUREPROC glActiveTexture_;
extern PFNGLTEXPARAMETERIPROC glTexParameteri_;
extern PFNGLTEXPARAMETERFPROC glTexParameterf_;
extern PFNGLTEXIMAGE2DPROC glTexImage2D_;
extern PFNGLTEXSTORAGE3DPROC glTexStorage3D_;
extern PFNGLTEXSUBIMAGE3DPROC glTexSubImage3D_;
extern PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC glCompressedTexSubImage3D_;
extern PFNGLGENERATEMIPMAPPROC glGenerateMipmap_;
extern PFNGLCREATETEXTURESPROC glCreateTextures_;
extern PFNGLTEXTURESTORAGE3DPROC glTextureStorage3D_;
extern PFNGLTEXTURESUBIMAGE3DPROC glTextureSubImage3D_;
extern PFNGLCOMPRESSEDTEXTURESUBIMAGE3DPROC glCompressedTextureSubImage3D_;
extern PFNGLTEXTUREPARAMETERIPROC glTextureParameteri_;
extern PFNGLTEXTUREPARAMETERFPROC glTextureParameterf_;
extern PFNGLGENERATETEXTUREMIPMAPPROC glGenerateTextureMipmap_;
extern PFNGLBINDTEXTUREUNITPROC glBindTextureUnit_;

extern PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers_;
extern PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers_;
extern PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer_;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D_;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus_;
extern PFNGLDRAWBUFFERPROC glDrawBuffer_;
extern PFNGLREADBUFFERPROC glReadBuffer_;

extern PFNGLDRAWELEMENTSPROC glDrawElements_;
extern PFNGLDRAWELEMENTSINSTANCEDPROC glDrawElementsInstanced_;
extern PFNGLDRAWELEMENTSINSTANCEDBASEINSTANCEPROC glDrawElementsInstancedBaseInstance_;
extern PFNGLMULTIDRAWELEMENTSINDIRECTPROC glMultiDrawElementsIndirect_;
extern PFNGLDRAWELEMENTSINDIRECTPROC glDrawElementsIndirect_;
extern PFNGLDRAWARRAYSPROC glDrawArrays_;
extern PFNGLDRAWARRAYSINSTANCEDPROC glDrawArraysInstanced_;
extern PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC glDrawArraysInstancedBaseInstance_;
extern PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer_;

extern PFNGLDISPATCHCOMPUTEPROC glDispatchCompute_;
extern PFNGLMEMORYBARRIERPROC glMemoryBarrier_;

extern PFNGLGENQUERIESPROC glGenQueries_;
extern PFNGLDELETEQUERIESPROC glDeleteQueries_;
extern PFNGLBEGINQUERYPROC glBeginQuery_;
extern PFNGLENDQUERYPROC glEndQuery_;
extern PFNGLQUERYCOUNTERPROC glQueryCounter_;
extern PFNGLGETQUERYOBJECTUI64VPROC glGetQueryObjectui64v_;
extern PFNGLGETQUERYOBJECTIVPROC glGetQueryObjectiv_;

extern PFNGLDEBUGMESSAGECALLBACKPROC glDebugMessageCallback_;
extern PFNGLOBJECTLABELPROC glObjectLabel_;

extern PFNGLVIEWPORTPROC glViewport_;
extern PFNGLSCISSORPROC glScissor_;
extern PFNGLENABLEPROC glEnable_;
extern PFNGLDISABLEPROC glDisable_;
extern PFNGLDEPTHFUNCPROC glDepthFunc_;
extern PFNGLDEPTHMASKPROC glDepthMask_;
extern PFNGLBLENDFUNCPROC glBlendFunc_;
extern PFNGLBLENDFUNCSEPARATEPROC glBlendFuncSeparate_;
extern PFNGLBLENDEQUATIONPROC glBlendEquation_;
extern PFNGLCLEARPROC glClear_;
extern PFNGLCLEARCOLORPROC glClearColor_;
extern PFNGLCLEARDEPTHPROC glClearDepth_;
extern PFNGLGETSTRINGPROC glGetString_;
extern PFNGLGETERRORPROC glGetError_;
extern PFNGLCULLFACEPROC glCullFace_;
extern PFNGLFRONTFACEPROC glFrontFace_;
extern PFNGLFLUSHPROC glFlush_;
extern PFNGLFINISHPROC glFinish_;
extern PFNGLPIXELSTOREIPROC glPixelStorei_;
extern PFNGLGETINTEGERVPROC glGetIntegerv_;
extern PFNGLGETFLOATVPROC glGetFloatv_;

extern PFNGLCREATESAMPLERSPROC glCreateSamplers_;
extern PFNGLDELETESAMPLERSPROC glDeleteSamplers_;
extern PFNGLBINDSAMPLERPROC glBindSampler_;
extern PFNGLSAMPLERPARAMETERIPROC glSamplerParameteri_;
extern PFNGLSAMPLERPARAMETERFPROC glSamplerParameterf_;
