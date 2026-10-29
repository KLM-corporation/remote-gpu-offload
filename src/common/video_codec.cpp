#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows 10 declarations (MF_LOW_LATENCY is Windows 8+); nothing here is linked statically, so
// the binaries still load on older systems.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>
#include <initguid.h> // defines the Media Foundation GUIDs below (selectany), no uuid library needed
#include <codecapi.h>
#include <d3d10.h> // ID3D10Multithread
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wmcodecdsp.h>
#include <emmintrin.h>

#include "video_codec.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>

namespace RemoteGPU {
namespace Video {

namespace {

constexpr UINT32 kFrameRate = 60;          // nominal rate for rate control; real frames come faster or slower
constexpr LONGLONG kFrameDuration = 10000000 / kFrameRate;
constexpr UINT32 kGopFrames = 600;         // a fresh IDR every 10 s; the transport never loses a frame
constexpr int kInputTimeoutMs = 2000;      // an encoder that accepts no input for this long is broken
constexpr int kOutputTimeoutMs = 50;       // a later frame carries the access unit if it is slower

// MinGW's strmif.h declares ICodecAPI without the __uuidof that IID_PPV_ARGS needs.
constexpr GUID kIidCodecApi = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};

template <class T> void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string EnvVar(const char* name) {
    char buffer[256];
    const DWORD n = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return (n > 0 && n < sizeof(buffer)) ? std::string(buffer, n) : std::string();
}

template <class F> void LoadProc(HMODULE dll, const char* name, F& fn) {
    fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
}

// The few mfplat.dll exports used here, resolved at run time.
struct MfApi {
    decltype(&MFStartup) Startup = nullptr;
    decltype(&MFShutdown) Shutdown = nullptr;
    decltype(&MFTEnumEx) TEnumEx = nullptr;
    decltype(&MFCreateMediaType) CreateMediaType = nullptr;
    decltype(&MFCreateSample) CreateSample = nullptr;
    decltype(&MFCreateMemoryBuffer) CreateMemoryBuffer = nullptr;
    decltype(&MFCreateDXGIDeviceManager) CreateDXGIDeviceManager = nullptr; // optional: hardware decoding
};

const MfApi* Mf() {
    static const MfApi* const api = []() -> const MfApi* {
        HMODULE dll = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) return nullptr;
        static MfApi a;
        LoadProc(dll, "MFStartup", a.Startup);
        LoadProc(dll, "MFShutdown", a.Shutdown);
        LoadProc(dll, "MFTEnumEx", a.TEnumEx);
        LoadProc(dll, "MFCreateMediaType", a.CreateMediaType);
        LoadProc(dll, "MFCreateSample", a.CreateSample);
        LoadProc(dll, "MFCreateMemoryBuffer", a.CreateMemoryBuffer);
        LoadProc(dll, "MFCreateDXGIDeviceManager", a.CreateDXGIDeviceManager);
        if (!a.Startup || !a.Shutdown || !a.TEnumEx || !a.CreateMediaType || !a.CreateSample || !a.CreateMemoryBuffer) {
            return nullptr;
        }
        return &a;
    }();
    return api;
}

// D3D11CreateDevice from the system's d3d11.dll, by full path: the application folder may hold
// this project's own d3d11.dll proxy.
decltype(&D3D11CreateDevice) D3D11Create() {
    static const decltype(&D3D11CreateDevice) fn = []() -> decltype(&D3D11CreateDevice) {
        wchar_t path[MAX_PATH];
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n + 11 > MAX_PATH) return nullptr;
        std::wcscpy(path + n, L"\\d3d11.dll");
        HMODULE dll = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        decltype(&D3D11CreateDevice) create = nullptr;
        if (dll) LoadProc(dll, "D3D11CreateDevice", create);
        return create;
    }();
    return fn;
}

// COM and Media Foundation for the calling thread, released in reverse order by Stop.
class MfScope {
public:
    bool Start() {
        if (!Mf()) return false;
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_com = SUCCEEDED(hr); // RPC_E_CHANGED_MODE: the thread already joined an STA, which works too
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
        m_mf = SUCCEEDED(Mf()->Startup(MF_VERSION, MFSTARTUP_LITE));
        if (!m_mf) Stop();
        return m_mf;
    }
    void Stop() {
        if (m_mf) Mf()->Shutdown();
        if (m_com) CoUninitialize();
        m_mf = m_com = false;
    }

private:
    bool m_com = false;
    bool m_mf = false;
};

void SetCodecUInt(ICodecAPI* api, const GUID& property, UINT32 value) {
    VARIANT v = {};
    v.vt = VT_UI4;
    v.ulVal = value;
    api->SetValue(&property, &v); // optional tuning: an encoder without the property keeps its default
}

void SetCodecBool(ICodecAPI* api, const GUID& property, bool value) {
    VARIANT v = {};
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&property, &v);
}

