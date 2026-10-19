#include "input.hpp"

#include <windowsx.h>
#include <xinput.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>

#include "../core/log.hpp"
#include "vtable_hook.hpp"

namespace rfv::win::input {

namespace {

HWND g_hwnd = nullptr;
WNDPROC g_origWndProc = nullptr;
UINT g_hotkey = VK_F7;
std::atomic<bool> g_capture{false};
std::atomic<bool> g_holdUntilRelease{false};
std::mutex g_mutex;
std::vector<Event> g_queue;
MouseState g_mouse;
std::set<UINT> g_keysDown;
std::atomic<UINT> g_bbW{0}, g_bbH{0};

// XInput
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xinputReal = nullptr;
std::atomic<WORD> g_padHeldMask{0};  // buttons to keep hiding from the game until released
WORD g_padPrev = 0;
DWORD g_padRepeatAt = 0;
int g_stickDirPrev = 0;

void pushLocked(Action a, bool pad) { g_queue.push_back({a, pad}); }

void push(Action a, bool pad) {
    std::lock_guard<std::mutex> lock(g_mutex);
    pushLocked(a, pad);
}

bool keyboardBlocked() { return g_capture || g_holdUntilRelease; }

// ---- Keyboard ------------------------------------------------------------------------
// Keys are detected two ways, so they work however the game reads its input (window
// messages, raw input with legacy messages disabled, DirectInput):
//   * WM_KEYDOWN / WM_KEYUP in our window procedure;
//   * polling the keyboard state once per frame (GetAsyncKeyState).
// g_latched[vk] makes sure one physical press produces exactly one event.
bool g_latched[256] = {};
DWORD g_heldSince[256] = {};
DWORD g_nextRepeat[256] = {};
int g_loggedKeys = 0;

const UINT kTrackedKeys[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'W', 'S', VK_RETURN, VK_SPACE, VK_ESCAPE, VK_BACK,
                             'V', VK_F9, VK_F10, VK_PRIOR, VK_NEXT, VK_INSERT, VK_DELETE};

void emitKeyLocked(UINT vk, bool repeat) {
    switch (vk) {
    case VK_UP: case 'W': pushLocked(Action::Up, false); break;
    case VK_DOWN: case 'S': pushLocked(Action::Down, false); break;
    case VK_LEFT: pushLocked(Action::Left, false); break;
    case VK_RIGHT: pushLocked(Action::Right, false); break;
    case VK_RETURN: case VK_SPACE: if (!repeat) pushLocked(Action::Accept, false); break;
    case VK_ESCAPE: if (!repeat) { pushLocked(Action::Back, false); pushLocked(Action::Pause, false); } break;
    case VK_BACK: if (!repeat) pushLocked(Action::Back, false); break;
    case 'V': if (!repeat) pushLocked(Action::OpenSkinMenu, false); break;
    case VK_F9: if (!repeat) pushLocked(Action::DebugToggle, false); break;
    case VK_F10: if (!repeat) pushLocked(Action::LoggerToggle, false); break;
    case VK_PRIOR: pushLocked(Action::PageUp, false); break;
    case VK_NEXT: pushLocked(Action::PageDown, false); break;
    case VK_INSERT: if (!repeat) pushLocked(Action::Insert, false); break;
    case VK_DELETE: if (!repeat) pushLocked(Action::Delete, false); break;
    default: break;
    }
    if (vk == g_hotkey && !repeat) pushLocked(Action::Hotkey, false);
}

void keyPressedLocked(UINT vk, const char* source) {
    if (vk >= 256 || g_latched[vk]) return;
    g_latched[vk] = true;
    g_heldSince[vk] = GetTickCount();
    g_nextRepeat[vk] = g_heldSince[vk] + 400;
    if (g_loggedKeys < 12 && (vk == g_hotkey || vk == VK_ESCAPE || vk == 'V')) {
        ++g_loggedKeys;
        RFV_LOG("Key 0x%02X pressed (seen via %s)", vk, source);
    }
    emitKeyLocked(vk, false);
}

void keyReleasedLocked(UINT vk) {
    if (vk < 256) g_latched[vk] = false;
}

bool gameHasFocus() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

bool anyTrackedKeyDown() {
    for (UINT vk : kTrackedKeys)
        if (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) return true;
    return (GetAsyncKeyState(static_cast<int>(g_hotkey)) & 0x8000) != 0;
}

void pollKeyboardLocked() {
    if (!gameHasFocus()) return;
    DWORD now = GetTickCount();
    auto check = [&](UINT vk) {
        bool down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
        if (down && !g_latched[vk]) keyPressedLocked(vk, "keyboard state");
        else if (!down && g_latched[vk]) keyReleasedLocked(vk);
        else if (down && (vk == VK_UP || vk == VK_DOWN || vk == 'W' || vk == 'S' || vk == VK_PRIOR || vk == VK_NEXT) &&
                 now >= g_nextRepeat[vk]) {
            emitKeyLocked(vk, true);  // auto-repeat while held
            g_nextRepeat[vk] = now + 90;
        }
    };
    for (UINT vk : kTrackedKeys) check(vk);
    bool trackedHotkey = false;
    for (UINT vk : kTrackedKeys) trackedHotkey |= vk == g_hotkey;
    if (!trackedHotkey && g_hotkey < 256) check(g_hotkey);
}

// Mouse: also polled, for games that hide button messages behind raw input.
bool g_mouseLatched = false;

void pollMouseLocked() {
    if (!g_hwnd || !gameHasFocus()) return;
    POINT pt;
    RECT rc;
    if (GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt) && GetClientRect(g_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
        float sx = g_bbW ? static_cast<float>(g_bbW) / rc.right : 1.0f;
        float sy = g_bbH ? static_cast<float>(g_bbH) / rc.bottom : 1.0f;
        float x = pt.x * sx, y = pt.y * sy;
        if (x != g_mouse.x || y != g_mouse.y) {
            g_mouse.x = x;
            g_mouse.y = y;
            g_mouse.moved = true;
        }
    }
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (down && !g_mouseLatched) { g_mouseLatched = true; g_mouse.clicked = true; }
    else if (!down) g_mouseLatched = false;
}

void updateMouse(LPARAM lp) {
    RECT rc;
    if (!GetClientRect(g_hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    float sx = g_bbW ? static_cast<float>(g_bbW) / rc.right : 1.0f;
    float sy = g_bbH ? static_cast<float>(g_bbH) / rc.bottom : 1.0f;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mouse.x = GET_X_LPARAM(lp) * sx;
    g_mouse.y = GET_Y_LPARAM(lp) * sy;
    g_mouse.moved = true;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = static_cast<UINT>(wp);
        bool repeat = (lp & (1 << 30)) != 0;
        bool wasBlocked = keyboardBlocked();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!repeat) keyPressedLocked(vk, "window message");
            if (wasBlocked) g_keysDown.insert(vk);
        }
        if (wasBlocked && vk != VK_F4) return 0;  // keep Alt+F4 working
        break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        bool blocked = keyboardBlocked();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_keysDown.erase(static_cast<UINT>(wp));
            keyReleasedLocked(static_cast<UINT>(wp));
        }
        if (blocked) return 0;
        break;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
        if (keyboardBlocked()) return 0;
        break;
    case WM_MOUSEMOVE:
        updateMouse(lp);
        if (g_capture) return 0;
        break;
    case WM_LBUTTONDOWN:
        updateMouse(lp);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_mouseLatched) { g_mouse.clicked = true; g_mouseLatched = true; }
        }
        if (g_capture) return 0;
        break;
    case WM_MOUSEWHEEL: {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_mouse.wheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (g_capture) return 0;
        break;
    }
    case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_LBUTTONDBLCLK:
        if (g_capture) return 0;
        break;
    case WM_INPUT:
        if (keyboardBlocked()) return DefWindowProcW(hwnd, msg, wp, lp);
        break;
    default:
        break;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

