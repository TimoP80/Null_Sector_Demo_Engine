// ---------------------------------------------------------------------------
// Minimal OpenGL 3.3 core function loader (via glfwGetProcAddress).
// No GLEW/glad dependency - just the functions this project uses.
// ---------------------------------------------------------------------------
#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// GL type definitions. glfw3.h does NOT provide these when GLFW_INCLUDE_NONE
// is set, so define the subset the engine uses. Each is guarded so a system
// <GL/gl.h> included through some other path cannot cause redefinitions.
// ---------------------------------------------------------------------------
#ifndef GLenum
typedef unsigned int GLenum;
#endif
#ifndef GLboolean
typedef unsigned char GLboolean;
#endif
#ifndef GLbitfield
typedef unsigned int GLbitfield;
#endif
#ifndef GLvoid
typedef void GLvoid;
#endif
#ifndef GLbyte
typedef signed char GLbyte;
#endif
#ifndef GLshort
typedef short GLshort;
#endif
#ifndef GLint
typedef int GLint;
#endif
#ifndef GLsizei
typedef int GLsizei;
#endif
#ifndef GLubyte
typedef unsigned char GLubyte;
#endif
#ifndef GLushort
typedef unsigned short GLushort;
#endif
#ifndef GLuint
typedef unsigned int GLuint;
#endif
#ifndef GLfloat
typedef float GLfloat;
#endif
#ifndef GLclampf
typedef float GLclampf;
#endif
#ifndef GLdouble
typedef double GLdouble;
#endif
#ifndef GLclampd
typedef double GLclampd;
#endif
#ifndef GLchar
typedef char GLchar;
#endif
#ifndef GLintptr
typedef std::ptrdiff_t GLintptr;
#endif
#ifndef GLsizeiptr
typedef std::ptrdiff_t GLsizeiptr;
#endif
#ifndef GLuint64
typedef std::uint64_t GLuint64;
#endif

// --- constants (core GL 3.3 values) -----------------------------------------
// glfw3.h is included with GLFW_INCLUDE_NONE (no system <GL/gl.h>), so the GL
// boolean macros below are the only GL_* identifiers that leak into the global
// namespace (GL_TRUE / GL_FALSE are used unqualified in attribute calls).
#ifndef GL_TRUE
#define GL_TRUE 1
#endif
#ifndef GL_FALSE
#define GL_FALSE 0
#endif

