// D3D11 / DXGI hooks: per-frame callback at Present, draw-call filtering (used to hide
// Mason's own mesh), skinned input-layout tracking and depth-buffer capture.
#pragma once
#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>
#include <set>
#include <vector>

namespace rfv::win {

struct DrawSig {
    UINT indexCount = 0;
    UINT startIndex = 0;
    bool operator<(const DrawSig& o) const {
        return indexCount != o.indexCount ? indexCount < o.indexCount : startIndex < o.startIndex;
    }
    bool operator==(const DrawSig& o) const { return indexCount == o.indexCount && startIndex == o.startIndex; }
};

struct DrawStat {
    DrawSig sig;
    UINT calls = 0;
    bool skinned = false;
};

class PresentListener {
public:
    virtual ~PresentListener() = default;
    virtual void onPresent(IDXGISwapChain* swapChain) = 0;
    virtual void onBeforeResize() = 0;
};

// Precise hooks on the game's own objects, called by the proxy exports as the game
// creates them (safe to call more than once).
void hookFactory(IUnknown* factory);
void hookCreatedDevice(ID3D11Device* device);
void hookCreatedSwapChain(IDXGISwapChain* swapChain);
void setPresentListener(PresentListener* listener);
unsigned presentCount();  // frames presented since startup (0 = Present hook never ran)

// Fallback: creates a throw-away device to find and patch the DXGI/D3D11 vtables. Call
// from a worker thread (never from DllMain).
bool installD3DHooks(PresentListener* listener);
void uninstallD3DHooks();

namespace draws {
// Draws to skip while the VRM skin is active.
void setHidden(const std::set<DrawSig>& sigs);
void setHidingEnabled(bool on);
// Only consider draws using an input layout with BLENDINDICES (skinned meshes).
void setSkinnedOnly(bool on);
// Draw logger: collects per-frame stats; `solo` (if set) is hidden for identification.
void setLogging(bool on);
void setSolo(bool on, DrawSig sig);
std::vector<DrawStat> lastFrameStats();
size_t knownSkinnedLayouts();
// True while our own renderer is issuing draws (bypasses filtering).
void setInternal(bool on);
// Track the game's depth buffer (needed only for Render.DepthMode != none).
void setDepthCapture(bool on);
} // namespace draws

// Last depth-stencil view the game bound whose size matches the back buffer (AddRef'd), or null.
ID3D11DepthStencilView* acquireCapturedDepth();

} // namespace rfv::win