void SetVideoAttributes(IMFMediaType* t, uint32_t width, uint32_t height) {
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, kFrameRate, 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
}

IMFMediaType* NewVideoType(const GUID& subtype, uint32_t width, uint32_t height) {
    IMFMediaType* t = nullptr;
    if (FAILED(Mf()->CreateMediaType(&t))) return nullptr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    SetVideoAttributes(t, width, height);
    return t;
}

// A sample holding one memory buffer of `capacity` bytes.
IMFSample* NewSample(DWORD capacity) {
    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    if (FAILED(Mf()->CreateMemoryBuffer(capacity, &buffer))) return nullptr;
    if (FAILED(Mf()->CreateSample(&sample)) || FAILED(sample->AddBuffer(buffer))) SafeRelease(sample);
    buffer->Release();
    return sample;
}

std::string FriendlyName(IMFActivate* activate) {
    WCHAR* wide = nullptr;
    UINT32 length = 0;
    if (FAILED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wide, &length))) return "unnamed encoder";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string name(static_cast<size_t>(std::max(bytes, 0)), '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), &name[0], bytes, nullptr, nullptr);
    CoTaskMemFree(wide);
    return name;
}

constexpr uint32_t kNalIdr = 1u << 5;
constexpr uint32_t kNalSps = 1u << 7;

// Bit n is set when the Annex-B bytes contain a NAL unit of type n.
uint32_t NalTypes(const uint8_t* p, size_t n) {
    uint32_t types = 0;
    for (size_t i = 0; i + 3 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) types |= 1u << (p[i + 3] & 0x1F);
    }
    return types;
}

void PutUnit(std::vector<uint8_t>& out, const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len) {
    const uint32_t size = static_cast<uint32_t>(a_len + b_len);
    const auto* s = reinterpret_cast<const uint8_t*>(&size);
    out.insert(out.end(), s, s + sizeof(size));
    out.insert(out.end(), a, a + a_len);
    out.insert(out.end(), b, b + b_len);
}

// Full-range BT.709, 12-bit fixed point; chroma comes from the sum of a 2x2 block.
inline uint8_t Luma(int r, int g, int b) {
    return static_cast<uint8_t>((871 * r + 2929 * g + 296 * b + 2048) >> 12);
}
inline uint8_t ChromaU(int r4, int g4, int b4) {
    return static_cast<uint8_t>(std::min((-469 * r4 - 1579 * g4 + 2048 * b4 + (128 << 14) + (1 << 13)) >> 14, 255));
}
inline uint8_t ChromaV(int r4, int g4, int b4) {
    return static_cast<uint8_t>(std::min((2048 * r4 - 1860 * g4 - 188 * b4 + (128 << 14) + (1 << 13)) >> 14, 255));
}

inline uint8_t Clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// Inverse transform with the exact arithmetic of the SSE2 loop (chroma scaled by 64, products
// kept in their high 16 bits), so both paths give identical pixels.
inline void YuvToBgra(int y, int u6, int v6, uint8_t* out) {
    out[0] = Clamp8(y + ((u6 * 1900) >> 16));
    out[1] = Clamp8(y - ((u6 * 192) >> 16) - ((v6 * 479) >> 16));
    out[2] = Clamp8(y + ((v6 * 1613) >> 16));
    out[3] = 255;
}

} // namespace

bool IsAvailable() { return Mf() != nullptr; }

namespace {

// SSE2 versions of Luma/ChromaU/ChromaV with the same integer arithmetic. Multiply-adds work on
// pixels widened to [B G R A] 16-bit lanes; _mm_shuffle_ps then pairs up the two partial sums of
// each pixel (lanes 0+1 and 2+3).
inline __m128i PairSums(__m128i lo, __m128i hi) {
    const __m128 even = _mm_shuffle_ps(_mm_castsi128_ps(lo), _mm_castsi128_ps(hi), _MM_SHUFFLE(2, 0, 2, 0));
    const __m128 odd = _mm_shuffle_ps(_mm_castsi128_ps(lo), _mm_castsi128_ps(hi), _MM_SHUFFLE(3, 1, 3, 1));
    return _mm_add_epi32(_mm_castps_si128(even), _mm_castps_si128(odd));
}

// Luma of 4 BGRA pixels, as 32-bit lanes.
inline __m128i Luma4(__m128i pixels) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i k = _mm_setr_epi16(296, 2929, 871, 0, 296, 2929, 871, 0);
    const __m128i lo = _mm_madd_epi16(_mm_unpacklo_epi8(pixels, zero), k);
    const __m128i hi = _mm_madd_epi16(_mm_unpackhi_epi8(pixels, zero), k);
    return _mm_srai_epi32(_mm_add_epi32(PairSums(lo, hi), _mm_set1_epi32(2048)), 12);
}

