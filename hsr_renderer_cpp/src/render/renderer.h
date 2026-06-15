#pragma once
#include "core/types.h"
#include "core/camera.h"
#include <vector>

// OpenGL 3.3 renderer — replicates libshell unlit.surface pipeline
// Requires GLFW and GLAD (gladLoadGLLoader)

class Renderer {
public:
    Camera cam;
    bool verbose = true;

    struct GpuMesh {
        u32 vao = 0, vbo = 0, ibo = 0, tex = 0;
        u32 nIdx = 0;
        float model[16]; // model matrix
    };

    std::vector<GpuMesh> gpuMeshes;
    u32 shaderProgram = 0;

    void log(const char* fmt, ...) {
        if (!verbose) return;
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "[RENDER] ");
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
    }

    bool init(int width, int height) {
        log("Initializing OpenGL renderer...");
        log("  Window: %dx%d", width, height);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glClearColor(0.07f, 0.07f, 0.12f, 1.0f);

        log("  OpenGL state set: depth test=ON, cull=back, CCW");

        if (!compileShaders()) {
            log("FATAL: Shader compilation failed");
            return false;
        }
        log("  Shaders compiled OK (program=%u)", shaderProgram);
        return true;
    }

    void uploadMesh(const MeshData& md) {
        GpuMesh gm;

        // Build interleaved VBO: px,py,pz, u,v × nVerts (stride=20)
        u32 nVerts = md.nVerts;
        std::vector<float> vboData(nVerts * 5);
        for (u32 i = 0; i < nVerts; ++i) {
            vboData[i*5+0] = md.positions[i*3+0];
            vboData[i*5+1] = md.positions[i*3+1];
            vboData[i*5+2] = md.positions[i*3+2];
            vboData[i*5+3] = (i*2 < md.uvs.size()) ? md.uvs[i*2+0] : 0;
            vboData[i*5+4] = (i*2 < md.uvs.size()) ? md.uvs[i*2+1] : 0;
        }

        glGenVertexArrays(1, &gm.vao);
        glGenBuffers(1, &gm.vbo);
        glGenBuffers(1, &gm.ibo);

        glBindVertexArray(gm.vao);

        // VBO
        glBindBuffer(GL_ARRAY_BUFFER, gm.vbo);
        glBufferData(GL_ARRAY_BUFFER, vboData.size() * sizeof(float),
                     vboData.data(), GL_STATIC_DRAW);

        // Position: location=0, 3 floats, offset=0, stride=20
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void*)0);
        glEnableVertexAttribArray(0);
        // UV: location=1, 2 floats, offset=12, stride=20
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void*)12);
        glEnableVertexAttribArray(1);

        // IBO
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, md.indices.size() * sizeof(u16),
                     md.indices.data(), GL_STATIC_DRAW);

        gm.nIdx = md.nIdx;

        // Texture
        glGenTextures(1, &gm.tex);
        glBindTexture(GL_TEXTURE_2D, gm.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, md.texW, md.texH,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, md.texRGBA.data());

        // Model matrix from transform
        buildModelMatrix(md.transform, gm.model);

        glBindVertexArray(0);
        gpuMeshes.push_back(gm);

        log("  GPU mesh uploaded: vao=%u vbo=%u ibo=%u tex=%u nIdx=%u tex=%ux%u",
            gm.vao, gm.vbo, gm.ibo, gm.tex, gm.nIdx, md.texW, md.texH);
        log("  Model matrix: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]",
            gm.model[0], gm.model[4], gm.model[8], gm.model[12],
            gm.model[1], gm.model[5], gm.model[9], gm.model[13],
            gm.model[2], gm.model[6], gm.model[10],gm.model[14],
            gm.model[3], gm.model[7], gm.model[11],gm.model[15]);
    }

    void render(int width, int height) {
        glViewport(0, 0, width, height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (gpuMeshes.empty()) {
            log("render: no meshes to draw");
            return;
        }

        glUseProgram(shaderProgram);

        float aspect = (float)width / (float)height;
        cam.updateProj(aspect);
        cam.updateView();

        // Combine proj * view → pv (column-major)
        float pv[16];
        mat4mul(cam.proj, cam.view, pv);

        int uMVP = glGetUniformLocation(shaderProgram, "uMVP");
        int uTex = glGetUniformLocation(shaderProgram, "uTexture");

        for (auto& gm : gpuMeshes) {
            // MVP = PV * model
            float mvp[16];
            mat4mul(pv, gm.model, mvp);

            glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, gm.tex);
            glUniform1i(uTex, 0);

            glBindVertexArray(gm.vao);
            glDrawElements(GL_TRIANGLES, gm.nIdx, GL_UNSIGNED_SHORT, nullptr);
        }
        glBindVertexArray(0);
        glUseProgram(0);
    }

