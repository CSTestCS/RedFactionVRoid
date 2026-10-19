// Proxy DLL plumbing. The same binary is installed next to rfg.exe as d3d11.dll (which the
// game is guaranteed to load) and as dxgi.dll. Every export forwards to the real system
// DLL. The device / swap-chain factory functions are wrapped so the mod can hook the
// game's actual D3D11 objects the moment they are created.
#include "proxy.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstdlib>
#include <string>

#include "../core/log.hpp"
#include "d3d_hooks.hpp"
#include "paths.hpp"

#define PROXY(idx, dll, name)  // only PROXY_COUNT is needed here
#include "proxy_export_list.h"
#undef PROXY

extern "C" {
extern void* rfv_real[PROXY_COUNT];
void* rfv_resolve_export(unsigned index);
HRESULT WINAPI rfv_proxy_not_implemented() { return E_NOTIMPL; }
}

namespace rfv::win {

namespace {

enum Kind { DXGI, D3D11 };
struct ExportInfo {
    Kind kind;
    const char* name;
};
#define PROXY(idx, dll, name) {dll, #name},
const ExportInfo kExports[PROXY_COUNT] = {
#include "proxy_export_list.h"
};
#undef PROXY

HMODULE g_self = nullptr;
std::atomic<HMODULE> g_real[2] = {nullptr, nullptr};
std::atomic<bool> g_active{false};

std::wstring moduleFileName(HMODULE m) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(m, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}

// Loads the real DLL for `kind`: a chained mod (e.g. ReShade renamed to dxgi_chain.dll) if
// present, otherwise the system copy. Never returns this module.
HMODULE realModule(Kind kind) {
    HMODULE m = g_real[kind].load();
    if (m) return m;
    const wchar_t* base = kind == DXGI ? L"dxgi" : L"d3d11";
    std::wstring chain = gameDirectoryW() + L"\\" + base + L"_chain.dll";
    if (GetFileAttributesW(chain.c_str()) != INVALID_FILE_ATTRIBUTES) {
        m = LoadLibraryW(chain.c_str());
        if (m == g_self) m = nullptr;
    }
    if (!m) {
        wchar_t sys[MAX_PATH];
        UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n > 0 && n < MAX_PATH) m = LoadLibraryW((std::wstring(sys) + L"\\" + base + L".dll").c_str());
    }
    if (m) g_real[kind] = m;
    else RFV_LOG("ERROR: could not load the real %ls.dll", base);
    return m;
}

void* realExport(Kind kind, const char* name) {
    HMODULE m = realModule(kind);
    return m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
}

} // namespace

bool proxyIsActiveInstance() { return g_active; }

// When this copy only forwards, hand created objects to the copy that runs the mod. The
// active copy publishes its module handle in a per-process environment variable.
using HookFn = void(WINAPI*)(IUnknown*);
HookFn activeHook(const char* exportName) {
    wchar_t buf[32];
    if (!GetEnvironmentVariableW(L"RFVROID_ACTIVE_MODULE", buf, 32)) return nullptr;
    HMODULE m = reinterpret_cast<HMODULE>(static_cast<uintptr_t>(wcstoull(buf, nullptr, 16)));
    if (!m || m == g_self) return nullptr;
    return reinterpret_cast<HookFn>(GetProcAddress(m, exportName));
}

bool proxyAttach(HMODULE self) {
    g_self = self;
    // Several copies of this DLL can be loaded into the game (d3d11.dll and dxgi.dll). Only
    // the first one runs the mod; the others just forward calls.
    wchar_t name[64];
    wsprintfW(name, L"Local\\RFVRoid_%lu", GetCurrentProcessId());
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
    bool first = mutex && GetLastError() != ERROR_ALREADY_EXISTS;
    if (!first && mutex) CloseHandle(mutex);  // keep the first instance's handle open forever
    g_active = first;
    if (first) {
        wchar_t buf[32];
        wsprintfW(buf, L"%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(self)));
        SetEnvironmentVariableW(L"RFVROID_ACTIVE_MODULE", buf);
    }
    return first;
}

std::wstring proxySelfPath() { return moduleFileName(g_self); }

} // namespace rfv::win

using namespace rfv::win;