// U, V, U, V (32-bit lanes) of the two 2x2 blocks covered by 4 pixels of two rows.
inline __m128i Chroma4(__m128i row0, __m128i row1) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i lo = _mm_add_epi16(_mm_unpacklo_epi8(row0, zero), _mm_unpacklo_epi8(row1, zero));
    const __m128i hi = _mm_add_epi16(_mm_unpackhi_epi8(row0, zero), _mm_unpackhi_epi8(row1, zero));
    const __m128i blocks = _mm_unpacklo_epi64(_mm_add_epi16(lo, _mm_srli_si128(lo, 8)), _mm_add_epi16(hi, _mm_srli_si128(hi, 8)));
    const __m128i u = _mm_madd_epi16(blocks, _mm_setr_epi16(2048, -1579, -469, 0, 2048, -1579, -469, 0));
    const __m128i v = _mm_madd_epi16(blocks, _mm_setr_epi16(-188, -1860, 2048, 0, -188, -1860, 2048, 0));
    const __m128i uuvv = _mm_srai_epi32(_mm_add_epi32(PairSums(u, v), _mm_set1_epi32((128 << 14) + (1 << 13))), 14);
    return _mm_shuffle_epi32(uuvv, _MM_SHUFFLE(3, 1, 2, 0));
}

} // namespace

void BgraToNv12(const uint8_t* bgra, ptrdiff_t stride, uint32_t width, uint32_t height,
                uint8_t* nv12, uint32_t coded_width, uint32_t coded_height) {
    uint8_t* const luma = nv12;
    uint8_t* const chroma = nv12 + static_cast<size_t>(coded_width) * coded_height;
    const uint32_t even_width = width & ~1u;
    for (uint32_t y = 0; y < coded_height; y += 2) {
        const uint8_t* row0 = bgra + static_cast<ptrdiff_t>(std::min(y, height - 1)) * stride;
        const uint8_t* row1 = bgra + static_cast<ptrdiff_t>(std::min(y + 1, height - 1)) * stride;
        uint8_t* l0 = luma + static_cast<size_t>(y) * coded_width;
        uint8_t* l1 = l0 + coded_width;
        uint8_t* c = chroma + static_cast<size_t>(y / 2) * coded_width;
        uint32_t x = 0;
        for (; x + 8 <= width; x += 8) { // 8 pixels of both rows per step
            const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row0 + x * 4));
            const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row0 + x * 4 + 16));
            const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row1 + x * 4));
            const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row1 + x * 4 + 16));
            const __m128i ya = _mm_packs_epi32(Luma4(a0), Luma4(a1));
            const __m128i yb = _mm_packs_epi32(Luma4(b0), Luma4(b1));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(l0 + x), _mm_packus_epi16(ya, ya));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(l1 + x), _mm_packus_epi16(yb, yb));
            const __m128i uv = _mm_packs_epi32(Chroma4(a0, b0), Chroma4(a1, b1));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(c + x), _mm_packus_epi16(uv, uv)); // 256 saturates to 255
        }
        for (; x < even_width; x += 2) {
            const uint8_t* a = row0 + x * 4;
            const uint8_t* b = row1 + x * 4;
            l0[x] = Luma(a[2], a[1], a[0]);
            l0[x + 1] = Luma(a[6], a[5], a[4]);
            l1[x] = Luma(b[2], b[1], b[0]);
            l1[x + 1] = Luma(b[6], b[5], b[4]);
            const int r4 = a[2] + a[6] + b[2] + b[6];
            const int g4 = a[1] + a[5] + b[1] + b[5];
            const int b4 = a[0] + a[4] + b[0] + b[4];
            c[x] = ChromaU(r4, g4, b4);
            c[x + 1] = ChromaV(r4, g4, b4);
        }
        for (; x < coded_width; x += 2) { // padding columns repeat the last pixel
            const uint8_t* a0 = row0 + std::min(x, width - 1) * 4;
            const uint8_t* a1 = row0 + std::min(x + 1, width - 1) * 4;
            const uint8_t* b0 = row1 + std::min(x, width - 1) * 4;
            const uint8_t* b1 = row1 + std::min(x + 1, width - 1) * 4;
            l0[x] = Luma(a0[2], a0[1], a0[0]);
            l0[x + 1] = Luma(a1[2], a1[1], a1[0]);
            l1[x] = Luma(b0[2], b0[1], b0[0]);
            l1[x + 1] = Luma(b1[2], b1[1], b1[0]);
            const int r4 = a0[2] + a1[2] + b0[2] + b1[2];
            const int g4 = a0[1] + a1[1] + b0[1] + b1[1];
            const int b4 = a0[0] + a1[0] + b0[0] + b1[0];
            c[x] = ChromaU(r4, g4, b4);
            c[x + 1] = ChromaV(r4, g4, b4);
        }
    }
}

