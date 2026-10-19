// Entry point. The DLL is normally loaded as the game's dxgi.dll (see dxgi_proxy.cpp);
// it also works when loaded by an ASI loader under another name.
#include <windows.h>

#include <cwchar>

#include "mod.hpp"

namespace rfv::win {
bool loadRealDxgi();
}

// The installer looks for this string to recognise our dxgi.dll.
extern "C" __attribute__((used)) const char rfv_marker[] = "RFVRoid-dxgi-proxy " RFV_VERSION_STRING;

namespace {

bool isGameProcess() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    if (_wcsicmp(name, L"rfg.exe") == 0) return true;
    // Escape hatch for renamed executables / testing.
    wchar_t env[8];
    return GetEnvironmentVariableW(L"RFVROID_ANY_EXE", env, 8) > 0;
}

DWORD WINAPI initThread(LPVOID) {
    rfv::win::Mod::get().start();
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        (void)*static_cast<const volatile char*>(rfv_marker);
        // Must happen before anyone calls our dxgi exports.
        rfv::win::loadRealDxgi();
        if (isGameProcess()) {
            HANDLE t = CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