extern "C" void* rfv_resolve_export(unsigned index) {
    void* p = nullptr;
    if (index < PROXY_COUNT) p = realExport(kExports[index].kind, kExports[index].name);
    if (!p) p = reinterpret_cast<void*>(&rfv_proxy_not_implemented);
    if (index < PROXY_COUNT) rfv_real[index] = p;
    return p;
}

// ---- Hand-over entry points (called across copies of this DLL) --------------------------

extern "C" __declspec(dllexport) void WINAPI RFVRoid_HookFactory(IUnknown* f) { hookFactory(f); }
extern "C" __declspec(dllexport) void WINAPI RFVRoid_HookDevice(IUnknown* d) {
    ID3D11Device* dev = nullptr;
    if (d && SUCCEEDED(d->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev)))) {
        hookCreatedDevice(dev);
        dev->Release();
    }
}
extern "C" __declspec(dllexport) void WINAPI RFVRoid_HookSwapChain(IUnknown* s) {
    IDXGISwapChain* sc = nullptr;
    if (s && SUCCEEDED(s->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&sc)))) {
        hookCreatedSwapChain(sc);
        sc->Release();
    }
}

namespace {
void notify(const char* exportName, void (*local)(IUnknown*), IUnknown* obj) {
    if (!obj) return;
    if (proxyIsActiveInstance()) local(obj);
    else if (auto fn = rfv::win::activeHook(exportName)) fn(obj);
}
void localFactory(IUnknown* o) { RFVRoid_HookFactory(o); }
void localDevice(IUnknown* o) { RFVRoid_HookDevice(o); }
void localSwapChain(IUnknown* o) { RFVRoid_HookSwapChain(o); }
} // namespace

// ---- Wrapped exports --------------------------------------------------------------------

extern "C" HRESULT WINAPI rfv_CreateDXGIFactory(REFIID riid, void** out) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    static Fn real = reinterpret_cast<Fn>(realExport(DXGI, "CreateDXGIFactory"));
    if (!real) return E_NOTIMPL;
    HRESULT hr = real(riid, out);
    if (SUCCEEDED(hr) && out && *out) notify("RFVRoid_HookFactory", localFactory, static_cast<IUnknown*>(*out));
    return hr;
}

extern "C" HRESULT WINAPI rfv_CreateDXGIFactory1(REFIID riid, void** out) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    static Fn real = reinterpret_cast<Fn>(realExport(DXGI, "CreateDXGIFactory1"));
    if (!real) return E_NOTIMPL;
    HRESULT hr = real(riid, out);
    if (SUCCEEDED(hr) && out && *out) notify("RFVRoid_HookFactory", localFactory, static_cast<IUnknown*>(*out));
    return hr;
}

extern "C" HRESULT WINAPI rfv_CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    static Fn real = reinterpret_cast<Fn>(realExport(DXGI, "CreateDXGIFactory2"));
    if (!real) return E_NOTIMPL;
    HRESULT hr = real(flags, riid, out);
    if (SUCCEEDED(hr) && out && *out) notify("RFVRoid_HookFactory", localFactory, static_cast<IUnknown*>(*out));
    return hr;
}

extern "C" HRESULT WINAPI rfv_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                                const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdk, ID3D11Device** device,
                                                D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    static auto real = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(realExport(D3D11, "D3D11CreateDevice"));
    if (!real) return E_NOTIMPL;
    HRESULT hr = real(adapter, type, software, flags, levels, numLevels, sdk, device, level, context);
    if (SUCCEEDED(hr) && device && *device) notify("RFVRoid_HookDevice", localDevice, *device);
    return hr;
}

extern "C" HRESULT WINAPI rfv_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                                            const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdk,
                                                            const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain,
                                                            ID3D11Device** device, D3D_FEATURE_LEVEL* level,
                                                            ID3D11DeviceContext** context) {
    static auto real = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(realExport(D3D11, "D3D11CreateDeviceAndSwapChain"));
    if (!real) return E_NOTIMPL;
    HRESULT hr = real(adapter, type, software, flags, levels, numLevels, sdk, desc, swapChain, device, level, context);
    if (SUCCEEDED(hr)) {
        if (device && *device) notify("RFVRoid_HookDevice", localDevice, *device);
        if (swapChain && *swapChain) notify("RFVRoid_HookSwapChain", localSwapChain, *swapChain);
    }
    return hr;
}
