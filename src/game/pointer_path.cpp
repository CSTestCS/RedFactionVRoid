#include "pointer_path.hpp"

#include <cctype>
#include <stdexcept>

#include "../core/ini.hpp"

namespace rfv::game {

struct PointerPath::Node {
    enum class Kind { Number, Module, Deref, Add, Sub, Sig } kind = Kind::Number;
    long long number = 0;
    std::string name;
    std::vector<int> pattern;
    long long ripOffset = -1;
    std::shared_ptr<Node> a, b;
    // Cache for module / signature lookups (stable for the process lifetime).
    mutable bool cached = false;
    mutable uintptr_t cache = 0;
};

std::vector<int> parsePattern(const std::string& text) {
    std::vector<int> out;
    size_t i = 0;
    while (i < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[i]))) { ++i; continue; }
        if (text[i] == '?') {
            out.push_back(-1);
            ++i;
            if (i < text.size() && text[i] == '?') ++i;
            continue;
        }
        if (i + 1 >= text.size() || !std::isxdigit(static_cast<unsigned char>(text[i])) ||
            !std::isxdigit(static_cast<unsigned char>(text[i + 1])))
            throw std::runtime_error("bad byte in pattern near '" + text.substr(i) + "'");
        out.push_back(static_cast<int>(std::stoi(text.substr(i, 2), nullptr, 16)));
        i += 2;
    }
    if (out.empty()) throw std::runtime_error("empty pattern");
    return out;
}

namespace {

using NodeP = std::shared_ptr<PointerPath::Node>;
using Kind = PointerPath::Node::Kind;

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    NodeP parseAll() {
        NodeP n = expr();
        ws();
        if (i_ != s_.size()) fail("unexpected '" + s_.substr(i_) + "'");
        return n;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    [[noreturn]] void fail(const std::string& m) { throw std::runtime_error("pointer path: " + m); }
    void ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
    bool peek(char c) { ws(); return i_ < s_.size() && s_[i_] == c; }
    void expect(char c) {
        if (!peek(c)) fail(std::string("expected '") + c + "'");
        ++i_;
    }

    NodeP expr() {
        NodeP left = term();
        for (;;) {
            if (peek('+') || peek('-')) {
                char op = s_[i_++];
                auto n = std::make_shared<PointerPath::Node>();
                n->kind = op == '+' ? Kind::Add : Kind::Sub;
                n->a = left;
                n->b = term();
                left = n;
            } else {
                return left;
            }
        }
    }

    NodeP term() {
        ws();
        if (i_ >= s_.size()) fail("unexpected end of expression");
        auto n = std::make_shared<PointerPath::Node>();
        if (s_[i_] == '[') {
            ++i_;
            n->kind = Kind::Deref;
            n->a = expr();
            expect(']');
            return n;
        }
        // identifier / number token
        size_t start = i_;
        while (i_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' || s_[i_] == '_')) ++i_;
        std::string tok = s_.substr(start, i_ - start);
        if (tok.empty()) fail("unexpected '" + s_.substr(start, 1) + "'");
        if (toLower(tok) == "sig") {
            expect('(');
            ws();
            if (i_ >= s_.size() || s_[i_] != '"') fail("sig() expects a quoted byte pattern");
            size_t q = s_.find('"', i_ + 1);
            if (q == std::string::npos) fail("unterminated string in sig()");
            n->kind = Kind::Sig;
            n->pattern = parsePattern(s_.substr(i_ + 1, q - i_ - 1));
            i_ = q + 1;
            if (peek(',')) {
                ++i_;
                ws();
                size_t ns = i_;
                while (i_ < s_.size() && std::isalnum(static_cast<unsigned char>(s_[i_]))) ++i_;
                long long v;
                if (!parseInteger(s_.substr(ns, i_ - ns), v)) fail("bad sig() offset");
                n->ripOffset = v;
            }
            expect(')');
            return n;
        }
        long long v;
        if (parseInteger(tok, v)) {
            n->kind = Kind::Number;
            n->number = v;
            return n;
        }
        std::string lt = toLower(tok);
        if (lt.size() > 4 && (lt.compare(lt.size() - 4, 4, ".exe") == 0 || lt.compare(lt.size() - 4, 4, ".dll") == 0)) {
            n->kind = Kind::Module;
            n->name = tok;
            return n;
        }
        fail("unknown token '" + tok + "' (numbers need a 0x prefix for hex; modules must end in .exe/.dll)");
    }
};

bool eval(const NodeP& n, MemoryAccess& mem, uintptr_t& out) {
    switch (n->kind) {
    case Kind::Number:
        out = static_cast<uintptr_t>(n->number);
        return true;
    case Kind::Module:
        if (!n->cached) { n->cache = mem.moduleBase(n->name); n->cached = n->cache != 0; }
        out = n->cache;
        return out != 0;
    case Kind::Sig: {
        if (!n->cached) {
            uintptr_t m = mem.findPattern(n->pattern);
            if (!m) return false;
            if (n->ripOffset >= 0) {
                int32_t disp;
                if (!mem.get(m + n->ripOffset, disp)) return false;
                m = m + n->ripOffset + 4 + disp;
            }
            n->cache = m;
            n->cached = true;
        }
        out = n->cache;
        return true;
    }
    case Kind::Deref: {
        uintptr_t addr;
        if (!eval(n->a, mem, addr)) return false;
        uint64_t v;
        if (!mem.get(addr, v) || v == 0) return false;
        out = static_cast<uintptr_t>(v);
        return true;
    }
    case Kind::Add:
    case Kind::Sub: {
        uintptr_t a, b;
        if (!eval(n->a, mem, a) || !eval(n->b, mem, b)) return false;
        out = n->kind == Kind::Add ? a + b : a - b;
        return true;
    }
    }
    return false;
}

} // namespace

PointerPath PointerPath::parse(const std::string& expr) {
    PointerPath p;
    p.text_ = trim(expr);
    if (p.text_.empty()) return p;
    Parser parser(p.text_);
    p.root_ = parser.parseAll();
    return p;
}

bool PointerPath::resolve(MemoryAccess& mem, uintptr_t& out) const {
    if (!root_) return false;
    return eval(root_, mem, out);
}

} // namespace rfv::game