// ---- XInput -----------------------------------------------------------------------

XInputGetStateFn realXInput() {
    if (g_xinputReal) return g_xinputReal;
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        HMODULE m = GetModuleHandleW(dll);
        if (!m) m = LoadLibraryW(dll);
        if (m)
            if (auto f = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"))) return g_xinputReal = f;
    }
    return nullptr;
}

XInputGetStateFn g_gameXInput[3] = {};

DWORD filterPad(DWORD r, XINPUT_STATE* s) {
    if (r != ERROR_SUCCESS || !s) return r;
    if (g_capture) {
        std::memset(&s->Gamepad, 0, sizeof(s->Gamepad));
    } else if (WORD mask = g_padHeldMask.load()) {
        WORD still = s->Gamepad.wButtons & mask;
        g_padHeldMask = still;
        s->Gamepad.wButtons &= static_cast<WORD>(~mask);
    }
    return r;
}

DWORD WINAPI hkXInput0(DWORD i, XINPUT_STATE* s) { return filterPad(g_gameXInput[0](i, s), s); }
DWORD WINAPI hkXInput1(DWORD i, XINPUT_STATE* s) { return filterPad(g_gameXInput[1](i, s), s); }
DWORD WINAPI hkXInput2(DWORD i, XINPUT_STATE* s) { return filterPad(g_gameXInput[2](i, s), s); }

