#include "mod.hpp"

#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <sstream>

#include "../core/log.hpp"
#include "memory_win.hpp"
#include "paths.hpp"
#include "state_backup.hpp"

namespace rfv::win {

#include "default_config.inc"  // kDefaultConfig: contents of config/RFVRoid.ini

namespace {

const Color kOrange = Color::rgb(242, 152, 33);
const Color kOrangeDim = Color::rgb(242, 152, 33, 0.55f);
const Color kWhite = Color::rgb(255, 255, 255);
const Color kGrey = Color::rgb(190, 180, 170);
const Color kRed = Color::rgb(255, 90, 70);
const Color kBarDark = Color::rgb(30, 22, 18, 0.85f);
const Color kShade = Color::rgb(0, 0, 0, 0.6f);

std::string upper(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return r;
}

Vec3 parseVec3(const std::string& s, Vec3 def) {
    float a, b, c;
    if (std::sscanf(s.c_str(), "%f , %f , %f", &a, &b, &c) == 3) return {a, b, c};
    return def;
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

} // namespace

// =====================================================================================
// PauseTracker
// =====================================================================================

void PauseTracker::onEvent(input::Action a, int menuItems, int resumeIndex) {
    using input::Action;
    switch (state_) {
    case State::Running:
        if (a == Action::Pause && !authoritative_) { state_ = State::Menu; sel_ = 0; }
        break;
    case State::Menu:
        if (a == Action::Back && !authoritative_) state_ = State::Running;
        else if (a == Action::Up) sel_ = (sel_ + menuItems - 1) % menuItems;
        else if (a == Action::Down) sel_ = (sel_ + 1) % menuItems;
        else if (a == Action::Accept) {
            if (sel_ == resumeIndex) { if (!authoritative_) state_ = State::Running; }
            else state_ = State::Submenu;
        }
        break;
    case State::Submenu:
        if (a == Action::Back) state_ = State::Menu;
        break;
    }
}

void PauseTracker::onClickRow(int row, int resumeIndex) {
    if (state_ != State::Menu || row < 0) return;
    sel_ = row;
    if (row == resumeIndex) { if (!authoritative_) state_ = State::Running; }
    else state_ = State::Submenu;
}

void PauseTracker::setAuthoritative(int paused) {
    authoritative_ = paused >= 0;
    if (paused == 0) state_ = State::Running;
    else if (paused == 1 && state_ == State::Running) { state_ = State::Menu; sel_ = 0; }
}

void PauseTracker::onGameplayDetected() {
    if (!authoritative_) state_ = State::Running;
}

// =====================================================================================
// Startup / configuration
// =====================================================================================

Mod& Mod::get() {
    static Mod m;
    return m;
}

void Mod::loadConfig() {
    std::string path = configPath();
    if (!fileExists(path)) {
        FILE* f = _wfopen(widen(path).c_str(), L"wb");
        if (f) {
            std::fwrite(kDefaultConfig, 1, sizeof(kDefaultConfig) - 1, f);
            std::fclose(f);
            RFV_LOG("Wrote default configuration to %s", path.c_str());
        }
    }
    cfg_ = Ini();
    cfg_.loadString(kDefaultConfig);  // defaults first ...
    Ini user;
    if (user.loadFile(path)) {        // ... then the user's file overrides key by key
        for (const char* sec : {"General", "Scale", "Menu", "Render", "Physics", "Expressions", "GameSkeleton", "BoneMap", "GameCamera",
                                "GamePause", "GameHide", "DrawFilter", "GameState", "GameLighting"})
            for (auto& kv : user.section(sec)) cfg_.set(sec, kv.first, kv.second);
    }
    state_ = Ini();
    state_.loadFile(statePath());

    bindingErrors_.clear();
    binding_.load(cfg_, bindingErrors_);
    if (!bindingErrors_.empty()) RFV_LOG("Configuration problems:\n%s", bindingErrors_.c_str());
    RFV_LOG("Game binding: skeleton=%s camera=%s pauseFlag=%s memoryHide=%s",
            binding_.hasSkeleton() ? "configured" : "not configured", binding_.hasCamera() ? "configured" : "not configured",
            binding_.hasPauseFlag() ? "configured" : "not configured", binding_.hasHideWrites() ? "configured" : "not configured");
    debugOverlay_ = cfg_.getBool("General", "Debug", false);
    draws::setSkinnedOnly(cfg_.getBool("DrawFilter", "SkinnedOnly", true));
    std::string dm = toLower(cfg_.get("Render", "DepthMode", "none"));
    draws::setDepthCapture(dm == "standard" || dm == "reversed");
    loadHiddenDraws();
}

void Mod::saveState() {
    if (!state_.saveFile(statePath())) RFV_LOG("Could not write %s", statePath().c_str());
}

void Mod::loadHiddenDraws() {
    hiddenDraws_.clear();
    // Sources: [DrawFilter] HideDraws in the config plus the list saved from the in-game logger.
    for (const std::string& list : {cfg_.get("DrawFilter", "HideDraws"), state_.get("State", "HideDraws")}) {
        std::stringstream ss(list);
        std::string item;
        while (std::getline(ss, item, ',')) {
            unsigned a = 0, b = 0;
            if (std::sscanf(trim(item).c_str(), "%u:%u", &a, &b) >= 1 && a > 0) hiddenDraws_.insert({a, b});
        }
    }
    draws::setHidden(hiddenDraws_);
}

void Mod::saveHiddenDraws() {
    std::string s;
    for (const DrawSig& d : hiddenDraws_) {
        if (!s.empty()) s += ",";
        s += std::to_string(d.indexCount) + ":" + std::to_string(d.startIndex);
    }
    state_.set("State", "HideDraws", s);
    saveState();
    draws::setHidden(hiddenDraws_);
}

void Mod::start() {
    log::open(logPath());
    RFV_LOG("RedFactionVRoid starting (game folder: %s)", gameDirectory().c_str());
    loadConfig();
    vrmDirectory();  // make sure the folder exists so players know where to put files
    if (!cfg_.getBool("General", "Enabled", true)) {
        RFV_LOG("Disabled in RFVRoid.ini");
        return;
    }
    if (!installD3DHooks(this)) {
        RFV_LOG("Failed to install D3D hooks; the mod is inactive");
        return;
    }
    std::string skin = state_.get("State", "Skin");
    if (!skin.empty() && state_.getBool("State", "Enabled", true)) {
        if (fileExists(skin)) requestLoad(skin);
        else RFV_LOG("Saved skin %s no longer exists", skin.c_str());
    }
}

// =====================================================================================
// Avatar loading
// =====================================================================================

void Mod::requestLoad(const std::string& path) {
    if (loading_.exchange(true)) {
        showToast("STILL LOADING THE PREVIOUS AVATAR...");
        return;
    }
    loadingPath_ = path;
    status_ = "LOADING " + upper(fileStem(path)) + "...";
    statusIsError_ = false;
    std::thread([this, path] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto la = std::make_unique<LoadedAvatar>();
        la->path = path;
        std::string error;
        try {
            la->model = std::make_unique<vrm::Model>(vrm::loadFile(path));
            la->avatar = std::make_unique<vrm::Avatar>(*la->model);
            la->springs = std::make_unique<vrm::SpringSystem>(*la->avatar);
            la->expressions = std::make_unique<vrm::ExpressionController>(*la->model, GetTickCount() | 1u);
            la->images.resize(la->model->images.size());
            for (size_t i = 0; i < la->model->images.size(); ++i) {
                std::string e;
                if (!decodeImage(la->model->images[i].data, la->images[i], e))
                    la->warnings.push_back("texture " + std::to_string(i) + ": " + e);
                la->model->images[i].data.clear();
                la->model->images[i].data.shrink_to_fit();
            }
        } catch (const std::exception& e) {
            error = e.what();
        }
        CoUninitialize();
        if (!error.empty()) {
            RFV_LOG("Failed to load %s: %s", path.c_str(), error.c_str());
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pendingError_ = "COULD NOT LOAD: " + error;
            loading_ = false;
            return;
        }
        for (auto& w : la->warnings) RFV_LOG("Warning (%s): %s", path.c_str(), w.c_str());
        RFV_LOG("Parsed %s: \"%s\" (VRM %s), %zu nodes, %zu meshes, %zu humanoid bones, rest height %.3f m", path.c_str(),
                la->model->meta.title.c_str(), la->model->version == vrm::VrmVersion::V1 ? "1.0" : "0.x", la->model->nodes.size(),
                la->model->meshes.size(), la->model->humanoid.size(), la->avatar->rest().height);
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_ = std::move(la);
        loading_ = false;
    }).detach();
}

void Mod::takePending() {
    std::unique_ptr<LoadedAvatar> p;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        p = std::move(pending_);
        if (!pendingError_.empty()) {
            status_ = pendingError_;
            statusIsError_ = true;
            showToast(pendingError_, 5000);
            pendingError_.clear();
        }
    }
    if (!p || !d3dReady_) {
        if (p) { std::lock_guard<std::mutex> lock(pendingMutex_); pending_ = std::move(p); }
        return;
    }
    std::string error;
    if (!avatarR_.upload(*p, error)) {
        status_ = "COULD NOT DISPLAY AVATAR: " + error;
        statusIsError_ = true;
        RFV_LOG("Upload failed: %s", error.c_str());
        if (active_) {  // restore the previous avatar's GPU data
            std::string e2;
            avatarR_.upload(*active_, e2);
        }
        return;
    }
    active_ = std::move(p);
    skinEnabled_ = true;
    if (active_->expressions) {
        active_->expressions->setAutoBlink(cfg_.getBool("Expressions", "AutoBlink", true));
        std::string face = state_.get("State", "Expression", cfg_.get("Expressions", "Default", "neutral"));
        active_->expressions->setBase(active_->expressions->has(face) ? face : "neutral");
    }
    RFV_LOG("Avatar features: %zu spring bones, %zu colliders, %zu expressions", active_->model->springJoints.size(),
            active_->model->colliders.size(), active_->model->expressions.size());
    std::string name = active_->model->meta.title.empty() ? fileStem(active_->path) : active_->model->meta.title;
    status_ = "ACTIVE: " + upper(name);
    statusIsError_ = false;
    showToast("VRM SKIN: " + upper(name));
    state_.set("State", "Skin", active_->path);
    state_.set("State", "Enabled", "1");
    saveState();
}

