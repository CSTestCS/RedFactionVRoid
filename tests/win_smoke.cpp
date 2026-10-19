// Windows smoke test: behaves like a minimal D3D11 game that loads the mod's dxgi.dll.
// Run from the folder that contains dxgi.dll (and optionally vrm\*.vrm):
//   rfv_smoke.exe [frames] [screenshot.bmp]
// It forwards a factory call through the proxy, renders frames, opens the VRM menu with
// F7, applies the first avatar and asks the mod to save the last frame as a BMP.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

// ---- "world" mode: a fake game skeleton + camera in this process's memory, wired up
// through RFVRoid.ini exactly like the real game's would be.
struct M43 { float r[3], u[3], f[3], p[3]; };
static M43* g_bones = nullptr;
static M43* g_camera = nullptr;
static const int kBones = 20;
// Game space: left-handed, Y up, metres. Mason stands 4 m in front of the camera, facing it.
static const float kJoints[kBones][3] = {
    {0, 1.00f, 4},      {0, 1.15f, 4},      {0, 1.32f, 4},      {0, 1.52f, 4},      {0, 1.64f, 4},       // hips spine chest neck head
    {0.19f, 1.47f, 4},  {0.20f, 1.20f, 3.98f}, {0.20f, 1.20f, 3.72f},                                    // L upper/lower/hand (elbow bent forward)
    {-0.19f, 1.47f, 4}, {-0.21f, 1.20f, 4},  {-0.23f, 0.94f, 4},                                         // R arm hanging
    {0.10f, 0.95f, 4},  {0.10f, 0.52f, 3.97f}, {0.10f, 0.09f, 4}, {0.10f, 0.02f, 3.86f},                // L leg
    {-0.10f, 0.95f, 4}, {-0.12f, 0.52f, 4.05f}, {-0.13f, 0.09f, 4.15f}, {-0.13f, 0.02f, 4.01f},         // R leg (step back)
    {0.08f, 1.47f, 4}};                                                                                  // L clavicle
static const char* kMap[kBones] = {"hips", "spine", "chest", "neck", "head", "leftUpperArm", "leftLowerArm", "leftHand",
                                   "rightUpperArm", "rightLowerArm", "rightHand", "leftUpperLeg", "leftLowerLeg", "leftFoot",
                                   "leftToes", "rightUpperLeg", "rightLowerLeg", "rightFoot", "rightToes", "leftShoulder"};

static void setupFakeGame() {
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    g_bones = reinterpret_cast<M43*>(mem);
    g_camera = reinterpret_cast<M43*>(mem + 0x8000);
    *g_camera = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.6f, 1.3f, 0.6f}};
    FILE* f = _wfopen(L"RFVRoid.ini", L"wb");
    std::fprintf(f, "[General]\nDebug=1\n[GameSkeleton]\nBoneArray=0x%llx\nBoneCount=%d\n[GameCamera]\nTransform=0x%llx\nFovDefault=60\n[BoneMap]\n",
                 (unsigned long long)(uintptr_t)g_bones, kBones, (unsigned long long)(uintptr_t)g_camera);
    for (int i = 0; i < kBones; ++i) std::fprintf(f, "%s=%d\n", kMap[i], i);
    std::fclose(f);
}

static void animateFakeGame(int frame) {
    // Mason faces the camera (-Z); his head's forward axis (+z column) must point that way too.
    for (int i = 0; i < kBones; ++i) {
        g_bones[i] = {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}, {kJoints[i][0], kJoints[i][1], kJoints[i][2]}};
    }
    // Mason strafes side to side (about 1 m), so hair and skirts have something to react to.
    float sway = 0.5f * std::sin(frame * 0.12f);
    for (int i = 0; i < kBones; ++i) g_bones[i].p[0] += sway;
}

