// Compatibility shim for building the Microsoft Direct3D 11 tutorials with LLVM-MinGW.
// LLVM-MinGW ships only the DirectXMath storage types (XMFLOAT2/3/4, ...). This header pulls in
// that file and adds the subset of the DirectXMath API the tutorials use, with the same
// conventions (row vectors, row-major XMMATRIX, left-handed matrices). Scalar, not SIMD.
#pragma once

#include_next <directxmath.h>

#include <cmath>

namespace DirectX {

constexpr float XM_PI = 3.141592654f;
constexpr float XM_PIDIV2 = 1.570796327f;
constexpr float XM_PIDIV4 = 0.785398163f;

struct alignas(16) XMVECTOR {
    float f[4];
};
using FXMVECTOR = const XMVECTOR&;

struct alignas(16) XMVECTORF32 {
    union {
        float f[4];
        XMVECTOR v;
    };
    operator XMVECTOR() const { return v; }
    operator const float*() const { return f; }
};

struct alignas(16) XMMATRIX {
    XMVECTOR r[4];
};
using FXMMATRIX = const XMMATRIX&;
using CXMMATRIX = const XMMATRIX&;

inline XMVECTOR XMVectorSet(float x, float y, float z, float w) { return XMVECTOR{{x, y, z, w}}; }
inline XMVECTOR operator*(float s, FXMVECTOR v) { return XMVECTOR{{s * v.f[0], s * v.f[1], s * v.f[2], s * v.f[3]}}; }
inline XMVECTOR operator*(FXMVECTOR v, float s) { return s * v; }
inline XMVECTOR XMLoadFloat4(const XMFLOAT4* p) { return XMVECTOR{{p->x, p->y, p->z, p->w}}; }
inline void XMStoreFloat4(XMFLOAT4* p, FXMVECTOR v) { *p = XMFLOAT4(v.f[0], v.f[1], v.f[2], v.f[3]); }

inline XMMATRIX XMMatrixIdentity() {
    return XMMATRIX{{{{1, 0, 0, 0}}, {{0, 1, 0, 0}}, {{0, 0, 1, 0}}, {{0, 0, 0, 1}}}};
}

inline XMMATRIX XMMatrixMultiply(CXMMATRIX a, CXMMATRIX b) {
    XMMATRIX m = {};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) m.r[i].f[j] += a.r[i].f[k] * b.r[k].f[j];
    return m;
}
inline XMMATRIX operator*(CXMMATRIX a, CXMMATRIX b) { return XMMatrixMultiply(a, b); }

inline XMMATRIX XMMatrixTranspose(CXMMATRIX a) {
    XMMATRIX m;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) m.r[i].f[j] = a.r[j].f[i];
    return m;
}

inline XMMATRIX XMMatrixScaling(float x, float y, float z) {
    XMMATRIX m = XMMatrixIdentity();
    m.r[0].f[0] = x; m.r[1].f[1] = y; m.r[2].f[2] = z;
    return m;
}

inline XMMATRIX XMMatrixTranslation(float x, float y, float z) {
    XMMATRIX m = XMMatrixIdentity();
    m.r[3] = XMVECTOR{{x, y, z, 1}};
    return m;
}
inline XMMATRIX XMMatrixTranslationFromVector(FXMVECTOR v) { return XMMatrixTranslation(v.f[0], v.f[1], v.f[2]); }

inline XMMATRIX XMMatrixRotationY(float a) {
    const float c = std::cos(a), s = std::sin(a);
    XMMATRIX m = XMMatrixIdentity();
    m.r[0] = XMVECTOR{{c, 0, -s, 0}};
    m.r[2] = XMVECTOR{{s, 0, c, 0}};
    return m;
}

inline XMMATRIX XMMatrixRotationZ(float a) {
    const float c = std::cos(a), s = std::sin(a);
    XMMATRIX m = XMMatrixIdentity();
    m.r[0] = XMVECTOR{{c, s, 0, 0}};
    m.r[1] = XMVECTOR{{-s, c, 0, 0}};
    return m;
}

inline XMMATRIX XMMatrixPerspectiveFovLH(float fovY, float aspect, float zn, float zf) {
    const float h = 1.0f / std::tan(fovY * 0.5f), range = zf / (zf - zn);
    XMMATRIX m = {};
    m.r[0].f[0] = h / aspect;
    m.r[1].f[1] = h;
    m.r[2].f[2] = range;
    m.r[2].f[3] = 1.0f;
    m.r[3].f[2] = -range * zn;
    return m;
}

inline XMMATRIX XMMatrixLookAtLH(FXMVECTOR eye, FXMVECTOR at, FXMVECTOR up) {
    auto sub = [](FXMVECTOR a, FXMVECTOR b) { return XMVECTOR{{a.f[0] - b.f[0], a.f[1] - b.f[1], a.f[2] - b.f[2], 0}}; };
    auto dot = [](FXMVECTOR a, FXMVECTOR b) { return a.f[0] * b.f[0] + a.f[1] * b.f[1] + a.f[2] * b.f[2]; };
    auto cross = [](FXMVECTOR a, FXMVECTOR b) {
        return XMVECTOR{{a.f[1] * b.f[2] - a.f[2] * b.f[1], a.f[2] * b.f[0] - a.f[0] * b.f[2], a.f[0] * b.f[1] - a.f[1] * b.f[0], 0}};
    };
    auto normalize = [&](FXMVECTOR a) {
        const float len = std::sqrt(dot(a, a));
        return XMVECTOR{{a.f[0] / len, a.f[1] / len, a.f[2] / len, 0}};
    };
    const XMVECTOR z = normalize(sub(at, eye));
    const XMVECTOR x = normalize(cross(up, z));
    const XMVECTOR y = cross(z, x);
    XMMATRIX m;
    m.r[0] = XMVECTOR{{x.f[0], y.f[0], z.f[0], 0}};
    m.r[1] = XMVECTOR{{x.f[1], y.f[1], z.f[1], 0}};
    m.r[2] = XMVECTOR{{x.f[2], y.f[2], z.f[2], 0}};
    m.r[3] = XMVECTOR{{-dot(x, eye), -dot(y, eye), -dot(z, eye), 1}};
    return m;
}

// Transforms (x, y, z, 1) by m.
inline XMVECTOR XMVector3Transform(FXMVECTOR v, CXMMATRIX m) {
    XMVECTOR r = {};
    for (int j = 0; j < 4; ++j) r.f[j] = v.f[0] * m.r[0].f[j] + v.f[1] * m.r[1].f[j] + v.f[2] * m.r[2].f[j] + m.r[3].f[j];
    return r;
}

} // namespace DirectX

using std::cos;
using std::cosf;
using std::sin;
using std::sinf;
