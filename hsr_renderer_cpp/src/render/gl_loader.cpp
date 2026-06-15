// Minimal OpenGL 3.3 loader implementation

#define GL_LOADER_IMPLEMENTATION
#include "render/gl_loader.h"
#include <cstdio>

// Function pointer definitions
PFNGLACTIVETEXTUREPROC          glActiveTexture = nullptr;
PFNGLATTACHSHADERPROC           glAttachShader = nullptr;
PFNGLBINDBUFFERPROC             glBindBuffer = nullptr;
PFNGLBINDTEXTUREPROC            glBindTexture = nullptr;
PFNGLBINDVERTEXARRAYPROC        glBindVertexArray = nullptr;
PFNGLBUFFERDATAPROC             glBufferData = nullptr;
PFNGLCLEARPROC                  glClear = nullptr;
PFNGLCLEARCOLORPROC             glClearColor = nullptr;
PFNGLCOMPILESHADERPROC          glCompileShader = nullptr;
PFNGLCREATEPROGRAMPROC          glCreateProgram = nullptr;
PFNGLCREATESHADERPROC           glCreateShader = nullptr;
PFNGLCULLFACEPROC               glCullFace = nullptr;
PFNGLDELETEPROGRAMPROC          glDeleteProgram = nullptr;
PFNGLDELETESHADERPROC           glDeleteShader = nullptr;
PFNGLDELETEBUFFERSPROC          glDeleteBuffers = nullptr;
PFNGLDELETEVERTEXARRAYSPROC     glDeleteVertexArrays = nullptr;
PFNGLDELETETEXTURESPROC         glDeleteTextures = nullptr;
PFNGLDEPTHFUNCPROC              glDepthFunc = nullptr;
PFNGLDISABLEPROC                glDisable = nullptr;
PFNGLDRAWARRAYSPROC             glDrawArrays = nullptr;
PFNGLDRAWELEMENTSPROC           glDrawElements = nullptr;
PFNGLENABLEPROC                 glEnable = nullptr;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = nullptr;
PFNGLFRONTFACEPROC              glFrontFace = nullptr;
PFNGLGENBUFFERSPROC             glGenBuffers = nullptr;
PFNGLGENVERTEXARRAYSPROC        glGenVertexArrays = nullptr;
PFNGLGENTEXTURESPROC            glGenTextures = nullptr;
PFNGLGETPROGRAMINFOLOGPROC      glGetProgramInfoLog = nullptr;
PFNGLGETPROGRAMIVPROC           glGetProgramiv = nullptr;
PFNGLGETSHADERINFOLOGPROC       glGetShaderInfoLog = nullptr;
PFNGLGETSHADERIVPROC            glGetShaderiv = nullptr;
PFNGLGETSTRINGPROC              glGetString = nullptr;
PFNGLGETUNIFORMLOCATIONPROC     glGetUniformLocation = nullptr;
PFNGLLINKPROGRAMPROC            glLinkProgram = nullptr;
PFNGLSHADERSOURCEPROC           glShaderSource = nullptr;
PFNGLUNIFORM1IPROC              glUniform1i = nullptr;
PFNGLUNIFORMMATRIX4FVPROC       glUniformMatrix4fv = nullptr;
PFNGLUSEPROGRAMPROC             glUseProgram = nullptr;
PFNGLVERTEXATTRIBPOINTERPROC    glVertexAttribPointer = nullptr;
PFNGLVIEWPORTPROC               glViewport = nullptr;
PFNGLTEXIMAGE2DPROC             glTexImage2D = nullptr;
PFNGLTEXPARAMETERIPROC          glTexParameteri = nullptr;

int loadGLFunctions(GLFWgetprocaddr getProc) {
    #define LOAD(name) \
        name = (decltype(name))getProc(#name); \
        if (!name) { \
            fprintf(stderr, "[GL] FAILED to load function: %s\n", #name); \
            return 0; \
        }

    LOAD(glActiveTexture);
    LOAD(glAttachShader);
    LOAD(glBindBuffer);
    LOAD(glBindTexture);
    LOAD(glBindVertexArray);
    LOAD(glBufferData);
    LOAD(glClear);
    LOAD(glClearColor);
    LOAD(glCompileShader);
    LOAD(glCreateProgram);
    LOAD(glCreateShader);
    LOAD(glCullFace);
    LOAD(glDeleteProgram);
    LOAD(glDeleteShader);
    LOAD(glDeleteBuffers);
    LOAD(glDeleteVertexArrays);
    LOAD(glDeleteTextures);
    LOAD(glDepthFunc);
    LOAD(glDisable);
    LOAD(glDrawArrays);
    LOAD(glDrawElements);
    LOAD(glEnable);
    LOAD(glEnableVertexAttribArray);
    LOAD(glFrontFace);
    LOAD(glGenBuffers);
    LOAD(glGenVertexArrays);
    LOAD(glGenTextures);
    LOAD(glGetProgramInfoLog);
    LOAD(glGetProgramiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glGetShaderiv);
    LOAD(glGetString);
    LOAD(glGetUniformLocation);
    LOAD(glLinkProgram);
    LOAD(glShaderSource);
    LOAD(glUniform1i);
    LOAD(glUniformMatrix4fv);
    LOAD(glUseProgram);
    LOAD(glVertexAttribPointer);
    LOAD(glViewport);
    LOAD(glTexImage2D);
    LOAD(glTexParameteri);

    #undef LOAD
    fprintf(stderr, "[GL] All 38 core OpenGL functions loaded successfully\n");
    return 1;
}