int wmain(int argc, wchar_t** argv) {
    int frames = argc > 1 ? _wtoi(argv[1]) : 90;
    const wchar_t* shot = argc > 2 ? argv[2] : L"smoke.bmp";
    std::wstring mode = argc > 3 ? argv[3] : L"menu";  // menu | pause | world
    SetEnvironmentVariableW(L"RFVROID_ANY_EXE", L"1");
    if (mode == L"world") setupFakeGame();
    // Ask the mod to capture the last frame from inside Present (after it has drawn).
    SetEnvironmentVariableW(L"RFVROID_CAPTURE_FRAME", std::to_wstring(frames).c_str());
    SetEnvironmentVariableW(L"RFVROID_CAPTURE_PATH", shot);

    HMODULE proxy = LoadLibraryW(L"dxgi.dll");
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(proxy, path, MAX_PATH);
    std::wprintf(L"loaded dxgi.dll from: %ls\n", path);
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto cf = reinterpret_cast<CreateFactory1>(GetProcAddress(proxy, "CreateDXGIFactory1"));
    IDXGIFactory1* fac = nullptr;
    HRESULT hr = cf ? cf(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&fac)) : E_FAIL;
    std::printf("CreateDXGIFactory1 through proxy: 0x%08lx %s\n", static_cast<unsigned long>(hr), fac ? "(factory ok)" : "");
    if (fac) fac->Release();
    if (FAILED(hr)) return 1;

    Sleep(3000);  // give the mod's init thread time to install its hooks

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RfvSmoke";
    RegisterClassExW(&wc);
    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"rfv smoke", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);

    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        GetProcAddress(LoadLibraryW(L"d3d11.dll"), "D3D11CreateDeviceAndSwapChain"));
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 1280;
    sd.BufferDesc.Height = 720;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    std::printf("game device: 0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 2;

    ID3D11Texture2D* back = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(back, nullptr, &rtv);

    // World mode draws a "scene" for the mod's scene lighting to pick up: cold blue light on
    // the left, warm orange on the right, bright sky above, dark ground below.
    ID3D11Texture2D* background = nullptr;
    if (mode == L"world") {
        std::vector<uint32_t> px(1280 * 720);
        for (int y = 0; y < 720; ++y)
            for (int x = 0; x < 1280; ++x) {
                float fx = x / 1279.0f, fy = 1.0f - y / 719.0f;
                float b = 0.25f + 0.75f * fy;
                int r = static_cast<int>(255 * b * (0.25f + 0.75f * fx));
                int g = static_cast<int>(255 * b * (0.45f + 0.15f * fx));
                int bl = static_cast<int>(255 * b * (1.0f - 0.8f * fx));
                wchar_t bg[16] = {};
                if (x == 0 && y == 0) GetEnvironmentVariableW(L"RFV_SMOKE_BG", bg, 16);
                static std::wstring theme = bg;
                if (theme == L"blue") { r = 40; g = 70; bl = 200; }
                else if (theme == L"orange") { r = 220; g = 120; bl = 40; }
                else if (theme == L"dark") { r = 15; g = 12; bl = 10; }
                px[y * 1280 + x] = 0xFF000000u | (bl << 16) | (g << 8) | r;
            }
        D3D11_TEXTURE2D_DESC bd;
        back->GetDesc(&bd);
        bd.BindFlags = 0;
        D3D11_SUBRESOURCE_DATA init{px.data(), 1280 * 4, 0};
        dev->CreateTexture2D(&bd, &init, &background);
    }

    for (int i = 0; i < frames; ++i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (mode == L"world") {
            animateFakeGame(i);
        } else if (mode == L"pause") {
            if (i == frames / 3) {
                PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
                PostMessageW(hwnd, WM_KEYUP, VK_ESCAPE, 0xC0000000);
            }
        } else if (i == frames / 3) {
            PostMessageW(hwnd, WM_KEYDOWN, VK_F7, 0);  // open the VRM menu
            PostMessageW(hwnd, WM_KEYUP, VK_F7, 0xC0000000);
        }
        if (mode == L"menu" && i == frames / 2) {  // move the selection down to the first .vrm ...
            PostMessageW(hwnd, WM_KEYDOWN, VK_DOWN, 0);
            PostMessageW(hwnd, WM_KEYUP, VK_DOWN, 0xC0000000);
        }
        if (mode == L"menu" && i == frames / 2 + 5) {  // ... and apply it
            PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);
            PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0xC0000000);
        }
        float clear[4] = {0.35f, 0.18f, 0.10f, 1.0f};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clear);
        if (background) ctx->CopyResource(back, background);
        sc->Present(0, 0);
        Sleep(16);
    }
    std::printf("done\n");
    return 0;
}