void Nv12ToBgra(const uint8_t* luma, const uint8_t* chroma, size_t pitch, uint32_t width, uint32_t height,
                uint8_t* bgra) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i bias = _mm_set1_epi16(128);
    const __m128i opaque = _mm_set1_epi8(static_cast<char>(0xFF));
    const __m128i rv = _mm_set1_epi16(1613), gu = _mm_set1_epi16(192), gv = _mm_set1_epi16(479),
                  bu = _mm_set1_epi16(1900);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* l = luma + static_cast<size_t>(y) * pitch;
        const uint8_t* c = chroma + static_cast<size_t>(y / 2) * pitch;
        uint8_t* out = bgra + static_cast<size_t>(y) * width * 4;
        uint32_t x = 0;
        for (; x + 8 <= width; x += 8) { // 8 pixels and their 4 chroma pairs per step
            const __m128i y16 = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(l + x)), zero);
            __m128i c16 = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(c + x)), zero);
            c16 = _mm_slli_epi16(_mm_sub_epi16(c16, bias), 6);
            const __m128i u = _mm_shufflehi_epi16(_mm_shufflelo_epi16(c16, _MM_SHUFFLE(2, 2, 0, 0)), _MM_SHUFFLE(2, 2, 0, 0));
            const __m128i v = _mm_shufflehi_epi16(_mm_shufflelo_epi16(c16, _MM_SHUFFLE(3, 3, 1, 1)), _MM_SHUFFLE(3, 3, 1, 1));
            const __m128i r = _mm_add_epi16(y16, _mm_mulhi_epi16(v, rv));
            const __m128i g = _mm_sub_epi16(_mm_sub_epi16(y16, _mm_mulhi_epi16(u, gu)), _mm_mulhi_epi16(v, gv));
            const __m128i b = _mm_add_epi16(y16, _mm_mulhi_epi16(u, bu));
            const __m128i bg = _mm_unpacklo_epi8(_mm_packus_epi16(b, b), _mm_packus_epi16(g, g));
            const __m128i ra = _mm_unpacklo_epi8(_mm_packus_epi16(r, r), opaque);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x * 4), _mm_unpacklo_epi16(bg, ra));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x * 4 + 16), _mm_unpackhi_epi16(bg, ra));
        }
        for (; x < width; ++x) {
            const uint32_t pair = x & ~1u;
            YuvToBgra(l[x], (c[pair] - 128) * 64, (c[pair + 1] - 128) * 64, out + x * 4);
        }
    }
}

// =============================================================================
// Encoder
// =============================================================================

struct H264Encoder::Impl {
    MfScope scope;
    IMFActivate* activate = nullptr;
    IMFTransform* mft = nullptr;
    IMFMediaEventGenerator* events = nullptr; // asynchronous (hardware) encoders only
    IMFSample* output = nullptr;              // our output sample, for encoders that do not provide one
    bool providesSamples = false;
    DWORD outputSize = 0;
    uint32_t inputRequests = 0;               // METransformNeedInput events not answered yet
    uint32_t outputs = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t codedWidth = 0;
    uint32_t codedHeight = 0;
    LONGLONG frameIndex = 0;
    std::vector<uint8_t> parameterSets;       // SPS/PPS, for encoders that keep them out of band
    std::string name;

    bool Configure(uint32_t bitrate_kbps);
    HRESULT PullOutput(std::vector<uint8_t>& out, bool& key);
    bool DrainSync(std::vector<uint8_t>& out, bool& key);
    bool PumpEvents(std::vector<uint8_t>& out, bool& key, bool want_output, int timeout_ms);
    void Release();
};

bool H264Encoder::Impl::Configure(uint32_t bitrate_kbps) {
    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED(mft->GetAttributes(&attrs))) {
        UINT32 async = FALSE;
        const bool is_async = SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async;
        const bool unlocked = !is_async || SUCCEEDED(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
        attrs->Release();
        if (!unlocked) return false;
        if (is_async && FAILED(mft->QueryInterface(IID_PPV_ARGS(&events)))) return false;
    }

    ICodecAPI* codec = nullptr;
    if (SUCCEEDED(mft->QueryInterface(kIidCodecApi, reinterpret_cast<void**>(&codec)))) {
        SetCodecUInt(codec, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        SetCodecUInt(codec, CODECAPI_AVEncCommonMeanBitRate, bitrate_kbps * 1000);
        SetCodecUInt(codec, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        SetCodecUInt(codec, CODECAPI_AVEncMPVGOPSize, kGopFrames);
        SetCodecBool(codec, CODECAPI_AVLowLatencyMode, true);
        codec->Release();
    }

    bool typed = false;
    for (UINT32 profile : {eAVEncH264VProfile_High, eAVEncH264VProfile_Main, eAVEncH264VProfile_Base}) {
        IMFMediaType* t = NewVideoType(MFVideoFormat_H264, codedWidth, codedHeight);
        if (!t) return false;
        t->SetUINT32(MF_MT_AVG_BITRATE, bitrate_kbps * 1000);
        t->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
        typed = SUCCEEDED(mft->SetOutputType(0, t, 0));
        t->Release();
        if (typed) break;
    }
    if (!typed) return false;

    // NV12 input, preferably as the encoder itself describes it.
    typed = false;
    for (DWORD i = 0; !typed; ++i) {
        IMFMediaType* t = nullptr;
        if (FAILED(mft->GetInputAvailableType(0, i, &t))) break;
        GUID subtype = {};
        if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
            SetVideoAttributes(t, codedWidth, codedHeight);
            typed = SUCCEEDED(mft->SetInputType(0, t, 0));
        }
        t->Release();
    }
    if (!typed) {
        IMFMediaType* t = NewVideoType(MFVideoFormat_NV12, codedWidth, codedHeight);
        typed = t && SUCCEEDED(mft->SetInputType(0, t, 0));
        SafeRelease(t);
    }
    if (!typed) return false;

    MFT_OUTPUT_STREAM_INFO info = {};
    if (FAILED(mft->GetOutputStreamInfo(0, &info))) return false;
    providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outputSize = info.cbSize;
    return SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) &&
           SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
}

