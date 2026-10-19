// Keyboard / mouse / gamepad input for the mod's menu, and blocking of game input while
// the menu has focus (window messages, XInput and DirectInput).
#pragma once
#include <windows.h>

#include <vector>

namespace rfv::win::input {

enum class Action {
    Up, Down, Left, Right,
    Accept,       // Enter / Space / A
    Back,         // Esc / Backspace / B
    Pause,        // Esc / Start (game pause toggle)
    OpenSkinMenu, // V / Y  (only meaningful on the pause menu)
    Hotkey,       // configurable (default F7): open skin menu anywhere
    DebugToggle,  // F9
    LoggerToggle, // F10
    PageUp, PageDown, Insert, Delete,
    Count
};

struct Event {
    Action action;
    bool fromGamepad;
};

struct MouseState {
    float x = -1, y = -1;   // in back-buffer pixels
    bool clicked = false;   // left button pressed since last poll
    int wheel = 0;          // notches since last poll (+ = up)
    bool moved = false;
};

void install(HWND gameWindow, UINT hotkeyVk);
void uninstall();
// While capturing, the game receives no keyboard/mouse/gamepad input. When capture is
// turned off, buttons still held stay blocked until released (prevents a "B" that
// closed our menu from also resuming the game).
void setCapture(bool capture);
bool capturing();
// Back-buffer size, for mouse coordinate scaling.
void setBackBufferSize(UINT w, UINT h);
// Events gathered since the last call (render thread). Polls the gamepad.
std::vector<Event> poll(MouseState& mouse);
// Hooks the game's XInput / DirectInput imports. Safe to call more than once.
void hookGameInput();

} // namespace rfv::win::input
