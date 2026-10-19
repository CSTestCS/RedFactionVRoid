#include "vtable_hook.hpp"

#include <windows.h>

#include <cstring>

#include "../core/ini.hpp"

namespace rfv::win {

static bool writePointer(void** where, void* value) {
    DWORD old;
    if (!VirtualProtect(where, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *where = value;
    VirtualProtect(where, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), where, sizeof(void*));
    return true;
}

bool VtableHook::install(void* obj, void* detour) {
    if (!obj) return false;
    void** vtable = *reinterpret_cast<void***>(obj);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : patched_)
        if (p.first == vtable) return true;
    void* orig = vtable[slot_];
    if (orig == detour) return true;
    if (!writePointer(&vtable[slot_], detour)) return false;
    detour_ = detour;
    patched_.emplace_back(vtable, orig);
    return true;
}

void* VtableHook::original(void* obj) const {
    void** vtable = *reinterpret_cast<void***>(obj);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : patched_)
        if (p.first == vtable) return p.second;
    // Object whose class we never patched but that reached our detour anyway (another
    // hooking tool copied our pointer): fall back to the first original we saw.
    return patched_.empty() ? nullptr : patched_.front().second;
}

void VtableHook::uninstallAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : patched_)
        if (p.first[slot_] == detour_) writePointer(&p.first[slot_], p.second);
    patched_.clear();
}

void* patchImport(void* module, const char* dllSub, const char* function, void* detour) {
    auto base = static_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    std::string want = toLower(dllSub);
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        std::string name = toLower(reinterpret_cast<const char*>(base + imp->Name));
        if (name.find(want) == std::string::npos) continue;
        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        auto orig = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; orig->u1.AddressOfData; ++orig, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), function) != 0) continue;
            void* prev = reinterpret_cast<void*>(thunk->u1.Function);
            if (!writePointer(reinterpret_cast<void**>(&thunk->u1.Function), detour)) return nullptr;
            return prev;
        }
    }
    return nullptr;
}

} // namespace rfv::win
