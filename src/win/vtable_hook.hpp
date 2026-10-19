// COM vtable slot patching. Vtables are shared by every object of a class, so patching
// one object's vtable hooks all of them. A slot can be patched on several distinct vtables
// (e.g. if the runtime uses different classes for different devices); the original is
// looked up per object.
#pragma once
#include <mutex>
#include <utility>
#include <vector>

namespace rfv::win {

class VtableHook {
public:
    explicit VtableHook(int slot) : slot_(slot) {}
    // Patches obj's vtable slot to `detour` (no-op if this vtable is already patched).
    bool install(void* obj, void* detour);
    // Original function for this object (nullptr if its vtable was never patched).
    void* original(void* obj) const;
    template <typename Fn> Fn orig(void* obj) const { return reinterpret_cast<Fn>(original(obj)); }
    void uninstallAll();

private:
    int slot_;
    void* detour_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<std::pair<void**, void*>> patched_;  // vtable -> original
};

// Replaces an imported function pointer in a module's IAT. Returns the previous pointer.
void* patchImport(void* module, const char* dllNameSubstring, const char* function, void* detour);

} // namespace rfv::win