void Mod::disableSkin() {
    skinEnabled_ = false;
    active_.reset();
    avatarR_.unload();
    binding_.applyHide(processMemory(), false);
    draws::setHidingEnabled(false);
    state_.set("State", "Enabled", "0");
    saveState();
    status_ = "ACTIVE: MASON (NO SKIN)";
    statusIsError_ = false;
    showToast("VRM SKIN DISABLED");
}

float Mod::avatarScale() const {
    if (!active_) return 1.0f;
    const vrm::Avatar& av = *active_->avatar;
    std::string mode = toLower(cfg_.get("Scale", "Mode", "auto"));
    float mult = cfg_.getFloat("Scale", "Multiplier", 1.0f);
    float masonHeight = cfg_.getFloat("Scale", "MasonHeight", 1.83f);
    float s = 1.0f;
    if (mode == "none") s = 1.0f;
    else if (mode == "auto" && masonSpanMax_ > 0.5f) s = av.scaleForJointSpan(masonSpanMax_);
    else s = av.scaleForHeight(masonHeight);
    return s * (mult > 0 ? mult : 1.0f);
}

// =====================================================================================
// Per-frame
// =====================================================================================

bool Mod::initD3D(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev_)))) {
        RFV_LOG("Swap chain has no D3D11 device");
        return false;
    }
    dev_->GetImmediateContext(&ctx_);
    std::string err;
    if (!avatarR_.init(dev_, err)) { RFV_LOG("Avatar renderer init failed: %s", err.c_str()); return false; }
    if (!ui_.init(dev_, err)) { RFV_LOG("UI renderer init failed: %s", err.c_str()); return false; }
    ui_.setFonts(widen(cfg_.get("Menu", "MenuFont", "Arial Black")), widen(cfg_.get("Menu", "HintFont", "Impact")),
                 widen(cfg_.get("Menu", "BodyFont", "Segoe UI")));
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(sc->GetDesc(&desc))) {
        UINT hotkey = static_cast<UINT>(cfg_.getInt("General", "MenuHotkey", VK_F7));
        input::install(desc.OutputWindow, hotkey);
    }
    input::hookGameInput();
    RFV_LOG("Renderer ready");
    return true;
}

