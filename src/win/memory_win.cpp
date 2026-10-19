#include "memory_win.hpp"

#include <windows.h>

#include "paths.hpp"

namespace rfv::win {

ProcessMemory& processMemory() {
    static ProcessMemory m;
    return m;
}

bool ProcessMemory::read(uintptr_t address, void* out, size_t size) {
    if (address < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &got) && got == size;
}

bool ProcessMemory::write(uintptr_t address, const void* data, size_t size) {
    if (address < 0x10000) return false;
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(address), data, size, &done) && done == size;
}

uintptr_t ProcessMemory::moduleBase(const std::string& name) {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(widen(name).c_str()));
}

uintptr_t ProcessMemory::findPattern(const std::vector<int>& pattern) {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* start = base + sec->VirtualAddress;
        size_t len = sec->Misc.VirtualSize;
        if (len < pattern.size()) continue;
        for (size_t i = 0; i + pattern.size() <= len; ++i) {
            size_t k = 0;
            for (; k < pattern.size(); ++k)
                if (pattern[k] >= 0 && start[i + k] != static_cast<uint8_t>(pattern[k])) break;
            if (k == pattern.size()) return reinterpret_cast<uintptr_t>(start + i);
        }
    }
    return 0;
}

} // namespace rfv::win