namespace gl {
inline constexpr GLenum COLOR_BUFFER_BIT = 0x00004000;
inline constexpr GLenum DEPTH_BUFFER_BIT = 0x00000100;
inline constexpr GLenum BLEND = 0x0BE2;
inline constexpr GLenum DEPTH_TEST = 0x0B71;
inline constexpr GLenum SCISSOR_TEST = 0x0C11;
inline constexpr GLenum CULL_FACE = 0x0B44;
inline constexpr GLenum PROGRAM_POINT_SIZE = 0x8642;
inline constexpr GLenum ONE = 1;
inline constexpr GLenum ZERO = 0;
inline constexpr GLenum SRC_ALPHA = 0x0302;
inline constexpr GLenum ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum TEXTURE_2D = 0x0DE1;
inline constexpr GLenum TEXTURE0 = 0x84C0;
inline constexpr GLenum TEXTURE1 = 0x84C1;
inline constexpr GLenum TEXTURE2 = 0x84C2;
inline constexpr GLenum TEXTURE3 = 0x84C3;
inline constexpr GLenum TEXTURE4 = 0x84C4;
inline constexpr GLenum TEXTURE5 = 0x84C5;
inline constexpr GLenum TEXTURE6 = 0x84C6;
inline constexpr GLenum TEXTURE7 = 0x84C7;
inline constexpr GLenum TEXTURE8 = 0x84C8;
inline constexpr GLenum TEXTURE9 = 0x84C9;
inline constexpr GLenum TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum NEAREST = 0x2600;
inline constexpr GLenum LINEAR = 0x2601;
inline constexpr GLenum LINEAR_MIPMAP_LINEAR = 0x2703;
inline constexpr GLenum LINEAR_MIPMAP_NEAREST = 0x2701;
inline constexpr GLenum CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum REPEAT = 0x2901;
inline constexpr GLenum RGBA = 0x1908;
inline constexpr GLenum RGBA8 = 0x8058;
inline constexpr GLenum RGBA16F = 0x881A;
inline constexpr GLenum RGB = 0x1907;
inline constexpr GLenum RGB8 = 0x8051;
inline constexpr GLenum UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum UNSIGNED_SHORT = 0x1403;
inline constexpr GLenum UNSIGNED_INT = 0x1405;
inline constexpr GLenum HALF_FLOAT = 0x140B;
inline constexpr GLenum FLOAT = 0x1406;
inline constexpr GLenum ARRAY_BUFFER = 0x8892;
inline constexpr GLenum ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr GLenum UNIFORM_BUFFER = 0x8A11;
inline constexpr GLenum STATIC_DRAW = 0x88E4;
inline constexpr GLenum DYNAMIC_DRAW = 0x88E8;
inline constexpr GLenum TRIANGLES = 0x0004;
inline constexpr GLenum POINTS = 0x0000;
inline constexpr GLenum FRAMEBUFFER = 0x8D40;
inline constexpr GLenum READ_FRAMEBUFFER = 0x8CA8;
inline constexpr GLenum DRAW_FRAMEBUFFER = 0x8CA9;
inline constexpr GLenum FRAMEBUFFER_BINDING = 0x8CA6;
inline constexpr GLenum COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr GLenum DEPTH_ATTACHMENT = 0x8D00;
inline constexpr GLenum DEPTH_COMPONENT = 0x1902;
inline constexpr GLenum DEPTH_COMPONENT24 = 0x81A6;
inline constexpr GLenum FRAMEBUFFER_COMPLETE = 0x8CD5;
inline constexpr GLenum VERTEX_SHADER = 0x8B31;
inline constexpr GLenum FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERSION = 0x1F02;
inline constexpr GLenum COMPILE_STATUS = 0x8B81;
inline constexpr GLenum LINK_STATUS = 0x8B82;
inline constexpr GLenum UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum PACK_ALIGNMENT = 0x0D05;
inline constexpr GLenum INVALID_INDEX = 0xFFFFFFFFu;
inline constexpr GLenum NO_ERROR = 0;
// --- query objects (GL_TIMESTAMP via glQueryCounter is core in GL 3.3) ------
inline constexpr GLenum TIMESTAMP = 0x8E28;
inline constexpr GLenum QUERY_RESULT = 0x8866;
inline constexpr GLenum QUERY_RESULT_AVAILABLE = 0x8867;
}  // namespace gl