void Mod::onBeforeResize() {
    avatarR_.releaseSizeDependent();
}

void Mod::sampleGame() {
    ProcessMemory& mem = processMemory();
    pause_.setAuthoritative(binding_.readPaused(mem));

    vrm::SourcePose p;
    poseValid_ = binding_.samplePose(mem, p);
    if (poseValid_) {
        float footY = 1e9f;
        for (const char* f : {vrm::bones::LeftFoot, vrm::bones::RightFoot})
            if (p.has(f)) footY = std::min(footY, p.at(f).y);
        if (footY < 1e8f) {
            float span = p.at(vrm::bones::Head).y - footY;
            if (span > 0.8f && span < 3.0f) masonSpanMax_ = std::max(masonSpanMax_, span);
        }
        Vec3 hips = p.at(vrm::bones::Hips);
        bool moving = length(hips - lastHips_) > 0.0005f;
        lastHips_ = hips;
        stillFrames_ = moving ? std::max(0, stillFrames_) + 1 : std::min(0, stillFrames_) - 1;
        if (stillFrames_ > 15 && pause_.state() != PauseTracker::State::Running) pause_.onGameplayDetected();
        pose_ = std::move(p);
    }
    float aspect = bbH_ ? static_cast<float>(bbW_) / bbH_ : 16.0f / 9.0f;
    cameraValid_ = binding_.readCamera(mem, aspect, camera_);
}

void Mod::onPresent(IDXGISwapChain* sc) {
    if (d3dFailed_) return;
    if (!d3dReady_) {
        d3dReady_ = initD3D(sc);
        if (!d3dReady_) { d3dFailed_ = true; return; }
    }
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(sc->GetDesc(&desc))) return;
    bbW_ = desc.BufferDesc.Width;
    bbH_ = desc.BufferDesc.Height;
    if (bbW_ < 64 || bbH_ < 64) return;
    input::setBackBufferSize(bbW_, bbH_);

    takePending();
    {
        std::lock_guard<std::mutex> lock(browseMutex_);
        if (!browsedPath_.empty()) {
            requestLoad(browsedPath_);
            browsedPath_.clear();
        }
    }

    sampleGame();
    input::MouseState mouse;
    auto events = input::poll(mouse);
    handleInput(events, mouse);

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    frameDt_ = lastFrameTime_ ? static_cast<float>(double(now.QuadPart - lastFrameTime_) / double(freq.QuadPart)) : 0.0f;
    lastFrameTime_ = now.QuadPart;

    bool canReplace = skinEnabled_ && active_ && poseValid_ && cameraValid_;
    if (active_) {
        active_->avatar->setScale(avatarScale());
        if (poseValid_ && !panelOpen_) {
            active_->avatar->applySource(pose_, cfg_.getBool("Render", "GroundFeet", true));
            if (active_->springs && cfg_.getBool("Physics", "Enabled", true)) {
                float strength = cfg_.getFloat("Physics", "Wind", 0.0f);
                active_->springs->setExternalForce(Vec3{strength, 0, strength * 0.3f} *
                                                   (0.6f + 0.4f * std::sin(static_cast<float>(now.QuadPart / double(freq.QuadPart)) * 1.7f)));
                // While the game is paused the world is frozen, so freeze the hair too.
                active_->springs->update(pause_.state() == PauseTracker::State::Running ? frameDt_ : 0.0f);
            }
        }
        updateFace();
    }
    draws::setHidingEnabled(canReplace);
    if (binding_.hasHideWrites()) binding_.applyHide(processMemory(), canReplace);

    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return;
    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_TEXTURE2D_DESC bd;
    back->GetDesc(&bd);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    // Typeless/sRGB back buffers need an explicit view format.
    rd.Format = bd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM
              : bd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : bd.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    HRESULT hr = dev_->CreateRenderTargetView(back, &rd, &rtv);
    back->Release();
    if (FAILED(hr)) return;

    D3D11StateBackup backup;
    backup.save(ctx_);
    bool hideInPause = cfg_.getBool("Render", "HideWhenPaused", true) && pause_.state() != PauseTracker::State::Running;
    if (canReplace && !panelOpen_ && !hideInPause) drawWorldAvatar(rtv);

    ui_.begin(ctx_, rtv, static_cast<float>(bbW_), static_cast<float>(bbH_));
    if (panelOpen_) drawPanel(rtv);
    else if (pause_.inPauseMenu() && !logger_) drawPauseEntry();
    if (debugOverlay_ || logger_) drawDebug();
    drawToast();
    ui_.end();
    backup.restore(ctx_);
    rtv->Release();
    captureIfRequested(sc);
}

