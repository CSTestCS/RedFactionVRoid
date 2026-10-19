#include "d3d_hooks.hpp"

#include <windows.h>
#include <dxgi1_2.h>
#include <atomic>

#include <atomic>
#include <map>
#include <mutex>
#include <unordered_set>

#include "../core/log.hpp"
#include "vtable_hook.hpp"

namespace rfv::win {

namespace {

// vtable slots
constexpr int kSwapPresent = 8;
constexpr int kSwapResizeBuffers = 13;
constexpr int kSwap1Present1 = 22;
constexpr int kDevCreateInputLayout = 11;
constexpr int kCtxDrawIndexed = 12;
constexpr int kCtxIASetInputLayout = 17;
constexpr int kCtxDrawIndexedInstanced = 20;
constexpr int kCtxOMSetRenderTargets = 33;

VtableHook g_present(kSwapPresent), g_present1(kSwap1Present1), g_resize(kSwapResizeBuffers);
VtableHook g_createLayout(kDevCreateInputLayout), g_drawIndexed(kCtxDrawIndexed),
    g_drawIndexedInst(kCtxDrawIndexedInstanced), g_setLayout(kCtxIASetInputLayout), g_omSet(kCtxOMSetRenderTargets);

std::atomic<PresentListener*> g_listener{nullptr};
std::atomic<unsigned> g_presentCount{0};
std::atomic<bool> g_gameObjectsHooked{false};
thread_local int g_presentDepth = 0;

// --- draw filter state (render thread only, except the layout set)
SRWLOCK g_layoutLock = SRWLOCK_INIT;
std::unordered_set<ID3D11InputLayout*> g_skinnedLayouts;
std::atomic<size_t> g_skinnedCount{0};
bool g_captureDepth = false;
ID3D11InputLayout* g_currentLayout = nullptr;
bool g_currentSkinned = false;
std::set<DrawSig> g_hidden;
bool g_hidingEnabled = false;
bool g_skinnedOnly = true;
bool g_logging = false;
bool g_soloOn = false;
DrawSig g_solo;
std::map<DrawSig, DrawStat> g_frameStats, g_lastStats;
std::mutex g_statsMutex;
bool g_internal = false;

// --- depth capture
ID3D11DepthStencilView* g_capturedDepth = nullptr;
std::mutex g_depthMutex;
UINT g_backW = 0, g_backH = 0;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using CreateLayoutFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawIndexedInstFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using SetLayoutFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11InputLayout*);
using OMSetFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);

void hookGameObjects(IDXGISwapChain* sc);

void beforePresent(IDXGISwapChain* sc) {
    if (g_presentDepth++ > 0) return;
    if (!g_gameObjectsHooked.exchange(true)) hookGameObjects(sc);
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        g_lastStats.swap(g_frameStats);
        g_frameStats.clear();
    }
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(sc->GetDesc(&desc))) { g_backW = desc.BufferDesc.Width; g_backH = desc.BufferDesc.Height; }
    unsigned n = ++g_presentCount;
    if (n == 1) {
        DXGI_SWAP_CHAIN_DESC d{};
        sc->GetDesc(&d);
        RFV_LOG("First Present: %ux%u format %d, %u buffers, swap effect %d, %s", d.BufferDesc.Width, d.BufferDesc.Height,
                static_cast<int>(d.BufferDesc.Format), d.BufferCount, static_cast<int>(d.SwapEffect), d.Windowed ? "windowed" : "fullscreen");
    }
    if (PresentListener* l = g_listener.load()) l->onPresent(sc);
}

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) beforePresent(sc);
    HRESULT hr = g_present.orig<PresentFn>(sc)(sc, sync, flags);
    if (!(flags & DXGI_PRESENT_TEST)) --g_presentDepth;
    return hr;
}

HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!(flags & DXGI_PRESENT_TEST)) beforePresent(sc);
    HRESULT hr = g_present1.orig<Present1Fn>(sc)(sc, sync, flags, p);
    if (!(flags & DXGI_PRESENT_TEST)) --g_presentDepth;
    return hr;
}

