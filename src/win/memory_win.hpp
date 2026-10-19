// MemoryAccess for the current process: fault-tolerant reads/writes (via Read/WriteProcessMemory)
// and signature scanning of the game executable's image.
#pragma once
#include "../game/pointer_path.hpp"

namespace rfv::win {

class ProcessMemory : public game::MemoryAccess {
public:
    bool read(uintptr_t address, void* out, size_t size) override;
    bool write(uintptr_t address, const void* data, size_t size) override;
    uintptr_t moduleBase(const std::string& name) override;
    uintptr_t findPattern(const std::vector<int>& pattern) override;
};

ProcessMemory& processMemory();

} // namespace rfv::win
