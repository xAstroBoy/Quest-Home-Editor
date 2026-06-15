// Minimal OpenGL 3.3 loader — no dependency needed.
// Uses glfwGetProcAddress to load all core 3.3 functions we need.
// ======================================================================

#ifndef GL_LOADER_H
#define GL_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

// ── Types ──────────────────────────────────────────────────────────────
typedef unsigned int    GLenum;
typedef unsigned char   GLboolean;
typedef unsigned int    GLbitfield;
typedef void            GLvoid;
typedef signed char     GLbyte;
typedef short           GLshort;
typedef int             GLint;
typedef unsigned char   GLubyte;
typedef unsigned short  GLushort;
typedef unsigned int    GLuint;
typedef int             GLsizei;
typedef float           GLfloat;
typedef double          GLdouble;
typedef char            GLchar;
typedef ptrdiff_t       GLsizeiptr;
typedef ptrdiff_t       GLintptr;

#define GL_FALSE                    0
#define GL_TRUE                     1

// ── Constants ──────────────────────────────────────────────────────────
#define GL_DEPTH_BUFFER_BIT         0x00000100
#define GL_STENCIL_BUFFER_BIT       0x00000400
#define GL_COLOR_BUFFER_BIT         0x00004000
#define GL_POINTS                   0x0000
#define GL_LINES                    0x0001
#define GL_LINE_LOOP                0x0002
#define GL_LINE_STRIP               0x0003
#define GL_TRIANGLES                0x0004
#define GL_TRIANGLE_STRIP           0x0005
#define GL_TRIANGLE_FAN             0x0006
#define GL_NEVER                    0x0200
#define GL_LESS                     0x0201
#define GL_EQUAL                    0x0202
#define GL_LEQUAL                   0x0203
#define GL_GREATER                  0x0204
#define GL_NOTEQUAL                 0x0205
#define GL_GEQUAL                   0x0206
#define GL_ALWAYS                   0x0207
#define GL_DEPTH_TEST               0x0B71
#define GL_STENCIL_TEST             0x0B90
#define GL_BLEND                    0x0BE2
#define GL_CULL_FACE                0x0B44
#define GL_FRONT                    0x0404
#define GL_BACK                     0x0405
#define GL_FRONT_AND_BACK           0x0408
#define GL_CW                       0x0900
#define GL_CCW                      0x0901
#define GL_NEAREST                  0x2600
#define GL_LINEAR                   0x2601
#define GL_NEAREST_MIPMAP_NEAREST   0x2700
#define GL_LINEAR_MIPMAP_NEAREST    0x2701
#define GL_NEAREST_MIPMAP_LINEAR    0x2702
#define GL_LINEAR_MIPMAP_LINEAR     0x2703
#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_TEXTURE_WRAP_T           0x2803
#define GL_TEXTURE_2D               0x0DE1
#define GL_TEXTURE0                 0x84C0
#define GL_UNSIGNED_BYTE            0x1401
#define GL_UNSIGNED_SHORT           0x1403
#define GL_UNSIGNED_INT             0x1405
#define GL_FLOAT                    0x1406
#define GL_RGBA                     0x1908
#define GL_RGB                      0x1907
#define GL_SRGB8_ALPHA8             0x8C43
#define GL_RGBA8                    0x8058
#define GL_ARRAY_BUFFER             0x8892
#define GL_ELEMENT_ARRAY_BUFFER     0x8893
#define GL_STATIC_DRAW              0x88E4
#define GL_DYNAMIC_DRAW             0x88E8
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_VERTEX_SHADER            0x8B31
#define GL_COMPILE_STATUS           0x8B81
#define GL_LINK_STATUS              0x8B82
#define GL_INFO_LOG_LENGTH          0x8B84
#define GL_NUM_EXTENSIONS           0x821D
#define GL_MAJOR_VERSION            0x821B
#define GL_MINOR_VERSION            0x821C

