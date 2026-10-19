// dxgi.dll proxy: the game loads this file from its own folder instead of the system
// dxgi.dll. We load the real one (or a chained proxy such as ReShade renamed to
// dxgi_chain.dll) and forward every export to it, then start the mod.
#include <windows.h>

#include <string>

#include "paths.hpp"

extern "C" {
#define FORWARD(name) void* g_real_##name = nullptr;
#include "dxgi_export_list.h"
#undef FORWARD
}

namespace {

HMODULE g_realDxgi = nullptr;

extern "C" HRESULT WINAPI rfv_dxgi_not_implemented() { return E_NOTIMPL; }

} // namespace

namespace rfv::win {

bool loadRealDxgi() {
    if (g_realDxgi) return true;
    // 1) A chained proxy next to the game (installer renames an existing dxgi.dll to this).
    std::wstring chain = gameDirectoryW() + L"\\dxgi_chain.dll";
    if (GetFileAttributesW(chain.c_str()) != INVALID_FILE_ATTRIBUTES)
        g_realDxgi = LoadLibraryW(chain.c_str());
    // 2) The real system DLL.
    if (!g_realDxgi) {
        wchar_t sys[MAX_PATH];
        UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return false;
        std::wstring path = std::wstring(sys) + L"\\dxgi.dll";
        g_realDxgi = LoadLibraryW(path.c_str());
    }
    if (!g_realDxgi) return false;
#define FORWARD(name)                                                                    \
    g_real_##name = reinterpret_cast<void*>(GetProcAddress(g_realDxgi, #name));         \
    if (!g_real_##name) g_real_##name = reinterpret_cast<void*>(&rfv_dxgi_not_implemented);
#include "dxgi_export_list.h"
#undef FORWARD
    return true;
}

} // namespace rfv::win