// One ProcessOutput call: S_OK with one access unit appended, S_FALSE after a renegotiated output
// type, MF_E_TRANSFORM_NEED_MORE_INPUT, or a failure.
HRESULT H264Encoder::Impl::PullOutput(std::vector<uint8_t>& out, bool& key) {
    if (!providesSamples && !output) {
        output = NewSample(std::max<DWORD>(outputSize, codedWidth * codedHeight * 2));
        if (!output) return E_OUTOFMEMORY;
    }
    if (output) {
        IMFMediaBuffer* b = nullptr;
        if (SUCCEEDED(output->GetBufferByIndex(0, &b))) {
            b->SetCurrentLength(0);
            b->Release();
        }
    }
    MFT_OUTPUT_DATA_BUFFER data = {};
    data.pSample = providesSamples ? nullptr : output;
    DWORD status = 0;
    HRESULT hr = mft->ProcessOutput(0, 1, &data, &status);
    SafeRelease(data.pEvents);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        IMFMediaType* t = nullptr;
        hr = mft->GetOutputAvailableType(0, 0, &t);
        if (SUCCEEDED(hr)) hr = mft->SetOutputType(0, t, 0);
        SafeRelease(t);
        return SUCCEEDED(hr) ? S_FALSE : hr;
    }
    if (FAILED(hr)) return hr;

    IMFMediaBuffer* buffer = nullptr;
    if (data.pSample && SUCCEEDED(data.pSample->ConvertToContiguousBuffer(&buffer))) {
        BYTE* bytes = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer->Lock(&bytes, nullptr, &length))) {
            const uint32_t types = NalTypes(bytes, length);
            const bool idr = (types & kNalIdr) != 0;
            const bool prepend = idr && !(types & kNalSps); // the decoder needs SPS/PPS before an IDR
            if (prepend && parameterSets.empty()) {
                IMFMediaType* t = nullptr;
                UINT32 size = 0;
                if (SUCCEEDED(mft->GetOutputCurrentType(0, &t)) &&
                    SUCCEEDED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
                    parameterSets.resize(size);
                    if (FAILED(t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, parameterSets.data(), size, nullptr))) {
                        parameterSets.clear();
                    }
                }
                SafeRelease(t);
            }
            PutUnit(out, prepend ? parameterSets.data() : nullptr, prepend ? parameterSets.size() : 0, bytes, length);
            key = key || idr;
            ++outputs;
            buffer->Unlock();
        }
        buffer->Release();
    }
    if (data.pSample != output) SafeRelease(data.pSample); // the encoder's own sample
    return S_OK;
}

// Synchronous encoders: pulls access units until the encoder needs more input.
bool H264Encoder::Impl::DrainSync(std::vector<uint8_t>& out, bool& key) {
    for (;;) {
        const HRESULT hr = PullOutput(out, key);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (FAILED(hr)) return false;
    }
}

// Handles the asynchronous encoder's events until it asks for input (or, with `want_output`,
// until it delivers an access unit) or `timeout_ms` passes. False only on an encoder error.
bool H264Encoder::Impl::PumpEvents(std::vector<uint8_t>& out, bool& key, bool want_output, int timeout_ms) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    const uint32_t outputs_before = outputs;
    for (;;) {
        if (want_output ? outputs != outputs_before : inputRequests > 0) return true;
        IMFMediaEvent* event = nullptr;
        const HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            if (Clock::now() >= deadline) return true;
            SwitchToThread();
            continue;
        }
        if (FAILED(hr)) return false;
        MediaEventType type = MEUnknown;
        HRESULT status = S_OK;
        event->GetType(&type);
        event->GetStatus(&status);
        event->Release();
        if (FAILED(status) || type == MEError) return false;
        if (type == METransformNeedInput) {
            ++inputRequests;
        } else if (type == METransformHaveOutput) {
            if (FAILED(PullOutput(out, key))) return false;
        }
    }
}