// --- function pointers --------------------------------------------------------
#define NSGL_DECL(ret, name, args) extern ret (*name) args
NSGL_DECL(const GLubyte*, glGetString, (GLenum));
NSGL_DECL(GLenum, glGetError, (void));
NSGL_DECL(void, glGetIntegerv, (GLenum, GLint*));
NSGL_DECL(void, glViewport, (GLint, GLint, GLsizei, GLsizei));
NSGL_DECL(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat));
NSGL_DECL(void, glClear, (GLbitfield));
NSGL_DECL(void, glEnable, (GLenum));
NSGL_DECL(void, glDisable, (GLenum));
NSGL_DECL(void, glBlendFunc, (GLenum, GLenum));
NSGL_DECL(void, glActiveTexture, (GLenum));
NSGL_DECL(void, glBindTexture, (GLenum, GLuint));
NSGL_DECL(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*));
NSGL_DECL(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*));
NSGL_DECL(void, glTexParameteri, (GLenum, GLenum, GLint));
NSGL_DECL(void, glGenerateMipmap, (GLenum));
NSGL_DECL(void, glPixelStorei, (GLenum, GLint));
NSGL_DECL(GLuint, glCreateShader, (GLenum));
NSGL_DECL(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*));
NSGL_DECL(void, glCompileShader, (GLuint));
NSGL_DECL(void, glGetShaderiv, (GLuint, GLenum, GLint*));
NSGL_DECL(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*));
NSGL_DECL(void, glDeleteShader, (GLuint));
NSGL_DECL(GLuint, glCreateProgram, (void));
NSGL_DECL(void, glAttachShader, (GLuint, GLuint));
NSGL_DECL(void, glLinkProgram, (GLuint));
NSGL_DECL(void, glGetProgramiv, (GLuint, GLenum, GLint*));
NSGL_DECL(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*));
NSGL_DECL(void, glDeleteProgram, (GLuint));
NSGL_DECL(void, glUseProgram, (GLuint));
NSGL_DECL(GLint, glGetUniformLocation, (GLuint, const GLchar*));
NSGL_DECL(void, glUniform1f, (GLint, GLfloat));
NSGL_DECL(void, glUniform2f, (GLint, GLfloat, GLfloat));
NSGL_DECL(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat));
NSGL_DECL(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat));
NSGL_DECL(void, glUniform1i, (GLint, GLint));
NSGL_DECL(void, glUniform2fv, (GLint, GLsizei, const GLfloat*));
NSGL_DECL(void, glUniform3fv, (GLint, GLsizei, const GLfloat*));
NSGL_DECL(void, glUniform4fv, (GLint, GLsizei, const GLfloat*));
NSGL_DECL(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*));
NSGL_DECL(void, glGetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*));
NSGL_DECL(void, glGenTextures, (GLsizei, GLuint*));
NSGL_DECL(void, glDeleteTextures, (GLsizei, const GLuint*));
NSGL_DECL(void, glGenBuffers, (GLsizei, GLuint*));
NSGL_DECL(void, glBindBuffer, (GLenum, GLuint));
NSGL_DECL(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum));
NSGL_DECL(void, glDeleteBuffers, (GLsizei, const GLuint*));
NSGL_DECL(void, glBindBufferBase, (GLenum, GLuint, GLuint));
NSGL_DECL(void, glGenVertexArrays, (GLsizei, GLuint*));
NSGL_DECL(void, glBindVertexArray, (GLuint));
NSGL_DECL(void, glDeleteVertexArrays, (GLsizei, const GLuint*));
NSGL_DECL(void, glEnableVertexAttribArray, (GLuint));
NSGL_DECL(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*));
NSGL_DECL(void, glGenFramebuffers, (GLsizei, GLuint*));
NSGL_DECL(void, glBindFramebuffer, (GLenum, GLuint));
NSGL_DECL(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum));
NSGL_DECL(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint));
NSGL_DECL(void, glDeleteFramebuffers, (GLsizei, const GLuint*));
NSGL_DECL(GLenum, glCheckFramebufferStatus, (GLenum));
NSGL_DECL(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar*));
NSGL_DECL(void, glUniformBlockBinding, (GLuint, GLuint, GLuint));
NSGL_DECL(void, glDrawArrays, (GLenum, GLint, GLsizei));
NSGL_DECL(void, glDrawElements, (GLenum, GLsizei, GLenum, const void*));
NSGL_DECL(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*));
NSGL_DECL(void, glGenQueries, (GLsizei, GLuint*));
NSGL_DECL(void, glDeleteQueries, (GLsizei, const GLuint*));
NSGL_DECL(void, glQueryCounter, (GLuint, GLenum));
NSGL_DECL(void, glGetQueryObjectiv, (GLuint, GLenum, GLint*));
NSGL_DECL(void, glGetQueryObjectui64v, (GLuint, GLenum, GLuint64*));
#undef NSGL_DECL

/** load all function pointers; returns false on failure. Call once after the
 *  context is current. */
bool glLoadFunctions();