// Test hook: RFVROID_CAPTURE_FRAME=<n> and RFVROID_CAPTURE_PATH=<file.bmp> save frame n
// (as presented, including everything the mod drew) to a BMP.
void Mod::captureIfRequested(IDXGISwapChain* sc) {
    ++frameCounter_;
    wchar_t buf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"RFVROID_CAPTURE_FRAME", buf, MAX_PATH)) return;
    if (static_cast<unsigned>(_wtoi(buf)) != frameCounter_) return;
    if (!GetEnvironmentVariableW(L"RFVROID_CAPTURE_PATH", buf, MAX_PATH)) return;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return;
    D3D11_TEXTURE2D_DESC td;
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &staging))) {
        ctx_->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            bool bgra = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            if (FILE* f = _wfopen(buf, L"wb")) {
                BITMAPFILEHEADER fh{};
                BITMAPINFOHEADER ih{};
                ih.biSize = sizeof(ih);
                ih.biWidth = static_cast<LONG>(td.Width);
                ih.biHeight = -static_cast<LONG>(td.Height);
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + td.Width * td.Height * 4;
                std::fwrite(&fh, sizeof(fh), 1, f);
                std::fwrite(&ih, sizeof(ih), 1, f);
                std::vector<uint8_t> row(td.Width * 4);
                for (UINT y = 0; y < td.Height; ++y) {
                    const uint8_t* src = static_cast<const uint8_t*>(map.pData) + y * map.RowPitch;
                    for (UINT x = 0; x < td.Width; ++x) {
                        row[x * 4 + 0] = src[x * 4 + (bgra ? 0 : 2)];
                        row[x * 4 + 1] = src[x * 4 + 1];
                        row[x * 4 + 2] = src[x * 4 + (bgra ? 2 : 0)];
                        row[x * 4 + 3] = 255;
                    }
                    std::fwrite(row.data(), 1, row.size(), f);
                }
                std::fclose(f);
                RFV_LOG("Captured frame %u", frameCounter_);
            }
            ctx_->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
}

void Mod::updateFace() {
    if (!active_ || !active_->expressions) return;
    vrm::ExpressionController& ec = *active_->expressions;
    // React to Mason being hurt / near death, if his health is configured.
    float h = binding_.hasHealth() ? binding_.readHealth(processMemory()) : -1.0f;
    if (h >= 0) {
        float maxCfg = cfg_.getFloat("GameState", "HealthMax", 0);
        healthMax_ = maxCfg > 0 ? maxCfg : std::max(healthMax_, h);
        if (lastHealth_ >= 0 && h < lastHealth_ - 0.001f * std::max(healthMax_, 1.0f))
            ec.trigger(cfg_.get("Expressions", "Hurt", "angry"), 0.6f);
        float lowFrac = cfg_.getFloat("Expressions", "LowHealthFraction", 0.25f);
        if (healthMax_ > 0 && h > 0 && h < healthMax_ * lowFrac) ec.trigger(cfg_.get("Expressions", "LowHealth", "sad"), 0.2f, 0.7f);
        lastHealth_ = h;
    }
    ec.update(frameDt_, active_->avatar->morphWeights());
}

void Mod::drawWorldAvatar(ID3D11RenderTargetView* rtv) {
    AvatarDrawParams p;
    p.viewProj = camera_.proj * camera_.view;
    p.avatarToWorld = binding_.avatarToGame();
    p.lightDir = binding_.gameDirToAvatar(parseVec3(cfg_.get("Render", "LightDir"), {0.3f, 0.8f, 0.5f}));
    Vec3 lc = parseVec3(cfg_.get("Render", "LightColor"), {1.0f, 0.95f, 0.9f});
    Vec3 am = parseVec3(cfg_.get("Render", "Ambient"), {0.35f, 0.3f, 0.3f});
    p.lightColor[0] = lc.x; p.lightColor[1] = lc.y; p.lightColor[2] = lc.z;
    p.ambient[0] = am.x; p.ambient[1] = am.y; p.ambient[2] = am.z;
    p.viewport = {0, 0, static_cast<float>(bbW_), static_cast<float>(bbH_), 0, 1};
    std::string dm = toLower(cfg_.get("Render", "DepthMode", "none"));
    ID3D11DepthStencilView* gameDepth = nullptr;
    if (dm == "standard" || dm == "reversed") {
        gameDepth = acquireCapturedDepth();
        if (gameDepth) p.depth = dm == "reversed" ? AvatarDrawParams::Depth::GameReversed : AvatarDrawParams::Depth::GameStandard;
    }

    // Lighting from the scene around Mason (and the sun, if its location in memory is known).
    if (toLower(cfg_.get("Render", "Lighting", "scene")) == "scene") {
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        ID3D11Texture2D* back = nullptr;
        if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) {
            if (avatarR_.captureScene(ctx_, back)) {
                p.sceneLighting = true;
                Mat4 vp = camera_.proj * camera_.view;
                auto project = [&](const Vec3& g, float& sx, float& sy) {
                    float cx = vp.at(0, 0) * g.x + vp.at(0, 1) * g.y + vp.at(0, 2) * g.z + vp.at(0, 3);
                    float cy = vp.at(1, 0) * g.x + vp.at(1, 1) * g.y + vp.at(1, 2) * g.z + vp.at(1, 3);
                    float cw = vp.at(3, 0) * g.x + vp.at(3, 1) * g.y + vp.at(3, 2) * g.z + vp.at(3, 3);
                    if (cw < 0.01f) return false;
                    sx = cx / cw * 0.5f + 0.5f;
                    sy = 0.5f - cy / cw * 0.5f;
                    return true;
                };
                Mat4 toGame = binding_.avatarToGame();
                Vec3 chest = toGame.transformPoint(active_->avatar->bonePosition(vrm::bones::Hips) * 0.4f +
                                                   active_->avatar->bonePosition(vrm::bones::Head) * 0.6f);
                Vec3 right{camera_.view.at(0, 0), camera_.view.at(0, 1), camera_.view.at(0, 2)};
                float ax, ay, bx, by;
                if (project(chest, ax, ay) && project(chest + right * 1.0f, bx, by)) {
                    p.sceneCenterUv[0] = std::clamp(ax, 0.0f, 1.0f);
                    p.sceneCenterUv[1] = std::clamp(ay, 0.0f, 1.0f);
                    float pxPerMetre = std::fabs(bx - ax) * bbW_;
                    p.sceneRadiusPx = std::clamp(pxPerMetre * cfg_.getFloat("Render", "SceneRadius", 1.5f), 24.0f, bbH_ * 0.5f);
                }
                Mat4 nv = camera_.view * toGame;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) p.normalToView[r][c] = nv.at(r, c);
                p.exposure = cfg_.getFloat("Render", "SceneExposure", 2.0f);
                p.minBright = cfg_.getFloat("Render", "SceneMinBrightness", 0.22f);
                p.maxBright = cfg_.getFloat("Render", "SceneMaxBrightness", 1.25f);
                p.saturation = cfg_.getFloat("Render", "SceneSaturation", 0.45f);
                Vec3 sunDir, sunCol;
                if (binding_.readSun(processMemory(), sunDir, sunCol)) {
                    p.lightDir = sunDir;
                    p.lightColor[0] = sunCol.x; p.lightColor[1] = sunCol.y; p.lightColor[2] = sunCol.z;
                } else {
                    p.lightColor[0] = p.lightColor[1] = p.lightColor[2] = 1.0f;  // colour comes from the scene
                }
            }
            back->Release();
        }
        if (res) res->Release();
    }
    avatarR_.draw(ctx_, *active_->avatar, p, rtv, gameDepth);
    if (gameDepth) gameDepth->Release();
}

