/* language: C11, file: host-daemon/gles_stream/gl_decl.h
 * Minimal desktop OpenGL declarations used by the GLES streaming server.
 * The NVIDIA 470 host provides OpenGL 4.5 through GLX; the server uses the
 * fixed/legacy + shader subset that is compatible with GLES 2.0.
 */
#ifndef GLES_SERVER_GL_DECL_H
#define GLES_SERVER_GL_DECL_H

#include <stdint.h>

typedef void             GLvoid;
typedef unsigned char    GLboolean;
typedef unsigned int     GLenum;
typedef unsigned int     GLbitfield;
typedef int              GLint;
typedef int              GLsizei;
typedef unsigned int     GLuint;
typedef float            GLfloat;
typedef double           GLdouble;
typedef long             GLsizeiptr;
typedef long             GLintptr;
typedef char             GLchar;
typedef unsigned char    GLubyte;

/* OpenGL / GLES 2.0 constants used by the server */
#define GL_FALSE                          0
#define GL_TRUE                           1
#define GL_NO_ERROR                       0
#define GL_INVALID_ENUM                   0x0500
#define GL_INVALID_VALUE                  0x0501
#define GL_INVALID_OPERATION              0x0502
#define GL_OUT_OF_MEMORY                  0x0505

#define GL_POINTS                         0x0000
#define GL_LINES                          0x0001
#define GL_LINE_LOOP                      0x0002
#define GL_LINE_STRIP                     0x0003
#define GL_TRIANGLES                      0x0004
#define GL_TRIANGLE_STRIP                 0x0005
#define GL_TRIANGLE_FAN                   0x0006

#define GL_DEPTH_BUFFER_BIT               0x00000100
#define GL_STENCIL_BUFFER_BIT             0x00000400
#define GL_COLOR_BUFFER_BIT               0x00004000

#define GL_DEPTH_TEST                     0x0B71
#define GL_BLEND                          0x0BE2
#define GL_SCISSOR_TEST                   0x0C11
#define GL_CULL_FACE                      0x0B44
#define GL_PACK_ALIGNMENT                 0x0D05
#define GL_UNPACK_ALIGNMENT               0x0CF5

#define GL_UNSIGNED_BYTE                  0x1401
#define GL_UNSIGNED_SHORT                 0x1403
#define GL_UNSIGNED_INT                   0x1405
#define GL_FLOAT                          0x1406

#define GL_RGB                            0x1907
#define GL_RGBA                           0x1908
#define GL_RED                            0x1903
#define GL_RG                            0x8227
#define GL_BGRA                           0x80E1

#define GL_VENDOR                         0x1F00
#define GL_RENDERER                       0x1F01
#define GL_VERSION                        0x1F02
#define GL_EXTENSIONS                     0x1F03

#define GL_NEAREST                        0x2600
#define GL_LINEAR                         0x2601
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_REPEAT                         0x2901
#define GL_CLAMP_TO_EDGE                  0x812F

#define GL_TEXTURE_2D                     0x0DE1
#define GL_TEXTURE0                       0x84C0

#define GL_ARRAY_BUFFER                   0x8892
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_STATIC_DRAW                    0x88E4
#define GL_DYNAMIC_DRAW                   0x88E8

#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_VALIDATE_STATUS                0x8B83
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_ATTACHED_SHADERS               0x8B85
#define GL_ACTIVE_UNIFORMS                0x8B86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH      0x8B87
#define GL_ACTIVE_ATTRIBUTES              0x8B89
#define GL_ACTIVE_ATTRIBUTE_MAX_LENGTH    0x8B88
#define GL_SHADER_TYPE                    0x8B4F
#define GL_DELETE_STATUS                  0x8B80
#define GL_SHADER_SOURCE_LENGTH           0x8B88

#define GL_SRC_ALPHA                      0x0302
#define GL_ONE_MINUS_SRC_ALPHA            0x0303
#define GL_ONE                            1
#define GL_ZERO                           0
#define GL_NEVER                          0x0200
#define GL_LESS                           0x0201
#define GL_EQUAL                          0x0202
#define GL_LEQUAL                         0x0203
#define GL_GREATER                        0x0204
#define GL_ALWAYS                         0x0207
#define GL_FRONT                          0x0404
#define GL_BACK                           0x0405
#define GL_FRONT_AND_BACK                 0x0408
#define GL_CW                             0x0900
#define GL_CCW                            0x0901

