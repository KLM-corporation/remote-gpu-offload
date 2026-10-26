// Minimal stand-in for the Windows SDK DirectXColors.h (absent from LLVM-MinGW):
// only the colours used by the Direct3D 11 tutorials.
#pragma once
#include <directxmath.h>
namespace DirectX { namespace Colors {
    static const XMVECTORF32 MidnightBlue = { { { 0.098039225f, 0.098039225f, 0.439215720f, 1.000000000f } } };
} }