// =====================================================================================
// Input
// =====================================================================================

Mod::Layout Mod::layout() const {
    Layout l;
    float w = static_cast<float>(bbW_), h = static_cast<float>(bbH_);
    l.s = std::min(w / 1280.0f, h / 720.0f);
    l.ox = (w - 1280.0f * l.s) * 0.5f;
    l.oy = (h - 720.0f * l.s) * 0.5f;
    return l;
}

bool Mod::entryRect(float& x, float& y, float& w, float& h) const {
    Layout L = layout();
    float rowH = cfg_.getFloat("Menu", "RowHeight", 44);
    float firstY = cfg_.getFloat("Menu", "FirstRowY", 300);
    int items = cfg_.getInt("Menu", "MenuItems", 7);
    float cx = cfg_.getFloat("Menu", "EntryX", 442);
    float cy = cfg_.has("Menu", "EntryY") && !cfg_.get("Menu", "EntryY").empty() ? cfg_.getFloat("Menu", "EntryY", 0) : firstY + items * rowH;
    x = L.X(cx - 14);
    y = L.Y(cy - rowH * 0.5f);
    w = L.S(512);
    h = L.S(rowH);
    return true;
}

void Mod::openPanel() {
    refreshFiles();
    panelOpen_ = true;
    panelSel_ = 0;
    if (active_ && skinEnabled_)
        for (size_t i = 0; i < files_.size(); ++i)
            if (CompareStringOrdinal(widen(files_[i]).c_str(), -1, widen(active_->path).c_str(), -1, TRUE) == CSTR_EQUAL)
                panelSel_ = static_cast<int>(i) + 1;
    panelScroll_ = 0;
    if (status_.empty()) status_ = skinEnabled_ && active_ ? "ACTIVE: " + upper(fileStem(active_->path)) : "ACTIVE: MASON (NO SKIN)";
    input::setCapture(true);
}

void Mod::closePanel() {
    panelOpen_ = false;
    input::setCapture(false);
}

void Mod::refreshFiles() { files_ = listVrmFiles(); }

int Mod::faceItemIndex() const {
    // Only offered when the avatar has at least one expression besides "neutral".
    bool usable = active_ && skinEnabled_ && active_->expressions && active_->expressions->selectable().size() > 1;
    return usable ? static_cast<int>(files_.size()) + 1 : -1;
}

int Mod::panelItemCount() const { return static_cast<int>(files_.size()) + 4 + (faceItemIndex() >= 0 ? 1 : 0); }

std::string Mod::panelItemLabel(int i) const {
    if (i == 0) return "MASON (NO SKIN)";
    int n = static_cast<int>(files_.size());
    if (i <= n) return upper(fileStem(files_[i - 1]));
    int face = faceItemIndex();
    if (i == face) {
        std::string b = active_->expressions->base();
        return "FACE:  < " + upper(b.empty() ? "neutral" : b) + " >";
    }
    int k = i - n - (face >= 0 ? 1 : 0);
    if (k == 1) return "BROWSE FOR A .VRM FILE...";
    if (k == 2) return "OPEN VRM FOLDER";
    return "BACK";
}

void Mod::cycleFace(int dir) {
    if (!active_ || !active_->expressions) return;
    auto opts = active_->expressions->selectable();
    std::string cur = active_->expressions->base();
    if (cur.empty()) cur = "neutral";
    int idx = 0;
    for (size_t i = 0; i < opts.size(); ++i)
        if (opts[i] == cur) idx = static_cast<int>(i);
    int n = static_cast<int>(opts.size());
    idx = ((idx + dir) % n + n) % n;
    active_->expressions->setBase(opts[idx]);
    state_.set("State", "Expression", opts[idx]);
    saveState();
}

