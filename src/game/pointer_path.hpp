// Pointer-path expressions used in RFVRoid.ini to locate game data, e.g.
//
//   [[rfg.exe+0x1A2B3C0]+0x58]+0x120          Cheat-Engine style: [x] dereferences a pointer
//   [sig("48 8B 0D ?? ?? ?? ?? 48 85 C9", 3)]+0x10
//
// sig("bytes", N) scans the main module for the byte pattern (?? = wildcard). With N it
// resolves the RIP-relative 32-bit displacement at match+N (target = match+N+4+disp32),
// which is how x64 code references globals; without N it yields the match address.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rfv::game {

class MemoryAccess {
public:
    virtual ~MemoryAccess() = default;
    virtual bool read(uintptr_t address, void* out, size_t size) = 0;
    virtual bool write(uintptr_t address, const void* data, size_t size) = 0;
    virtual uintptr_t moduleBase(const std::string& name) = 0;
    // Returns 0 if not found. pattern entries are 0..255 or -1 for wildcard.
    virtual uintptr_t findPattern(const std::vector<int>& pattern) = 0;

    template <typename T> bool get(uintptr_t a, T& v) { return read(a, &v, sizeof(T)); }
};

std::vector<int> parsePattern(const std::string& text);  // throws on bad syntax

class PointerPath {
public:
    PointerPath() = default;
    // Throws std::runtime_error with a readable message on syntax errors.
    static PointerPath parse(const std::string& expr);

    bool empty() const { return !root_; }
    const std::string& text() const { return text_; }
    // Evaluates the expression. Returns false if a dereference or lookup failed.
    bool resolve(MemoryAccess& mem, uintptr_t& out) const;

    struct Node;

private:
    std::shared_ptr<Node> root_;
    std::string text_;
};

} // namespace rfv::game