void pollPad() {
    XInputGetStateFn get = realXInput();
    if (!get) return;
    XINPUT_STATE st{};
    if (get(0, &st) != ERROR_SUCCESS) { g_padPrev = 0; return; }
    WORD b = st.Gamepad.wButtons;
    WORD pressed = b & static_cast<WORD>(~g_padPrev);
    if (g_capture) g_padHeldMask = static_cast<WORD>(g_padHeldMask | b);
    auto edge = [&](WORD bit, Action a) { if (pressed & bit) push(a, true); };
    edge(XINPUT_GAMEPAD_A, Action::Accept);
    edge(XINPUT_GAMEPAD_B, Action::Back);
    edge(XINPUT_GAMEPAD_START, Action::Pause);
    edge(XINPUT_GAMEPAD_Y, Action::OpenSkinMenu);
    edge(XINPUT_GAMEPAD_DPAD_LEFT, Action::Left);
    edge(XINPUT_GAMEPAD_DPAD_RIGHT, Action::Right);
    edge(XINPUT_GAMEPAD_LEFT_SHOULDER, Action::PageUp);
    edge(XINPUT_GAMEPAD_RIGHT_SHOULDER, Action::PageDown);

    // Up/down from d-pad or left stick, with key-repeat.
    int dir = 0;
    if (b & XINPUT_GAMEPAD_DPAD_UP || st.Gamepad.sThumbLY > 16000) dir = -1;
    else if (b & XINPUT_GAMEPAD_DPAD_DOWN || st.Gamepad.sThumbLY < -16000) dir = 1;
    DWORD now = GetTickCount();
    if (dir != 0 && (dir != g_stickDirPrev || now >= g_padRepeatAt)) {
        push(dir < 0 ? Action::Up : Action::Down, true);
        g_padRepeatAt = now + (dir != g_stickDirPrev ? 400 : 120);
    }
    g_stickDirPrev = dir;
    g_padPrev = b;
}

// ---- DirectInput ---------------------------------------------------------------------

VtableHook g_diStateW(9), g_diDataW(10), g_diStateA(9), g_diDataA(10);
using GetStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

template <VtableHook* H>
HRESULT STDMETHODCALLTYPE hkGetDeviceState(void* self, DWORD cb, LPVOID data) {
    HRESULT hr = H->orig<GetStateFn>(self)(self, cb, data);
    if (SUCCEEDED(hr) && data && keyboardBlocked()) std::memset(data, 0, cb);
    return hr;
}

