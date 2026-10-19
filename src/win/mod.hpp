// The mod controller: owns configuration, the active avatar, the pause-menu UI and the
// per-frame logic that runs inside the game's Present call.
#pragma once
#include <d3d11.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../core/ini.hpp"
#include "../game/binding.hpp"
#include "avatar_renderer.hpp"
#include "d3d_hooks.hpp"
#include "input.hpp"
#include "ui_renderer.hpp"

namespace rfv::win {

// Tracks whether the game's pause menu is open. Uses a memory flag when configured,
// otherwise infers it from the inputs that open/close the menu.
class PauseTracker {
public:
    enum class State { Running, Menu, Submenu };
    void onEvent(input::Action a, int menuItems, int resumeIndex);
    void onClickRow(int row, int resumeIndex);  // row clicked on the game's pause menu (-1 = none)
    void setAuthoritative(int pausedFlag);       // from memory: -1 unknown, 0/1
    void onGameplayDetected();                   // skeleton moving => not paused
    State state() const { return state_; }
    bool inPauseMenu() const { return state_ == State::Menu; }
    int selection() const { return sel_; }
    void forceMenu() { state_ = State::Menu; }

private:
    State state_ = State::Running;
    int sel_ = 0;
    bool authoritative_ = false;
};

class Mod : public PresentListener {
public:
    static Mod& get();
    void start();  // worker thread entry point
    void onPresent(IDXGISwapChain* sc) override;
    void onBeforeResize() override;

private:
    // ---- configuration
    Ini cfg_, state_;
    game::GameBinding binding_;
    std::string bindingErrors_;
    void loadConfig();
    void saveState();

    // ---- d3d
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    bool d3dReady_ = false, d3dFailed_ = false;
    AvatarRenderer avatarR_;
    UiRenderer ui_;
    UINT bbW_ = 0, bbH_ = 0;
    bool initD3D(IDXGISwapChain* sc);

    // ---- avatar loading (background thread)
    std::unique_ptr<LoadedAvatar> active_;
    std::unique_ptr<LoadedAvatar> pending_;
    std::mutex pendingMutex_;
    std::atomic<bool> loading_{false};
    std::string loadingPath_;
    std::string pendingError_;    // set by the loader thread (guarded by pendingMutex_)
    std::string status_;          // shown in the menu
    bool statusIsError_ = false;
    bool skinEnabled_ = false;    // true when a VRM replaces Mason
    void requestLoad(const std::string& path);
    void disableSkin();
    void takePending();

    // ---- game state
    PauseTracker pause_;
    vrm::SourcePose pose_;
    bool poseValid_ = false;
    game::CameraState camera_;
    bool cameraValid_ = false;
    float masonSpanMax_ = 0;
    Vec3 lastHips_{};
    int stillFrames_ = 0;
    void sampleGame();
    float avatarScale() const;

    // ---- hiding Mason
    std::set<DrawSig> hiddenDraws_;
    void loadHiddenDraws();
    void saveHiddenDraws();

    // ---- menu
    bool panelOpen_ = false;
    int panelSel_ = 0, panelScroll_ = 0;
    std::vector<std::string> files_;
    float previewYaw_ = 0;
    std::atomic<bool> browsing_{false};
    std::string browsedPath_;
    std::mutex browseMutex_;
    void openPanel();
    void closePanel();
    void refreshFiles();
    int panelItemCount() const;
    std::string panelItemLabel(int i) const;
    void activatePanelItem(int i);
    void browseForFile();
    void handleInput(const std::vector<input::Event>& events, const input::MouseState& mouse);

    // ---- debugging aids
    bool debugOverlay_ = false;
    bool logger_ = false;
    int loggerSel_ = 0;
    std::vector<DrawStat> loggerStats_;

    // ---- drawing
    struct Layout {
        float s = 1, ox = 0, oy = 0;  // reference 1280x720 -> screen
        float X(float x) const { return ox + x * s; }
        float Y(float y) const { return oy + y * s; }
        float S(float v) const { return v * s; }
    };
    Layout layout() const;
    bool entryHover_ = false;
    void drawWorldAvatar(ID3D11RenderTargetView* rtv);
    void drawPauseEntry();
    void drawPanel(ID3D11RenderTargetView* rtv);
    void drawPreview(ID3D11RenderTargetView* rtv, float x, float y, float w, float h);
    void drawDebug();
    void drawToast();
    std::string toast_;
    DWORD toastUntil_ = 0;
    void showToast(const std::string& s, DWORD ms = 3000);
    bool entryRect(float& x, float& y, float& w, float& h) const;
    unsigned frameCounter_ = 0;
    void captureIfRequested(IDXGISwapChain* sc);
};

} // namespace rfv::win