// ── Function pointer types ─────────────────────────────────────────────
typedef void           (APIENTRY *PFNGLACTIVETEXTUREPROC)(GLenum);
typedef void           (APIENTRY *PFNGLATTACHSHADERPROC)(GLuint, GLuint);
typedef void           (APIENTRY *PFNGLBINDBUFFERPROC)(GLenum, GLuint);
typedef void           (APIENTRY *PFNGLBINDTEXTUREPROC)(GLenum, GLuint);
typedef void           (APIENTRY *PFNGLBINDVERTEXARRAYPROC)(GLuint);
typedef void           (APIENTRY *PFNGLBUFFERDATAPROC)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void           (APIENTRY *PFNGLCLEARPROC)(GLbitfield);
typedef void           (APIENTRY *PFNGLCLEARCOLORPROC)(GLfloat,GLfloat,GLfloat,GLfloat);
typedef void           (APIENTRY *PFNGLCOMPILESHADERPROC)(GLuint);
typedef GLuint         (APIENTRY *PFNGLCREATEPROGRAMPROC)(void);
typedef GLuint         (APIENTRY *PFNGLCREATESHADERPROC)(GLenum);
typedef void           (APIENTRY *PFNGLCULLFACEPROC)(GLenum);
typedef void           (APIENTRY *PFNGLDELETEPROGRAMPROC)(GLuint);
typedef void           (APIENTRY *PFNGLDELETESHADERPROC)(GLuint);
typedef void           (APIENTRY *PFNGLDELETEBUFFERSPROC)(GLsizei, const GLuint*);
typedef void           (APIENTRY *PFNGLDELETEVERTEXARRAYSPROC)(GLsizei, const GLuint*);
typedef void           (APIENTRY *PFNGLDELETETEXTURESPROC)(GLsizei, const GLuint*);
typedef void           (APIENTRY *PFNGLDEPTHFUNCPROC)(GLenum);
typedef void           (APIENTRY *PFNGLDISABLEPROC)(GLenum);
typedef void           (APIENTRY *PFNGLDRAWARRAYSPROC)(GLenum, GLint, GLsizei);
typedef void           (APIENTRY *PFNGLDRAWELEMENTSPROC)(GLenum, GLsizei, GLenum, const void*);
typedef void           (APIENTRY *PFNGLENABLEPROC)(GLenum);
typedef void           (APIENTRY *PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint);
typedef void           (APIENTRY *PFNGLFRONTFACEPROC)(GLenum);
typedef void           (APIENTRY *PFNGLGENBUFFERSPROC)(GLsizei, GLuint*);
typedef void           (APIENTRY *PFNGLGENVERTEXARRAYSPROC)(GLsizei, GLuint*);
typedef void           (APIENTRY *PFNGLGENTEXTURESPROC)(GLsizei, GLuint*);
typedef void           (APIENTRY *PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void           (APIENTRY *PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint*);
typedef void           (APIENTRY *PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void           (APIENTRY *PFNGLGETSHADERIVPROC)(GLuint, GLenum, GLint*);
typedef const GLubyte* (APIENTRY *PFNGLGETSTRINGPROC)(GLenum);
typedef GLint          (APIENTRY *PFNGLGETUNIFORMLOCATIONPROC)(GLuint, const GLchar*);
typedef void           (APIENTRY *PFNGLLINKPROGRAMPROC)(GLuint);
typedef void           (APIENTRY *PFNGLSHADERSOURCEPROC)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void           (APIENTRY *PFNGLUNIFORM1IPROC)(GLint, GLint);
typedef void           (APIENTRY *PFNGLUNIFORMMATRIX4FVPROC)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef void           (APIENTRY *PFNGLUSEPROGRAMPROC)(GLuint);
typedef void           (APIENTRY *PFNGLVERTEXATTRIBPOINTERPROC)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void           (APIENTRY *PFNGLVIEWPORTPROC)(GLint, GLint, GLsizei, GLsizei);
typedef void           (APIENTRY *PFNGLTEXIMAGE2DPROC)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
typedef void           (APIENTRY *PFNGLTEXPARAMETERIPROC)(GLenum, GLenum, GLint);
typedef void           (APIENTRY *PFNGLACTIVETEXTUREPROC)(GLenum);

// ── Function exports ──────────────────────────────────────────────────
extern PFNGLACTIVETEXTUREPROC          glActiveTexture;
extern PFNGLATTACHSHADERPROC           glAttachShader;
extern PFNGLBINDBUFFERPROC             glBindBuffer;
extern PFNGLBINDTEXTUREPROC            glBindTexture;
extern PFNGLBINDVERTEXARRAYPROC        glBindVertexArray;
extern PFNGLBUFFERDATAPROC             glBufferData;
extern PFNGLCLEARPROC                  glClear;
extern PFNGLCLEARCOLORPROC             glClearColor;
extern PFNGLCOMPILESHADERPROC          glCompileShader;
extern PFNGLCREATEPROGRAMPROC          glCreateProgram;
extern PFNGLCREATESHADERPROC           glCreateShader;
extern PFNGLCULLFACEPROC               glCullFace;
extern PFNGLDELETEPROGRAMPROC          glDeleteProgram;
extern PFNGLDELETESHADERPROC           glDeleteShader;
extern PFNGLDELETEBUFFERSPROC          glDeleteBuffers;
extern PFNGLDELETEVERTEXARRAYSPROC     glDeleteVertexArrays;
extern PFNGLDELETETEXTURESPROC         glDeleteTextures;
extern PFNGLDEPTHFUNCPROC              glDepthFunc;
extern PFNGLDISABLEPROC                glDisable;
extern PFNGLDRAWARRAYSPROC             glDrawArrays;
extern PFNGLDRAWELEMENTSPROC           glDrawElements;
extern PFNGLENABLEPROC                 glEnable;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
extern PFNGLFRONTFACEPROC              glFrontFace;
extern PFNGLGENBUFFERSPROC             glGenBuffers;
extern PFNGLGENVERTEXARRAYSPROC        glGenVertexArrays;
extern PFNGLGENTEXTURESPROC            glGenTextures;
extern PFNGLGETPROGRAMINFOLOGPROC      glGetProgramInfoLog;
extern PFNGLGETPROGRAMIVPROC           glGetProgramiv;
extern PFNGLGETSHADERINFOLOGPROC       glGetShaderInfoLog;
extern PFNGLGETSHADERIVPROC            glGetShaderiv;
extern PFNGLGETSTRINGPROC              glGetString;
extern PFNGLGETUNIFORMLOCATIONPROC     glGetUniformLocation;
extern PFNGLLINKPROGRAMPROC            glLinkProgram;
extern PFNGLSHADERSOURCEPROC           glShaderSource;
extern PFNGLUNIFORM1IPROC              glUniform1i;
extern PFNGLUNIFORMMATRIX4FVPROC       glUniformMatrix4fv;
extern PFNGLUSEPROGRAMPROC             glUseProgram;
extern PFNGLVERTEXATTRIBPOINTERPROC    glVertexAttribPointer;
extern PFNGLVIEWPORTPROC               glViewport;
extern PFNGLTEXIMAGE2DPROC             glTexImage2D;
extern PFNGLTEXPARAMETERIPROC          glTexParameteri;

// ── Load all GL functions ──────────────────────────────────────────────
typedef void* (*GLFWgetprocaddr)(const char*);
int loadGLFunctions(GLFWgetprocaddr getProc);

#ifdef __cplusplus
}
#endif

#endif // GL_LOADER_H
