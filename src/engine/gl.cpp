// ---------------------------------------------------------------------------
// GL function pointer definitions + loader (gl.hpp declares them extern).
// ---------------------------------------------------------------------------
#include "engine/gl.hpp"

// function pointer storage (external linkage, one definition here)
#define NSGL_DECL(ret, name, args) ret (*name) args = nullptr
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

bool glLoadFunctions() {
#define NSGL_LOAD(name)                                                        \
  do {                                                                         \
    ::name = reinterpret_cast<decltype(::name)>(::glfwGetProcAddress(#name));  \
    if (!::name) return false;                                                 \
  } while (0)
  NSGL_LOAD(glGetString);
  NSGL_LOAD(glGetError);
  NSGL_LOAD(glGetIntegerv);
  NSGL_LOAD(glViewport);
  NSGL_LOAD(glClearColor);
  NSGL_LOAD(glClear);
  NSGL_LOAD(glEnable);
  NSGL_LOAD(glDisable);
  NSGL_LOAD(glBlendFunc);
  NSGL_LOAD(glActiveTexture);
  NSGL_LOAD(glBindTexture);
  NSGL_LOAD(glTexImage2D);
  NSGL_LOAD(glTexParameteri);
  NSGL_LOAD(glGenerateMipmap);
  NSGL_LOAD(glPixelStorei);
  NSGL_LOAD(glCreateShader);
  NSGL_LOAD(glShaderSource);
  NSGL_LOAD(glCompileShader);
  NSGL_LOAD(glGetShaderiv);
  NSGL_LOAD(glGetShaderInfoLog);
  NSGL_LOAD(glDeleteShader);
  NSGL_LOAD(glCreateProgram);
  NSGL_LOAD(glAttachShader);
  NSGL_LOAD(glLinkProgram);
  NSGL_LOAD(glGetProgramiv);
  NSGL_LOAD(glGetProgramInfoLog);
  NSGL_LOAD(glDeleteProgram);
  NSGL_LOAD(glUseProgram);
  NSGL_LOAD(glGetUniformLocation);
  NSGL_LOAD(glUniform1f);
  NSGL_LOAD(glUniform2f);
  NSGL_LOAD(glUniform3f);
  NSGL_LOAD(glUniform4f);
  NSGL_LOAD(glUniform1i);
  NSGL_LOAD(glUniform2fv);
  NSGL_LOAD(glUniform3fv);
  NSGL_LOAD(glUniform4fv);
  NSGL_LOAD(glUniformMatrix4fv);
  NSGL_LOAD(glGetActiveUniform);
  NSGL_LOAD(glGenTextures);
  NSGL_LOAD(glDeleteTextures);
  NSGL_LOAD(glGenBuffers);
  NSGL_LOAD(glBindBuffer);
  NSGL_LOAD(glBufferData);
  NSGL_LOAD(glDeleteBuffers);
  NSGL_LOAD(glBindBufferBase);
  NSGL_LOAD(glGenVertexArrays);
  NSGL_LOAD(glBindVertexArray);
  NSGL_LOAD(glDeleteVertexArrays);
  NSGL_LOAD(glEnableVertexAttribArray);
  NSGL_LOAD(glVertexAttribPointer);
  NSGL_LOAD(glGenFramebuffers);
  NSGL_LOAD(glBindFramebuffer);
  NSGL_LOAD(glBlitFramebuffer);
  NSGL_LOAD(glFramebufferTexture2D);
  NSGL_LOAD(glDeleteFramebuffers);
  NSGL_LOAD(glCheckFramebufferStatus);
  NSGL_LOAD(glGetUniformBlockIndex);
  NSGL_LOAD(glUniformBlockBinding);
  NSGL_LOAD(glDrawArrays);
  NSGL_LOAD(glDrawElements);
  NSGL_LOAD(glReadPixels);
  NSGL_LOAD(glGenQueries);
  NSGL_LOAD(glDeleteQueries);
  NSGL_LOAD(glQueryCounter);
  NSGL_LOAD(glGetQueryObjectiv);
  NSGL_LOAD(glGetQueryObjectui64v);
#undef NSGL_LOAD
  return true;
}
