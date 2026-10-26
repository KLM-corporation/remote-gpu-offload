#pragma once
#ifndef REMOTE_GPU_CUBE_GEOMETRY_H
#define REMOTE_GPU_CUBE_GEOMETRY_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Sample {

struct Vertex {
    float pos[3];
    float color[3];
    float uv[2];
};

/**
 * @brief Builds a unit cube centred on the origin: 24 vertices (4 per face, own colour per face)
 * and 36 16-bit indices, wound clockwise as seen from outside (Direct3D default front face).
 */
inline void BuildCube(std::vector<Vertex>& vertices, std::vector<uint16_t>& indices) {
    struct Face {
        float n[3];     // outward normal
        float r[3];     // "right" as seen by a viewer outside the face
        float t[3];     // "up" as seen by that viewer; cross(r, t) == -n in a left-handed frame
        float color[3];
    };
    static const Face faces[6] = {
        {{0, 0, -1}, {1, 0, 0}, {0, 1, 0}, {1.0f, 0.25f, 0.2f}},   // front  : red
        {{0, 0, 1}, {-1, 0, 0}, {0, 1, 0}, {0.2f, 0.9f, 0.3f}},    // back   : green
        {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0.25f, 0.45f, 1.0f}},   // right  : blue
        {{-1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {1.0f, 0.85f, 0.2f}},  // left   : yellow
        {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {0.9f, 0.3f, 0.9f}},     // top    : magenta
        {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}, {0.2f, 0.9f, 0.9f}},   // bottom : cyan
    };
    static const float corners[4][4] = {
        // sign of r, sign of t, u, v
        {-1, 1, 0, 0}, {1, 1, 1, 0}, {1, -1, 1, 1}, {-1, -1, 0, 1},
    };

    vertices.clear();
    indices.clear();
    for (const Face& f : faces) {
        const uint16_t base = static_cast<uint16_t>(vertices.size());
        for (const auto& c : corners) {
            Vertex v = {};
            for (int k = 0; k < 3; ++k) v.pos[k] = 0.5f * (f.n[k] + c[0] * f.r[k] + c[1] * f.t[k]);
            std::memcpy(v.color, f.color, sizeof(v.color));
            v.uv[0] = c[2];
            v.uv[1] = c[3];
            vertices.push_back(v);
        }
        const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
        for (uint16_t i : quad) indices.push_back(static_cast<uint16_t>(base + i));
    }
}

// =============================================================================
// Minimal row-vector (v * M) matrix helpers, same conventions as DirectXMath.
// =============================================================================

struct Mat4 {
    float m[4][4];
};

inline Mat4 Identity() {
    Mat4 r = {};
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
    return r;
}

inline Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r = {};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

inline Mat4 Transpose(const Mat4& a) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[j][i];
    return r;
}

inline Mat4 RotationX(float a) {
    Mat4 r = Identity();
    const float c = std::cos(a), s = std::sin(a);
    r.m[1][1] = c; r.m[1][2] = s;
    r.m[2][1] = -s; r.m[2][2] = c;
    return r;
}

inline Mat4 RotationY(float a) {
    Mat4 r = Identity();
    const float c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c; r.m[0][2] = -s;
    r.m[2][0] = s; r.m[2][2] = c;
    return r;
}

inline Mat4 Translation(float x, float y, float z) {
    Mat4 r = Identity();
    r.m[3][0] = x; r.m[3][1] = y; r.m[3][2] = z;
    return r;
}

inline Mat4 PerspectiveFovLH(float fovY, float aspect, float zn, float zf) {
    Mat4 r = {};
    const float h = 1.0f / std::tan(fovY * 0.5f);
    r.m[0][0] = h / aspect;
    r.m[1][1] = h;
    r.m[2][2] = zf / (zf - zn);
    r.m[2][3] = 1.0f;
    r.m[3][2] = -zn * zf / (zf - zn);
    return r;
}

} // namespace Sample

#endif // REMOTE_GPU_CUBE_GEOMETRY_H