void H264Encoder::Impl::Release() {
    if (mft) {
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    SafeRelease(output);
    SafeRelease(events);
    SafeRelease(mft);
    if (activate) {
        activate->ShutdownObject();
        SafeRelease(activate);
    }
    inputRequests = outputs = 0;
    parameterSets.clear();
}

H264Encoder::H264Encoder() : m(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() { Destroy(); }

bool H264Encoder::IsValid() const noexcept { return m->mft != nullptr; }
uint32_t H264Encoder::Width() const noexcept { return m->width; }
uint32_t H264Encoder::Height() const noexcept { return m->height; }
const std::string& H264Encoder::Name() const noexcept { return m->name; }

void H264Encoder::Destroy() {
    const bool started = m->mft != nullptr;
    m->Release();
    if (started) m->scope.Stop();
    m->width = m->height = m->codedWidth = m->codedHeight = 0;
}

bool H264Encoder::Create(uint32_t width, uint32_t height, uint32_t bitrate_kbps) {
    Destroy();
    if (width == 0 || height == 0 || width > 4096 || height > 4096 || !m->scope.Start()) return false;
    m->width = width;
    m->height = height;
    m->codedWidth = CodedSize(width);
    m->codedHeight = CodedSize(height);
    m->frameIndex = 0;

    const MFT_REGISTER_TYPE_INFO input = {MFMediaType_Video, MFVideoFormat_NV12};
    const MFT_REGISTER_TYPE_INFO output = {MFMediaType_Video, MFVideoFormat_H264};
    const bool software_only = EnvVar("RGPU_VIDEO_ENCODER") == "software";
    for (UINT32 flags : {static_cast<UINT32>(MFT_ENUM_FLAG_HARDWARE), static_cast<UINT32>(MFT_ENUM_FLAG_SYNCMFT)}) {
        if (m->mft || (software_only && flags == MFT_ENUM_FLAG_HARDWARE)) continue;
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        if (FAILED(Mf()->TEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags | MFT_ENUM_FLAG_SORTANDFILTER, &input, &output,
                                 &activates, &count))) {
            continue;
        }
        for (UINT32 i = 0; i < count; ++i) {
            if (!m->mft && SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&m->mft)))) {
                m->activate = activates[i];
                m->activate->AddRef();
                m->name = FriendlyName(m->activate);
                if (!m->Configure(bitrate_kbps)) m->Release(); // try the next encoder
            }
            activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
    if (!m->mft) {
        Destroy();
        m->scope.Stop();
        return false;
    }
    return true;
}

bool H264Encoder::Encode(const uint8_t* bgra, ptrdiff_t stride, std::vector<uint8_t>& out, bool& key) {
    key = false;
    if (!m->mft) return false;
    const DWORD size = m->codedWidth * m->codedHeight * 3 / 2;
    IMFSample* sample = NewSample(size);
    if (!sample) return false;
    IMFMediaBuffer* buffer = nullptr;
    BYTE* nv12 = nullptr;
    bool filled = false;
    if (SUCCEEDED(sample->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer->Lock(&nv12, nullptr, nullptr))) {
        BgraToNv12(bgra, stride, m->width, m->height, nv12, m->codedWidth, m->codedHeight);
        buffer->Unlock();
        filled = SUCCEEDED(buffer->SetCurrentLength(size));
    }
    SafeRelease(buffer);
    sample->SetSampleTime(m->frameIndex * kFrameDuration);
    sample->SetSampleDuration(kFrameDuration);
    ++m->frameIndex;

    bool ok = filled;
    if (ok && m->events) {
        // Asynchronous: input only when asked for, then wait briefly for this frame's output.
        ok = m->PumpEvents(out, key, false, kInputTimeoutMs) && m->inputRequests > 0 &&
             SUCCEEDED(m->mft->ProcessInput(0, sample, 0));
        if (ok) {
            --m->inputRequests;
            ok = m->PumpEvents(out, key, true, kOutputTimeoutMs);
        }
    } else if (ok) {
        HRESULT hr = m->mft->ProcessInput(0, sample, 0);
        if (hr == MF_E_NOTACCEPTING) { // output still queued: take it, then retry
            ok = m->DrainSync(out, key);
            hr = ok ? m->mft->ProcessInput(0, sample, 0) : hr;
        }
        ok = ok && SUCCEEDED(hr) && m->DrainSync(out, key);
    }
    sample->Release();
    return ok;
}

// =============================================================================
// Decoder
// =============================================================================

struct H264Decoder::Impl {
    MfScope scope;
    IMFTransform* mft = nullptr;
    IMFSample* output = nullptr; // our output sample, for a decoder that does not provide one
    bool providesSamples = false;
    DWORD outputSize = 0;
    uint32_t frameWidth = 0;     // decoded picture buffer, from the output type
    uint32_t frameHeight = 0;
    uint32_t stride = 0;