HRESULT STDMETHODCALLTYPE hkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    if (PresentListener* l = g_listener.load()) l->onBeforeResize();
    {
        std::lock_guard<std::mutex> lock(g_depthMutex);
        if (g_capturedDepth) { g_capturedDepth->Release(); g_capturedDepth = nullptr; }
    }
    return g_resize.orig<ResizeFn>(sc)(sc, n, w, h, f, flags);
}

HRESULT STDMETHODCALLTYPE hkCreateInputLayout(ID3D11Device* dev, const D3D11_INPUT_ELEMENT_DESC* elems, UINT count,
                                              const void* bytecode, SIZE_T len, ID3D11InputLayout** out) {
    HRESULT hr = g_createLayout.orig<CreateLayoutFn>(dev)(dev, elems, count, bytecode, len, out);
    if (SUCCEEDED(hr) && out && *out && elems) {
        bool skinned = false;
        for (UINT i = 0; i < count && !skinned; ++i)
            if (elems[i].SemanticName && _stricmp(elems[i].SemanticName, "BLENDINDICES") == 0) skinned = true;
        if (skinned) {
            AcquireSRWLockExclusive(&g_layoutLock);
            g_skinnedLayouts.insert(*out);
            g_skinnedCount = g_skinnedLayouts.size();
            ReleaseSRWLockExclusive(&g_layoutLock);
        }
    }
    return hr;
}

void STDMETHODCALLTYPE hkSetInputLayout(ID3D11DeviceContext* ctx, ID3D11InputLayout* layout) {
    if (layout != g_currentLayout) {
        g_currentLayout = layout;
        AcquireSRWLockShared(&g_layoutLock);
        g_currentSkinned = layout && g_skinnedLayouts.count(layout) != 0;
        ReleaseSRWLockShared(&g_layoutLock);
    }
    g_setLayout.orig<SetLayoutFn>(ctx)(ctx, layout);
}

// Returns true if the draw should be skipped.
bool filterDraw(UINT indexCount, UINT startIndex) {
    if (g_internal) return false;
    bool eligible = g_currentSkinned || !g_skinnedOnly || g_skinnedCount.load(std::memory_order_relaxed) == 0;
    if (!eligible) return false;
    DrawSig sig{indexCount, startIndex};
    if (g_logging) {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        DrawStat& s = g_frameStats[sig];
        s.sig = sig;
        s.calls++;
        s.skinned = g_currentSkinned;
    }
    if (g_soloOn && sig == g_solo) return true;
    return g_hidingEnabled && g_hidden.count(sig) != 0;
}

void STDMETHODCALLTYPE hkDrawIndexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base) {
    if (filterDraw(count, start)) return;
    g_drawIndexed.orig<DrawIndexedFn>(ctx)(ctx, count, start, base);
}

void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT count, UINT inst, UINT start, INT base, UINT startInst) {
    if (filterDraw(count, start)) return;
    g_drawIndexedInst.orig<DrawIndexedInstFn>(ctx)(ctx, count, inst, start, base, startInst);
}

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (g_captureDepth && dsv && !g_internal && g_backW) {
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
            D3D11_TEXTURE2D_DESC td;
            tex->GetDesc(&td);
            if (td.Width == g_backW && td.Height == g_backH && td.SampleDesc.Count == 1) {
                std::lock_guard<std::mutex> lock(g_depthMutex);
                if (g_capturedDepth != dsv) {
                    dsv->AddRef();
                    if (g_capturedDepth) g_capturedDepth->Release();
                    g_capturedDepth = dsv;
                }
            }
            tex->Release();
        }
        if (res) res->Release();
    }
    g_omSet.orig<OMSetFn>(ctx)(ctx, n, rtvs, dsv);
}

