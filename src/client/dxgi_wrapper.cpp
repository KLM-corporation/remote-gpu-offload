// Drop-in dxgi.dll: CreateDXGIFactory/1/2 return the RemoteGPU proxy factory, so applications
// that create their swap chain from a factory they made themselves also present remote frames.
// The factory itself lives in the proxy d3d11.dll, loaded from this DLL's own directory (never
// the system one), so devices and factories always come from the same module.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>

#include <cwchar>

namespace {

using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);

CreateFactoryFn ResolveProxyFactory() {
    static CreateFactoryFn fn = [] () -> CreateFactoryFn {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&ResolveProxyFactory), &self)) {
            return nullptr;
        }
        wchar_t path[MAX_PATH];
        DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return nullptr;
        wchar_t* slash = std::wcsrchr(path, L'\\');
        if (!slash || (slash - path) + 11 >= MAX_PATH) return nullptr;
        std::wcscpy(slash + 1, L"d3d11.dll");
        HMODULE proxy = LoadLibraryW(path);
        return proxy ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(proxy, "RgpuCreateDXGIFactory")) : nullptr;
    }();
    return fn;
}

HRESULT CreateFactory(REFIID riid, void** ppFactory) {
    if (!ppFactory) return E_INVALIDARG;
    *ppFactory = nullptr;
    CreateFactoryFn fn = ResolveProxyFactory();
    return fn ? fn(riid, ppFactory) : DXGI_ERROR_UNSUPPORTED;
}

} // namespace

extern "C" {

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) { return CreateFactory(riid, ppFactory); }
HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) { return CreateFactory(riid, ppFactory); }
HRESULT WINAPI CreateDXGIFactory2(UINT, REFIID riid, void** ppFactory) { return CreateFactory(riid, ppFactory); }

HRESULT WINAPI DXGIGetDebugInterface1(UINT, REFIID, void** ppDebug) {
    if (ppDebug) *ppDebug = nullptr;
    return E_NOINTERFACE;
}

HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() { return S_OK; }

} // extern "C"