    // Hardware decoding (DXVA): pictures are D3D11 surfaces, copied to `staging` to be read.
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IMFDXGIDeviceManager* manager = nullptr;
    ID3D11Texture2D* staging = nullptr;
    D3D11_TEXTURE2D_DESC stagingDesc = {};

    bool EnableHardware();
    void ReleaseHardware();
    bool SelectOutputType();
    bool ReadSurface(IMFDXGIBuffer* surface, uint32_t width, uint32_t height, uint8_t* bgra);
    bool ConvertPicture(IMFSample* sample, uint32_t width, uint32_t height, uint8_t* bgra);
    bool Drain(uint32_t width, uint32_t height, uint8_t* bgra, bool& picture);
};

bool H264Decoder::Impl::EnableHardware() {
    const auto create = D3D11Create();
    IMFAttributes* attrs = nullptr;
    UINT32 aware = FALSE;
    if (SUCCEEDED(mft->GetAttributes(&attrs))) {
        attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware);
        attrs->Release();
    }
    if (!create || !Mf()->CreateDXGIDeviceManager || !aware) return false;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
    if (FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                      static_cast<UINT>(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION, &device, nullptr, &context))) {
        return false;
    }
    // The decoder uses the device from its own threads as well.
    ID3D10Multithread* multithread = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }
    UINT token = 0;
    const bool ok = SUCCEEDED(Mf()->CreateDXGIDeviceManager(&token, &manager)) &&
                    SUCCEEDED(manager->ResetDevice(device, token)) &&
                    SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager)));
    if (!ok) ReleaseHardware();
    return ok;
}

void H264Decoder::Impl::ReleaseHardware() {
    SafeRelease(staging);
    SafeRelease(manager);
    SafeRelease(context);
    SafeRelease(device);
    stagingDesc = {};
}

// Copies a decoded D3D11 surface to the CPU and converts its top-left width x height.
bool H264Decoder::Impl::ReadSurface(IMFDXGIBuffer* surface, uint32_t width, uint32_t height, uint8_t* bgra) {
    ID3D11Texture2D* texture = nullptr;
    UINT index = 0;
    if (!context || FAILED(surface->GetResource(IID_PPV_ARGS(&texture))) || FAILED(surface->GetSubresourceIndex(&index))) {
        SafeRelease(texture);
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    bool ok = desc.Format == DXGI_FORMAT_NV12 && width <= desc.Width && height <= desc.Height;
    if (ok && (!staging || stagingDesc.Width != desc.Width || stagingDesc.Height != desc.Height)) {
        SafeRelease(staging);
        stagingDesc = desc;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        ok = SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging));
    }
    if (ok) {
        context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, index, nullptr);
        D3D11_MAPPED_SUBRESOURCE map = {};
        ok = SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map));
        if (ok) {
            const auto* luma = static_cast<const uint8_t*>(map.pData);
            Nv12ToBgra(luma, luma + static_cast<size_t>(map.RowPitch) * stagingDesc.Height, map.RowPitch, width, height, bgra);
            context->Unmap(staging, 0);
        }
    }
    texture->Release();
    return ok;
}

bool H264Decoder::Impl::SelectOutputType() {
    bool selected = false;
    for (DWORD i = 0; !selected; ++i) {
        IMFMediaType* t = nullptr;
        if (FAILED(mft->GetOutputAvailableType(0, i, &t))) return false;
        GUID subtype = {};
        if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12 &&
            SUCCEEDED(mft->SetOutputType(0, t, 0))) {
            UINT32 w = 0, h = 0, s = 0;
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
            frameWidth = w;
            frameHeight = h;
            stride = (SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) && static_cast<INT32>(s) >= static_cast<INT32>(w))
                         ? s : w;
            selected = true;
        }
        t->Release();
    }
    MFT_OUTPUT_STREAM_INFO info = {};
    if (FAILED(mft->GetOutputStreamInfo(0, &info))) return false;
    providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outputSize = info.cbSize;
    SafeRelease(output); // reallocated at the new size
    return true;
}

bool H264Decoder::Impl::ConvertPicture(IMFSample* sample, uint32_t width, uint32_t height, uint8_t* bgra) {
    IMFMediaBuffer* buffer = nullptr;
    if (!sample || width > frameWidth || height > frameHeight) return false;
    if (device && SUCCEEDED(sample->GetBufferByIndex(0, &buffer))) {
        IMFDXGIBuffer* surface = nullptr;
        const bool read = SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&surface))) && ReadSurface(surface, width, height, bgra);
        SafeRelease(surface);
        SafeRelease(buffer);
        if (read) return true;
    }
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
    bool ok = false;
    IMF2DBuffer* buffer2d = nullptr;
    BYTE* bytes = nullptr;
    DWORD length = 0;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d)))) {
        LONG pitch = 0;
        if (SUCCEEDED(buffer2d->GetContiguousLength(&length)) && SUCCEEDED(buffer2d->Lock2D(&bytes, &pitch))) {
            const size_t plane = static_cast<size_t>(pitch) * frameHeight;
            if (pitch >= static_cast<LONG>(width) && length >= plane * 3 / 2) {
                Nv12ToBgra(bytes, bytes + plane, static_cast<size_t>(pitch), width, height, bgra);
                ok = true;
            }
            buffer2d->Unlock2D();
        }
        buffer2d->Release();
    } else if (SUCCEEDED(buffer->Lock(&bytes, nullptr, &length))) {
        const size_t plane = static_cast<size_t>(stride) * frameHeight;
        if (length >= plane * 3 / 2) {
            Nv12ToBgra(bytes, bytes + plane, stride, width, height, bgra);
            ok = true;
        }
        buffer->Unlock();
    }
    buffer->Release();
    return ok;
}