void hookDeviceAndContext(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (dev) g_createLayout.install(dev, reinterpret_cast<void*>(&hkCreateInputLayout));
    if (ctx) {
        g_drawIndexed.install(ctx, reinterpret_cast<void*>(&hkDrawIndexed));
        g_drawIndexedInst.install(ctx, reinterpret_cast<void*>(&hkDrawIndexedInstanced));
        g_setLayout.install(ctx, reinterpret_cast<void*>(&hkSetInputLayout));
        g_omSet.install(ctx, reinterpret_cast<void*>(&hkOMSetRenderTargets));
    }
}

void hookSwapChain(IDXGISwapChain* sc) {
    g_present.install(sc, reinterpret_cast<void*>(&hkPresent));
    g_resize.install(sc, reinterpret_cast<void*>(&hkResize));
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1)))) {
        g_present1.install(sc1, reinterpret_cast<void*>(&hkPresent1));
        sc1->Release();
    }
}

// The game's own objects may use different vtables than our dummy device (e.g. other
// creation flags); patch them as well. Installing on an already-patched vtable is a no-op.
void hookGameObjects(IDXGISwapChain* sc) {
    hookSwapChain(sc);
    ID3D11Device* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev)))) {
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        hookDeviceAndContext(dev, ctx);
        if (ctx) ctx->Release();
        dev->Release();
        RFV_LOG("Hooked the game's D3D11 device and immediate context");
    } else {
        RFV_LOG("Swap chain is not backed by a D3D11 device; rendering disabled");
    }
}

// ---- swap-chain creation (catches the game's real swap chain, whatever its class)
constexpr int kFactoryCreateSwapChain = 10;
constexpr int kFactory2CreateForHwnd = 15;
constexpr int kFactory2CreateForCoreWindow = 16;
constexpr int kFactory2CreateForComposition = 24;
VtableHook g_fCreate(kFactoryCreateSwapChain), g_fCreateHwnd(kFactory2CreateForHwnd),
    g_fCreateCore(kFactory2CreateForCoreWindow), g_fCreateComp(kFactory2CreateForComposition);

void onSwapChainCreated(IUnknown* obj, const char* how) {
    IDXGISwapChain* sc = nullptr;
    if (obj && SUCCEEDED(obj->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&sc)))) {
        DXGI_SWAP_CHAIN_DESC d{};
        sc->GetDesc(&d);
        RFV_LOG("Game created a swap chain (%s): %ux%u, swap effect %d", how, d.BufferDesc.Width, d.BufferDesc.Height,
                static_cast<int>(d.SwapEffect));
        hookSwapChain(sc);
        sc->Release();
    }
}

HRESULT STDMETHODCALLTYPE hkFactoryCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    HRESULT hr = g_fCreate.orig<Fn>(f)(f, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out) onSwapChainCreated(*out, "CreateSwapChain");
    return hr;
}
HRESULT STDMETHODCALLTYPE hkFactoryCreateForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** sc) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    HRESULT hr = g_fCreateHwnd.orig<Fn>(f)(f, dev, hwnd, desc, fs, out, sc);
    if (SUCCEEDED(hr) && sc && *sc) onSwapChainCreated(*sc, "CreateSwapChainForHwnd");
    return hr;
}
HRESULT STDMETHODCALLTYPE hkFactoryCreateForCore(IDXGIFactory2* f, IUnknown* dev, IUnknown* wnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                 IDXGIOutput* out, IDXGISwapChain1** sc) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    HRESULT hr = g_fCreateCore.orig<Fn>(f)(f, dev, wnd, desc, out, sc);
    if (SUCCEEDED(hr) && sc && *sc) onSwapChainCreated(*sc, "CreateSwapChainForCoreWindow");
    return hr;
}
HRESULT STDMETHODCALLTYPE hkFactoryCreateForComp(IDXGIFactory2* f, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* out,
                                                 IDXGISwapChain1** sc) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    HRESULT hr = g_fCreateComp.orig<Fn>(f)(f, dev, desc, out, sc);
    if (SUCCEEDED(hr) && sc && *sc) onSwapChainCreated(*sc, "CreateSwapChainForComposition");
    return hr;
}

