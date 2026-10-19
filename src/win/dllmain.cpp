// Entry point. The DLL is installed next to rfg.exe as d3d11.dll and dxgi.dll (see
// proxy.cpp); the first copy the game loads runs the mod. It also works when loaded by an
// ASI loader under another name.
#include <windows.h>

#include "mod.hpp"
#include "proxy.hpp"

// The installer looks for this string to recognise our DLL.
extern "C" __attribute__((used)) const char rfv_marker[] = "RFVRoid-dxgi-proxy " RFV_VERSION_STRING;

namespace {

DWORD WINAPI initThread(LPVOID) {
    rfv::win::Mod::get().start();
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        (void)*static_cast<const volatile char*>(rfv_marker);
        // No LoadLibrary here: the real DLLs are loaded lazily on the first forwarded call.
        if (rfv::win::proxyAttach(instance)) {
            HANDLE t = CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
