#pragma once
#include <windows.h>

#include <string>

namespace rfv::win {

// Called from DllMain. Returns true if this copy of the DLL should run the mod (the first
// copy loaded into the process); other copies only forward calls.
bool proxyAttach(HMODULE self);
bool proxyIsActiveInstance();
std::wstring proxySelfPath();

} // namespace rfv::win