private:
    void buildModelMatrix(const Transform& t, float out[16]) {
        float x=t.rot[0], y=t.rot[1], z=t.rot[2], w=t.rot[3];
        float sx=t.scale[0], sy=t.scale[1], sz=t.scale[2];

        float r00=1-2*(y*y+z*z), r01=2*(x*y-w*z),  r02=2*(x*z+w*y);
        float r10=2*(x*y+w*z),   r11=1-2*(x*x+z*z), r12=2*(y*z-w*x);
        float r20=2*(x*z-w*y),   r21=2*(y*z+w*x),   r22=1-2*(x*x+y*y);

        // Column-major: model[col*4 + row]
        out[0] =r00*sx; out[4] =r01*sy; out[8] =r02*sz; out[12]=t.pos[0];
        out[1] =r10*sx; out[5] =r11*sy; out[9] =r12*sz; out[13]=t.pos[1];
        out[2] =r20*sx; out[6] =r21*sy; out[10]=r22*sz; out[14]=t.pos[2];
        out[3] =0;      out[7] =0;      out[11]=0;      out[15]=1;
    }

    void mat4mul(const float a[16], const float b[16], float out[16]) {
        for (int col=0; col<4; ++col) {
            for (int row=0; row<4; ++row) {
                float sum=0;
                for (int k=0; k<4; ++k)
                    sum += a[k*4+row] * b[col*4+k];
                out[col*4+row] = sum;
            }
        }
    }

    bool compileShaders() {
        const char* vsSrc = R"GLSL(#version 330 core
            layout(location = 0) in vec3 aPos;
            layout(location = 1) in vec2 aUV;

            uniform mat4 uMVP;
            out vec2 vUV;

            void main() {
                gl_Position = uMVP * vec4(aPos, 1.0);
                vUV = vec2(aUV.x, 1.0 - aUV.y); // flip V: libshell UV origin=top-left
            }
        )GLSL";

        const char* fsSrc = R"GLSL(#version 330 core
            in vec2 vUV;
            uniform sampler2D uTexture;
            out vec4 fragColor;

            void main() {
                vec4 c = texture(uTexture, vUV);
                fragColor = vec4(c.rgb, 1.0); // force full alpha (opaque unlit)
            }
        )GLSL";

        u32 vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vsSrc, nullptr);
        glCompileShader(vs);

        int success;
        glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[1024];
            glGetShaderInfoLog(vs, 1024, nullptr, log);
            fprintf(stderr, "[RENDER] Vertex shader compile error:\n%s\n", log);
            return false;
        }

        u32 fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fsSrc, nullptr);
        glCompileShader(fs);
        glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[1024];
            glGetShaderInfoLog(fs, 1024, nullptr, log);
            fprintf(stderr, "[RENDER] Fragment shader compile error:\n%s\n", log);
            return false;
        }

        shaderProgram = glCreateProgram();
        glAttachShader(shaderProgram, vs);
        glAttachShader(shaderProgram, fs);
        glLinkProgram(shaderProgram);
        glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
        if (!success) {
            char log[1024];
            glGetProgramInfoLog(shaderProgram, 1024, nullptr, log);
            fprintf(stderr, "[RENDER] Program link error:\n%s\n", log);
            return false;
        }

        glDeleteShader(vs);
        glDeleteShader(fs);
        return true;
    }

    #include <cstdarg>
};