void Mod::activatePanelItem(int i) {
    int n = static_cast<int>(files_.size());
    int face = faceItemIndex();
    if (i == 0) disableSkin();
    else if (i <= n) requestLoad(files_[i - 1]);
    else if (i == face) cycleFace(+1);
    else {
        int k = i - n - (face >= 0 ? 1 : 0);
        if (k == 1) browseForFile();
        else if (k == 2) {
            std::wstring dir = widen(vrmDirectory());
            ShellExecuteW(nullptr, L"explore", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else closePanel();
    }
}

void Mod::browseForFile() {
    if (browsing_.exchange(true)) return;
    HWND owner = GetForegroundWindow();
    std::thread([this, owner] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        IFileOpenDialog* dlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
            COMDLG_FILTERSPEC spec[] = {{L"VRM avatar (*.vrm)", L"*.vrm"}, {L"All files", L"*.*"}};
            dlg->SetFileTypes(2, spec);
            dlg->SetTitle(L"Choose a VRM avatar for Mason");
            if (SUCCEEDED(dlg->Show(owner))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dlg->GetResult(&item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        std::lock_guard<std::mutex> lock(browseMutex_);
                        browsedPath_ = narrow(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dlg->Release();
        }
        CoUninitialize();
        browsing_ = false;
    }).detach();
}

void Mod::handleInput(const std::vector<input::Event>& events, const input::MouseState& mouse) {
    using input::Action;
    int menuItems = cfg_.getInt("Menu", "MenuItems", 7);
    int resumeIdx = cfg_.getInt("Menu", "ResumeRow", 0);
    Layout L = layout();

    for (const auto& e : events) {
        if (e.action == Action::DebugToggle) { debugOverlay_ = !debugOverlay_; continue; }
        if (e.action == Action::LoggerToggle) {
            logger_ = !logger_;
            draws::setLogging(logger_);
            if (!logger_) draws::setSolo(false, {});
            showToast(logger_ ? "DRAW LOGGER ON (PGUP/PGDN SELECT, INS HIDE, DEL UNHIDE)" : "DRAW LOGGER OFF");
            continue;
        }
        if (logger_ && !panelOpen_) {
            int n = static_cast<int>(loggerStats_.size());
            if (e.action == Action::PageUp && n) { loggerSel_ = (loggerSel_ + n - 1) % n; continue; }
            if (e.action == Action::PageDown && n) { loggerSel_ = (loggerSel_ + 1) % n; continue; }
            if (e.action == Action::Insert && loggerSel_ < n) { hiddenDraws_.insert(loggerStats_[loggerSel_].sig); saveHiddenDraws(); continue; }
            if (e.action == Action::Delete && loggerSel_ < n) { hiddenDraws_.erase(loggerStats_[loggerSel_].sig); saveHiddenDraws(); continue; }
        }

        if (panelOpen_) {
            int count = panelItemCount();
            switch (e.action) {
            case Action::Up: panelSel_ = (panelSel_ + count - 1) % count; break;
            case Action::Down: panelSel_ = (panelSel_ + 1) % count; break;
            case Action::PageUp: panelSel_ = std::max(0, panelSel_ - 5); break;
            case Action::PageDown: panelSel_ = std::min(count - 1, panelSel_ + 5); break;
            case Action::Accept: activatePanelItem(panelSel_); break;
            case Action::Left: if (panelSel_ == faceItemIndex()) cycleFace(-1); break;
            case Action::Right: if (panelSel_ == faceItemIndex()) cycleFace(+1); break;
            case Action::Back: closePanel(); break;
            case Action::Hotkey: closePanel(); break;
            default: break;
            }
            continue;
        }

        if (e.action == Action::Hotkey) { openPanel(); continue; }
        if (pause_.inPauseMenu() && e.action == Action::OpenSkinMenu) { openPanel(); continue; }
        pause_.onEvent(e.action, menuItems, resumeIdx);
    }

    // Mouse.
    if (panelOpen_) {
        float rowH = L.S(44), listX = L.X(428), listY = L.Y(230), listW = L.S(420);
        int visible = 8;
        if (mouse.wheel) panelScroll_ = std::max(0, std::min(panelItemCount() - visible, panelScroll_ - mouse.wheel));
        if (mouse.x >= listX && mouse.x <= listX + listW && mouse.y >= listY) {
            int row = static_cast<int>((mouse.y - listY) / rowH);
            if (row < visible && panelScroll_ + row < panelItemCount()) {
                if (mouse.moved) panelSel_ = panelScroll_ + row;
                if (mouse.clicked) activatePanelItem(panelScroll_ + row);
            }
        }
    } else if (pause_.inPauseMenu()) {
        float x, y, w, h;
        entryRect(x, y, w, h);
        entryHover_ = mouse.x >= x && mouse.x <= x + w && mouse.y >= y && mouse.y <= y + h;
        if (mouse.clicked) {
            if (entryHover_) openPanel();
            else {
                // Track clicks on the game's own menu rows (heuristic pause tracking).
                float firstY = cfg_.getFloat("Menu", "FirstRowY", 300), rh = cfg_.getFloat("Menu", "RowHeight", 44);
                float ry = (mouse.y - L.oy) / L.s, rx = (mouse.x - L.ox) / L.s;
                int row = static_cast<int>(std::floor((ry - firstY) / rh + 0.5f));
                if (rx > 420 && rx < 960 && row >= 0 && row < menuItems) pause_.onClickRow(row, resumeIdx);
            }
        }
    }
    // Keep the selection on screen.
    if (panelOpen_) {
        int visible = 8;
        if (panelSel_ < panelScroll_) panelScroll_ = panelSel_;
        if (panelSel_ >= panelScroll_ + visible) panelScroll_ = panelSel_ - visible + 1;
    }
}

// =====================================================================================
// Drawing
// =====================================================================================

void Mod::showToast(const std::string& s, DWORD ms) {
    toast_ = s;
    toastUntil_ = GetTickCount() + ms;
}

void Mod::drawToast() {
    if (toast_.empty() || GetTickCount() > toastUntil_) return;
    Layout L = layout();
    float px = L.S(20);
    float w = ui_.measure(toast_, px, FontStyle::Hint);
    float x = (bbW_ - w) * 0.5f, y = L.Y(40);
    ui_.rect(x - L.S(16), y - L.S(8), w + L.S(32), px + L.S(16), kBarDark);
    ui_.frame(x - L.S(16), y - L.S(8), w + L.S(32), px + L.S(16), std::max(1.0f, L.S(2)), kOrange);
    ui_.text(toast_, x, y - L.S(2), px, FontStyle::Hint, kOrange);
}

void Mod::drawPauseEntry() {
    Layout L = layout();
    float x, y, w, h;
    entryRect(x, y, w, h);
    float px = L.S(cfg_.getFloat("Menu", "TextSize", 30));
    bool hot = entryHover_;
    if (hot) {
        ui_.rect(x, y + L.S(3), w, h - L.S(6), kBarDark);
        ui_.frame(x, y + L.S(3), w, h - L.S(6), std::max(1.0f, L.S(3)), kOrange);
    }
    float tx = x + L.S(14), ty = y + (h - px * 1.25f) * 0.5f;
    float tw = ui_.text("VRM SKIN", tx, ty, px, FontStyle::Menu, hot ? kWhite : kOrange);
    if (cfg_.getBool("Menu", "ShowEntryHint", true)) {
        float hp = L.S(18);
        ui_.text("[Y] / [V]", tx + tw + L.S(16), y + (h - hp * 1.25f) * 0.5f, hp, FontStyle::Hint, kOrangeDim);
    }
}

void Mod::drawPreview(ID3D11RenderTargetView* rtv, float x, float y, float w, float h) {
    if (!active_ || !skinEnabled_ || !avatarR_.hasAvatar()) return;
    vrm::Avatar& av = *active_->avatar;
    previewYaw_ += frameDt_ * 0.8f;
    Vec3 f = av.restForward();
    float face = -std::atan2(f.x, f.z);
    av.setRestPose({0, 0, 0}, face + std::sin(previewYaw_) * 0.6f, true);
    if (active_->springs && cfg_.getBool("Physics", "Enabled", true)) active_->springs->update(frameDt_);

    float H = av.rest().height * av.scale();
    float fov = 30.0f * kPi / 180.0f;
    float aspect = w / h;
    float dist = (H * 0.62f) / std::tan(fov * 0.5f);
    // The model now faces +Z in avatar space, which is -Z after the left-handed conversion
    // below, so the camera sits at -Z (world) looking towards +Z.
    Vec3 eye{0, H * 0.52f, -dist}, target{0, H * 0.5f, 0};
    Vec3 fwd = normalize(target - eye), up{0, 1, 0};
    Vec3 right = normalize(cross(up, fwd));
    up = cross(fwd, right);
    AvatarDrawParams p;
    p.viewProj = Mat4::perspectiveLH(fov, aspect, 0.05f, 50.0f) * Mat4::viewLH(eye, right, up, fwd);
    p.avatarToWorld = Mat4::scale(Vec3{1, 1, -1});
    p.lightDir = {0.4f, 0.7f, 0.6f};
    p.viewport = {x, y, w, h, 0, 1};
    p.ambient[0] = p.ambient[1] = p.ambient[2] = 0.35f;
    ui_.end();
    avatarR_.draw(ctx_, av, p, rtv, nullptr);
    ui_.begin(ctx_, rtv, static_cast<float>(bbW_), static_cast<float>(bbH_));
}

void Mod::drawPanel(ID3D11RenderTargetView* rtv) {
    Layout L = layout();
    ui_.rect(0, 0, static_cast<float>(bbW_), static_cast<float>(bbH_), kShade);

    // Title, styled after the game's slanted "PAUSE" box.
    float tpx = L.S(42);
    float tw = ui_.measure("VRM SKIN", tpx, FontStyle::Menu);
    float bx = L.X(360), by = L.Y(140), bw = tw + L.S(80), bh = L.S(70);
    ui_.frame(bx, by, bw, bh, std::max(2.0f, L.S(5)), kOrange);
    ui_.text("VRM SKIN", bx + L.S(40), by + (bh - tpx * 1.3f) * 0.5f, tpx, FontStyle::Menu, kOrange);

    // Item list.
    float rowH = L.S(44), listX = L.X(428), listY = L.Y(230), listW = L.S(420);
    float px = L.S(26);
    int visible = 8, count = panelItemCount();
    for (int r = 0; r < visible && panelScroll_ + r < count; ++r) {
        int i = panelScroll_ + r;
        float y = listY + r * rowH;
        bool sel = i == panelSel_;
        if (sel) {
            ui_.rect(listX, y + L.S(3), listW, rowH - L.S(6), kBarDark);
            ui_.frame(listX, y + L.S(3), listW, rowH - L.S(6), std::max(1.0f, L.S(3)), kOrange);
            float sx = listX + listW + L.S(14), ex = L.X(900) - L.S(14);  // stop before the preview box
            if (ex > sx) ui_.slant(sx, y + L.S(3), ex - sx, rowH - L.S(6), -L.S(12), kOrange);
        }
        std::string label = panelItemLabel(i);
        bool isActive = (i == 0 && !(skinEnabled_ && active_)) ||
                        (i > 0 && i <= static_cast<int>(files_.size()) && skinEnabled_ && active_ &&
                         CompareStringOrdinal(widen(files_[i - 1]).c_str(), -1, widen(active_->path).c_str(), -1, TRUE) == CSTR_EQUAL);
        float maxW = listW - L.S(60);
        while (label.size() > 4 && ui_.measure(label, px, FontStyle::Menu) > maxW) label = label.substr(0, label.size() - 4) + "...";
        ui_.text(label, listX + L.S(14), y + (rowH - px * 1.3f) * 0.5f, px, FontStyle::Menu, sel ? kWhite : kOrange);
        if (isActive) ui_.rect(listX + listW - L.S(26), y + rowH * 0.5f - L.S(6), L.S(12), L.S(12), sel ? kWhite : kOrange);
    }
    if (panelScroll_ > 0) ui_.text("\xE2\x96\xB2", listX + listW * 0.5f, listY - L.S(26), L.S(18), FontStyle::Body, kOrangeDim);
    if (panelScroll_ + visible < count) ui_.text("\xE2\x96\xBC", listX + listW * 0.5f, listY + visible * rowH, L.S(18), FontStyle::Body, kOrangeDim);

    // Status + scale info.
    float infoY = L.Y(612), bpx = L.S(17);
    std::string status = loading_ ? "LOADING " + upper(fileStem(loadingPath_)) + "..." : status_;
    if (files_.empty()) ui_.text("PUT .VRM FILES IN: " + vrmDirectory(), listX, infoY - L.S(26), bpx, FontStyle::Body, kGrey);
    ui_.text(status, listX, infoY, bpx, FontStyle::Body, statusIsError_ ? kRed : kGrey);
    if (active_ && skinEnabled_) {
        const vrm::Avatar& av = *active_->avatar;
        std::string mode = toLower(cfg_.get("Scale", "Mode", "auto"));
        std::string how = (mode == "auto" && masonSpanMax_ > 0.5f) ? "MATCHED TO MASON'S MEASURED SKELETON"
                        : mode == "none" ? "ORIGINAL SIZE" : fmt("MATCHED TO MASON'S HEIGHT (%.2f M)", cfg_.getFloat("Scale", "MasonHeight", 1.83f));
        ui_.text(fmt("HEIGHT %.2f M -> %.2f M  (x%.2f, %s)", av.rest().height, av.rest().height * av.scale(), av.scale(), how.c_str()),
                 listX, infoY + L.S(22), bpx, FontStyle::Body, kGrey);
        if (!poseValid_ || !cameraValid_)
            ui_.text("NOTE: GAME SKELETON/CAMERA NOT FOUND - SEE RFVROID.INI (PREVIEW ONLY)", listX, infoY + L.S(44), bpx, FontStyle::Body, kOrangeDim);
    }

    // Preview.
    float pvx = L.X(900), pvy = L.Y(150), pvw = L.S(320), pvh = L.S(440);
    ui_.rect(pvx, pvy, pvw, pvh, Color::rgb(10, 8, 6, 0.75f));
    ui_.frame(pvx, pvy, pvw, pvh, std::max(1.0f, L.S(2)), kOrangeDim);
    if (active_ && skinEnabled_) drawPreview(rtv, pvx, pvy, pvw, pvh);
    else ui_.text("MASON", pvx + L.S(110), pvy + pvh * 0.5f - L.S(14), L.S(28), FontStyle::Menu, kOrangeDim);

    // Button hints, like the game's footer.
    ui_.text("A / ENTER  SELECT     B / ESC  BACK     UP / DOWN  CHOOSE", L.X(100), L.Y(670), L.S(20), FontStyle::Hint, kOrange);
}

void Mod::drawDebug() {
    Layout L = layout();
    float px = std::max(12.0f, L.S(14));
    float x = L.S(12), y = L.S(12), lh = px * 1.35f;
    auto line = [&](const std::string& s, Color c = kWhite) {
        float w = ui_.measure(s, px, FontStyle::Body);
        ui_.rect(x - 4, y - 2, w + 8, lh, Color::rgb(0, 0, 0, 0.6f));
        ui_.text(s, x, y, px, FontStyle::Body, c);
        y += lh;
    };
    const char* ps = pause_.state() == PauseTracker::State::Running ? "running" : pause_.state() == PauseTracker::State::Menu ? "pause menu" : "pause submenu";
    line("RedFactionVRoid debug (F9)", kOrange);
    line(fmt("skeleton: %s   camera: %s   pause: %s", binding_.hasSkeleton() ? (poseValid_ ? "OK" : "FAILED") : "not configured",
             binding_.hasCamera() ? (cameraValid_ ? "OK" : "FAILED") : "not configured", ps));
    line(fmt("skinned input layouts seen: %zu   hidden draw signatures: %zu   Mason joint span: %.2f m", draws::knownSkinnedLayouts(),
             hiddenDraws_.size(), masonSpanMax_));
    if (active_) line(fmt("avatar: %s  scale x%.3f  rest height %.2f m", fileStem(active_->path).c_str(), active_->avatar->scale(), active_->avatar->rest().height));
    if (!bindingErrors_.empty()) {
        std::stringstream ss(bindingErrors_);
        std::string l;
        while (std::getline(ss, l)) line("config: " + l, kRed);
    }

    // Bone markers: index (and name) at each projected bone of the game skeleton.
    if (debugOverlay_ && cameraValid_ && binding_.hasSkeleton()) {
        std::vector<Mat4> bones;
        static std::vector<std::string> names;
        if (names.empty()) names = binding_.readBoneNames(processMemory());
        if (binding_.readBones(processMemory(), bones)) {
            Mat4 vp = camera_.proj * camera_.view;
            for (size_t i = 0; i < bones.size(); ++i) {
                Vec3 p = bones[i].translation();
                float cx = vp.at(0, 0) * p.x + vp.at(0, 1) * p.y + vp.at(0, 2) * p.z + vp.at(0, 3);
                float cy = vp.at(1, 0) * p.x + vp.at(1, 1) * p.y + vp.at(1, 2) * p.z + vp.at(1, 3);
                float cw = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z + vp.at(3, 3);
                if (cw <= 0.01f) continue;
                float sx = (cx / cw * 0.5f + 0.5f) * bbW_, sy = (0.5f - cy / cw * 0.5f) * bbH_;
                ui_.rect(sx - 2, sy - 2, 4, 4, kOrange);
                std::string label = std::to_string(i);
                if (i < names.size()) label += " " + names[i];
                ui_.text(label, sx + 4, sy - px * 0.6f, px * 0.85f, FontStyle::Body, kWhite);
            }
        }
    }

    // Draw logger.
    if (logger_) {
        loggerStats_ = draws::lastFrameStats();
        std::sort(loggerStats_.begin(), loggerStats_.end(), [](const DrawStat& a, const DrawStat& b) {
            return a.sig.indexCount != b.sig.indexCount ? a.sig.indexCount > b.sig.indexCount : a.sig.startIndex < b.sig.startIndex;
        });
        int n = static_cast<int>(loggerStats_.size());
        if (loggerSel_ >= n) loggerSel_ = std::max(0, n - 1);
        draws::setSolo(n > 0, n > 0 ? loggerStats_[loggerSel_].sig : DrawSig{});
        y += lh * 0.5f;
        line(fmt("DRAW LOGGER: %d candidate draws. The selected one is hidden so you can see what it is.", n), kOrange);
        line("PgUp/PgDn select   Insert = always hide while a VRM is active   Delete = stop hiding   F10 close", kGrey);
        int from = std::max(0, loggerSel_ - 8), to = std::min(n, from + 17);
        for (int i = from; i < to; ++i) {
            const DrawStat& d = loggerStats_[i];
            bool hidden = hiddenDraws_.count(d.sig) != 0;
            line(fmt("%s %6u indices @ %-7u x%u %s%s", i == loggerSel_ ? ">" : " ", d.sig.indexCount, d.sig.startIndex, d.calls,
                     d.skinned ? "skinned" : "", hidden ? "  [HIDDEN]" : ""),
                 i == loggerSel_ ? kOrange : (hidden ? kRed : kWhite));
        }
    }
}

} // namespace rfv::win
