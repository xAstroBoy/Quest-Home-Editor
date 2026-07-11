#pragma once
#include <cmath>

// Freecam camera — matches libshell's MainRenderSceneView camera
// Y-up world, Vulkan-style clip space

struct Camera {
    // Spawn where the ENV places the player: Meta Home envs put the avatar at the world/tracking
    // ORIGIN (markup.json avatar_position [0,0,0], floor at Y=0), standing eye height ~1.6 m, facing
    // forward (yaw 0). This matches what the headset shows on load. (HSR_CAM overrides for captures.)
    float pos[3]   = {0.0f, 1.6f, 0.0f};
    float yaw      = 0.0f;
    float pitch    = 0.0f;
    float speed    = 3.0f;    // m/s
    float nearZ    = 0.02f;   // close near plane so you can get right up to a mesh without it clipping away (reversed-Z float depth keeps precision)
    float farZ     = 40000.0f;   // the vista/skybox sphere is ~13k out (up to ~26k across) — must be inside the far plane or it gets clipped away (flat clear-color background)
    float fovDeg   = 75.0f;

    // Derived matrices
    float view[16];
    float proj[16];

    void updateProj(float aspect) {
        float f = 1.0f / tanf(fovDeg * 0.5f * 3.14159265f / 180.0f);
        float fn = farZ - nearZ;
        // Vulkan RH perspective, depth [0..1], Y-flipped for Vulkan NDC (Y-down).
        // REVERSED-Z: near->1, far->0. Gives near-uniform depth precision across the whole
        // range, so a huge far plane (40k, for the vista skybox) doesn't cause z-fighting on
        // the close, detailed station geometry. Pair with depth clear=0 and compareOp=GREATER.
        // At z_eye=-near: z_ndc = near/near = 1 ; at z_eye=-far: z_ndc = 0.
        float A = nearZ / fn;
        float B = nearZ * farZ / fn;

        proj[0]=f/aspect; proj[4]=0;  proj[8]=0;   proj[12]=0;
        proj[1]=0;        proj[5]=-f; proj[9]=0;   proj[13]=0;
        proj[2]=0;        proj[6]=0;  proj[10]=A;  proj[14]=B;
        proj[3]=0;        proj[7]=0;  proj[11]=-1; proj[15]=0;
    }

    void updateView() {
        float cy = cosf(yaw), sy = sinf(yaw);
        float cp = cosf(pitch), sp = sinf(pitch);

        float fx =  sy * cp;
        float fy =  sp;
        float fz = -cy * cp;

        // Right = front × up (up = 0,1,0)
        float rx =  fy*0 - fz*1;
        float ry =  fz*0 - fx*0;
        float rz =  fx*1 - fy*0;
        float rlen = sqrtf(rx*rx + ry*ry + rz*rz);
        if (rlen > 0) { rx/=rlen; ry/=rlen; rz/=rlen; }

        // Up = right × front
        float ux = ry*fz - rz*fy;
        float uy = rz*fx - rx*fz;
        float uz = rx*fy - ry*fx;

        // Column-major layout: basis vectors go into columns, translation in column 3
        view[0]=rx;   view[4]=ry;   view[8]=rz;    view[12]=-(rx*pos[0]+ry*pos[1]+rz*pos[2]);
        view[1]=ux;   view[5]=uy;   view[9]=uz;    view[13]=-(ux*pos[0]+uy*pos[1]+uz*pos[2]);
        view[2]=-fx;  view[6]=-fy;  view[10]=-fz;  view[14]= (fx*pos[0]+fy*pos[1]+fz*pos[2]);
        view[3]=0;    view[7]=0;    view[11]=0;    view[15]=1;
    }

    void moveForward(float dt) {
        float fx = sinf(yaw) * cosf(pitch);
        float fz = -cosf(yaw) * cosf(pitch);
        pos[0] += fx * speed * dt;
        pos[2] += fz * speed * dt;
    }

    void moveBack(float dt) {
        float fx = sinf(yaw) * cosf(pitch);
        float fz = -cosf(yaw) * cosf(pitch);
        pos[0] -= fx * speed * dt;
        pos[2] -= fz * speed * dt;
    }

    void moveRight(float dt) {
        float rx = cosf(yaw);
        float rz = sinf(yaw);
        pos[0] += rx * speed * dt;
        pos[2] += rz * speed * dt;
    }

    void moveLeft(float dt) {
        float rx = cosf(yaw);
        float rz = sinf(yaw);
        pos[0] -= rx * speed * dt;
        pos[2] -= rz * speed * dt;
    }

    void moveUp(float dt)   { pos[1] += speed * dt; }
    void moveDown(float dt) { pos[1] -= speed * dt; }

    void rotate(float dx, float dy) {
        yaw   += dx * 0.003f;   // drag-look: mouse LEFT (dx<0) -> yaw decreases -> look LEFT (was inverted: -= made left look right)
        pitch -= dy * 0.003f;
        if (pitch > 1.55f) pitch = 1.55f;
        if (pitch < -1.55f) pitch = -1.55f;
    }

    float minSpeed = 0.1f;
    float maxSpeed = 2000.0f;   // big maps put props hundreds of m out; let the flycam actually reach them
    void adjustSpeed(float delta) {
        speed *= delta;
        if (speed < minSpeed) speed = minSpeed;
        if (speed > maxSpeed) speed = maxSpeed;
    }
};