template <VtableHook* H>
HRESULT STDMETHODCALLTYPE hkGetDeviceData(void* self, DWORD cb, LPDIDEVICEOBJECTDATA d, LPDWORD inOut, DWORD flags) {
    HRESULT hr = H->orig<GetDataFn>(self)(self, cb, d, inOut, flags);
    if (SUCCEEDED(hr) && inOut && keyboardBlocked() && !(flags & DIGDD_PEEK)) *inOut = 0;
    return hr;
}

void hookDirectInput() {
    HMODULE di = GetModuleHandleW(L"dinput8.dll");
    if (!di) return;  // the game does not use DirectInput
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(di, "DirectInput8Create"));
    if (!create) return;
    IDirectInput8W* w = nullptr;
    if (SUCCEEDED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&w), nullptr))) {
        IDirectInputDevice8W* dev = nullptr;
        if (SUCCEEDED(w->CreateDevice(GUID_SysKeyboard, &dev, nullptr))) {
            g_diStateW.install(dev, reinterpret_cast<void*>(&hkGetDeviceState<&g_diStateW>));
            g_diDataW.install(dev, reinterpret_cast<void*>(&hkGetDeviceData<&g_diDataW>));
            dev->Release();
        }
        w->Release();
    }
    IDirectInput8A* a = nullptr;
    if (SUCCEEDED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(&a), nullptr))) {
        IDirectInputDevice8A* dev = nullptr;
        if (SUCCEEDED(a->CreateDevice(GUID_SysKeyboard, &dev, nullptr))) {
            g_diStateA.install(dev, reinterpret_cast<void*>(&hkGetDeviceState<&g_diStateA>));
            g_diDataA.install(dev, reinterpret_cast<void*>(&hkGetDeviceData<&g_diDataA>));
            dev->Release();
        }
        a->Release();
    }
    RFV_LOG("DirectInput keyboard hooks installed");
}

} // namespace

void install(HWND hwnd, UINT hotkeyVk) {
    g_hotkey = hotkeyVk;
    if (!hwnd || hwnd == g_hwnd) return;
    if (g_hwnd && g_origWndProc) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
    g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndProc)));
    g_hwnd = hwnd;
    RFV_LOG("Window procedure hooked (window %p)", static_cast<void*>(hwnd));
}

void uninstall() {
    if (g_hwnd && g_origWndProc) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
    g_hwnd = nullptr;
}

void hookGameInput() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE exe = GetModuleHandleW(nullptr);
    void* detours[3] = {reinterpret_cast<void*>(&hkXInput0), reinterpret_cast<void*>(&hkXInput1), reinterpret_cast<void*>(&hkXInput2)};
    const char* dlls[3] = {"xinput1_4", "xinput1_3", "xinput9_1_0"};
    for (int i = 0; i < 3; ++i) {
        if (void* prev = patchImport(exe, dlls[i], "XInputGetState", detours[i])) {
            g_gameXInput[i] = reinterpret_cast<XInputGetStateFn>(prev);
            RFV_LOG("Hooked %s!XInputGetState", dlls[i]);
        }
    }
    hookDirectInput();
}

void setCapture(bool capture) {
    bool was = g_capture.exchange(capture);
    if (was && !capture) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_holdUntilRelease = !g_keysDown.empty() || anyTrackedKeyDown();
    }
}

bool capturing() { return g_capture; }

void setBackBufferSize(UINT w, UINT h) {
    g_bbW = w;
    g_bbH = h;
}

std::vector<Event> poll(MouseState& mouse) {
    pollPad();
    std::lock_guard<std::mutex> lock(g_mutex);
    pollKeyboardLocked();
    pollMouseLocked();
    if (g_holdUntilRelease && g_keysDown.empty() && !anyTrackedKeyDown()) g_holdUntilRelease = false;
    std::vector<Event> out;
    out.swap(g_queue);
    mouse = g_mouse;
    g_mouse.clicked = false;
    g_mouse.wheel = 0;
    g_mouse.moved = false;
    return out;
}

} // namespace rfv::win::input