/* Textures */
extern void      glGenTextures(GLsizei n, GLuint *textures);
extern void      glBindTexture(GLenum target, GLuint texture);
extern void      glTexImage2D(GLenum target, GLint level, GLint internalformat,
                              GLsizei width, GLsizei height, GLint border,
                              GLenum format, GLenum type, const void *pixels);
extern void      glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                 GLsizei width, GLsizei height, GLenum format, GLenum type,
                                 const void *pixels);
extern void      glTexParameteri(GLenum target, GLenum pname, GLint param);
extern void      glTexParameterf(GLenum target, GLenum pname, GLfloat param);
extern void      glDeleteTextures(GLsizei n, const GLuint *textures);
extern void      glActiveTexture(GLenum texture);
extern void      glGenerateMipmap(GLenum target);
extern void      glPixelStorei(GLenum pname, GLint param);

/* Buffers */
extern void      glGenBuffers(GLsizei n, GLuint *buffers);
extern void      glBindBuffer(GLenum target, GLuint buffer);
extern void      glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
extern void      glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
extern void      glDeleteBuffers(GLsizei n, const GLuint *buffers);

/* Shaders / programs */
extern GLuint    glCreateShader(GLenum type);
extern void      glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
extern void      glCompileShader(GLuint shader);
extern void      glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
extern void      glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
extern void      glDeleteShader(GLuint shader);

extern GLuint    glCreateProgram(void);
extern void      glAttachShader(GLuint program, GLuint shader);
extern void      glLinkProgram(GLuint program);
extern void      glGetProgramiv(GLuint program, GLenum pname, GLint *params);
extern void      glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
extern void      glUseProgram(GLuint program);
extern void      glDeleteProgram(GLuint program);

extern GLint     glGetAttribLocation(GLuint program, const GLchar *name);
extern GLint     glGetUniformLocation(GLuint program, const GLchar *name);
extern void      glVertexAttrib1f(GLuint index, GLfloat x);
extern void      glVertexAttrib1fv(GLuint index, const GLfloat *v);
extern void      glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                       GLboolean normalized, GLsizei stride, const void *pointer);
extern void      glEnableVertexAttribArray(GLuint index);
extern void      glDisableVertexAttribArray(GLuint index);

/* Uniforms */
extern void      glUniform1f(GLint location, GLfloat v0);
extern void      glUniform1fv(GLint location, GLsizei count, const GLfloat *value);
extern void      glUniform2fv(GLint location, GLsizei count, const GLfloat *value);
extern void      glUniform3fv(GLint location, GLsizei count, const GLfloat *value);
extern void      glUniform4fv(GLint location, GLsizei count, const GLfloat *value);
extern void      glUniform1i(GLint location, GLint v0);
extern void      glUniform1iv(GLint location, GLsizei count, const GLint *value);
extern void      glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
extern void      glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
extern void      glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);

/* State / draw */
extern void      glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
extern void      glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
extern void      glClear(GLbitfield mask);
extern void      glDrawArrays(GLenum mode, GLint first, GLsizei count);
extern void      glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
extern void      glEnable(GLenum cap);
extern void      glDisable(GLenum cap);
extern void      glBlendFunc(GLenum sfactor, GLenum dfactor);
extern void      glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB,
                                     GLenum sfactorAlpha, GLenum dfactorAlpha);
extern void      glDepthFunc(GLenum func);
extern void      glDepthMask(GLboolean flag);
extern void      glCullFace(GLenum mode);
extern void      glFrontFace(GLenum mode);
extern void      glScissor(GLint x, GLint y, GLsizei width, GLsizei height);

/* Readback / misc */
extern void      glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                              GLenum format, GLenum type, void *pixels);
extern GLenum    glGetError(void);
extern const GLubyte *glGetString(GLenum name);
extern void      glFlush(void);
extern void      glFinish(void);

#endif /* GLES_SERVER_GL_DECL_H */