bool H264Decoder::Impl::Drain(uint32_t width, uint32_t height, uint8_t* bgra, bool& picture) {
    for (;;) {
        if (!providesSamples && !output) {
            output = NewSample(std::max<DWORD>(outputSize, stride * frameHeight * 3 / 2));
            if (!output) return false;
        }
        if (output) {
            IMFMediaBuffer* b = nullptr;
            if (SUCCEEDED(output->GetBufferByIndex(0, &b))) {
                b->SetCurrentLength(0);
                b->Release();
            }
        }
        MFT_OUTPUT_DATA_BUFFER data = {};
        data.pSample = providesSamples ? nullptr : output;
        DWORD status = 0;
        const HRESULT hr = mft->ProcessOutput(0, 1, &data, &status);
        SafeRelease(data.pEvents);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!SelectOutputType()) return false;
            continue;
        }
        if (FAILED(hr)) return false;
        const bool converted = ConvertPicture(data.pSample, width, height, bgra);
        if (data.pSample != output) SafeRelease(data.pSample);
        if (!converted) return false;
        picture = true;
    }
}

H264Decoder::H264Decoder() : m(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() { Destroy(); }

bool H264Decoder::IsValid() const noexcept { return m->mft != nullptr; }

bool H264Decoder::IsHardware() const noexcept { return m->device != nullptr; }

void H264Decoder::Destroy() {
    if (!m->mft) return;
    m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    SafeRelease(m->output);
    SafeRelease(m->mft);
    m->ReleaseHardware();
    m->scope.Stop();
    m->frameWidth = m->frameHeight = m->stride = 0;
}

bool H264Decoder::Create() {
    Destroy();
    if (!m->scope.Start()) return false;
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m->mft)))) {
        m->scope.Stop();
        return false;
    }
    // Low latency: each access unit comes out as soon as it is decoded (there are no B-frames).
    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED(m->mft->GetAttributes(&attrs))) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        attrs->Release();
    }
    ICodecAPI* codec = nullptr;
    if (SUCCEEDED(m->mft->QueryInterface(kIidCodecApi, reinterpret_cast<void**>(&codec)))) {
        SetCodecBool(codec, CODECAPI_AVLowLatencyMode, true);
        codec->Release();
    }
    // The GPU's decoder when there is one (a software 1080p decode costs a laptop CPU ~35 ms).
    if (EnvVar("RGPU_VIDEO_DECODER") != "software") m->EnableHardware();
    IMFMediaType* input = nullptr;
    bool ok = SUCCEEDED(Mf()->CreateMediaType(&input));
    if (ok) {
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ok = SUCCEEDED(m->mft->SetInputType(0, input, 0));
        input->Release();
    }
    ok = ok && m->SelectOutputType() && SUCCEEDED(m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) &&
         SUCCEEDED(m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    if (!ok) Destroy();
    return ok;
}

bool H264Decoder::Decode(const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint8_t* bgra,
                         bool& picture) {
    picture = false;
    if (!m->mft || size == 0 || size > 0x7FFFFFFF) return false;
    IMFSample* sample = NewSample(static_cast<DWORD>(size));
    if (!sample) return false;
    IMFMediaBuffer* buffer = nullptr;
    BYTE* bytes = nullptr;
    bool ok = SUCCEEDED(sample->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer->Lock(&bytes, nullptr, nullptr));
    if (ok) {
        std::memcpy(bytes, data, size);
        buffer->Unlock();
        ok = SUCCEEDED(buffer->SetCurrentLength(static_cast<DWORD>(size)));
    }
    SafeRelease(buffer);
    if (ok) {
        HRESULT hr = m->mft->ProcessInput(0, sample, 0);
        if (hr == MF_E_NOTACCEPTING) { // pictures still queued: take them, then retry
            ok = m->Drain(width, height, bgra, picture);
            hr = ok ? m->mft->ProcessInput(0, sample, 0) : hr;
        }
        ok = ok && SUCCEEDED(hr) && m->Drain(width, height, bgra, picture);
    }
    sample->Release();
    return ok;
}

} // namespace Video
} // namespace RemoteGPU