void hookFactoryObject(IUnknown* obj) {
    IDXGIFactory* f = nullptr;
    if (!obj || FAILED(obj->QueryInterface(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&f)))) return;
    g_fCreate.install(f, reinterpret_cast<void*>(&hkFactoryCreateSwapChain));
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(f->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f2)))) {
        g_fCreateHwnd.install(f2, reinterpret_cast<void*>(&hkFactoryCreateForHwnd));
        g_fCreateCore.install(f2, reinterpret_cast<void*>(&hkFactoryCreateForCore));
        g_fCreateComp.install(f2, reinterpret_cast<void*>(&hkFactoryCreateForComp));
        f2->Release();
    }
    f->Release();
}

LRESULT CALLBACK dummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

} // namespace

namespace {
void hookFactoryOfDevice(ID3D11Device* dev) {
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev)))) return;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
        IDXGIFactory* f = nullptr;
        if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&f)))) {
            hookFactoryObject(f);
            f->Release();
        }
        adapter->Release();
    }
    dxgiDev->Release();
}
} // namespace

void setPresentListener(PresentListener* listener) { g_listener = listener; }
unsigned presentCount() { return g_presentCount.load(); }

void hookFactory(IUnknown* factory) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) RFV_LOG("Game created a DXGI factory; hooking swap-chain creation");
    hookFactoryObject(factory);
}

void hookCreatedDevice(ID3D11Device* dev) {
    static std::atomic<int> count{0};
    if (count++ < 4) RFV_LOG("Game created a D3D11 device (feature level 0x%x); hooking it", static_cast<unsigned>(dev->GetFeatureLevel()));
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    hookDeviceAndContext(dev, ctx);
    if (ctx) ctx->Release();
    hookFactoryOfDevice(dev);
}

void hookCreatedSwapChain(IDXGISwapChain* sc) { onSwapChainCreated(sc, "D3D11CreateDeviceAndSwapChain"); }

bool installD3DHooks(PresentListener* listener) {
    g_listener = listener;
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) { RFV_LOG("d3d11.dll not found"); return false; }
    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (!create) return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = dummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RFVRoidDummyWindow";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { RFV_LOG("Could not create dummy window (%lu)", GetLastError()); return false; }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = create(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) {
        RFV_LOG("Dummy D3D11 device creation failed (0x%08lx)", static_cast<unsigned long>(hr));
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }
    hookSwapChain(sc);
    hookDeviceAndContext(dev, ctx);
    hookFactoryOfDevice(dev);
    ctx->Release();
    dev->Release();
    sc->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    RFV_LOG("Fallback DXGI/D3D11 vtable hooks installed (from a test device)");
    return true;
}

void uninstallD3DHooks() {
    for (VtableHook* h : {&g_present, &g_present1, &g_resize, &g_createLayout, &g_drawIndexed, &g_drawIndexedInst, &g_setLayout, &g_omSet,
                          &g_fCreate, &g_fCreateHwnd, &g_fCreateCore, &g_fCreateComp})
        h->uninstallAll();
    g_listener = nullptr;
}

namespace draws {
void setHidden(const std::set<DrawSig>& sigs) { g_hidden = sigs; }
void setHidingEnabled(bool on) { g_hidingEnabled = on; }
void setSkinnedOnly(bool on) { g_skinnedOnly = on; }
void setLogging(bool on) { g_logging = on; }
void setSolo(bool on, DrawSig sig) { g_soloOn = on; g_solo = sig; }
void setInternal(bool on) { g_internal = on; }
void setDepthCapture(bool on) { g_captureDepth = on; }
std::vector<DrawStat> lastFrameStats() {
    std::lock_guard<std::mutex> lock(g_statsMutex);
    std::vector<DrawStat> v;
    for (auto& kv : g_lastStats) v.push_back(kv.second);
    return v;
}
size_t knownSkinnedLayouts() { return g_skinnedCount.load(); }
} // namespace draws

ID3D11DepthStencilView* acquireCapturedDepth() {
    std::lock_guard<std::mutex> lock(g_depthMutex);
    if (g_capturedDepth) g_capturedDepth->AddRef();
    return g_capturedDepth;
}

} // namespace rfv::win
