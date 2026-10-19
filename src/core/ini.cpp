#include "ini.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rfv {

std::string toLower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool parseInteger(const std::string& in, long long& out) {
    std::string s = trim(in);
    if (s.empty()) return false;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
    int base = 10;
    if (s.size() > i + 1 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }
    if (i >= s.size()) return false;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str() + i, &end, base);
    if (!end || *end != '\0') return false;
    out = neg ? -static_cast<long long>(v) : static_cast<long long>(v);
    return true;
}

static FILE* openUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    auto widen = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    };
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool Ini::loadFile(const std::string& path) {
    FILE* f = openUtf8(path, "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    loadString(text);
    return true;
}

void Ini::loadString(const std::string& text) {
    sections_.clear();
    sections_.push_back({"", {}});
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);  // BOM
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            sections_.push_back({trim(t.substr(1, t.size() - 2)), {}});
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string value = trim(t.substr(eq + 1));
        // Strip trailing " ; comment" (only when preceded by whitespace so paths with ';' survive).
        for (size_t i = 1; i < value.size(); ++i) {
            if ((value[i] == ';' || value[i] == '#') && (value[i - 1] == ' ' || value[i - 1] == '\t')) {
                value = trim(value.substr(0, i));
                break;
            }
        }
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        sections_.back().second.push_back({key, value});
    }
}

bool Ini::saveFile(const std::string& path) const {
    FILE* f = openUtf8(path, "wb");
    if (!f) return false;
    for (const auto& sec : sections_) {
        if (sec.second.empty()) continue;
        if (!sec.first.empty()) std::fprintf(f, "[%s]\r\n", sec.first.c_str());
        for (const auto& e : sec.second) std::fprintf(f, "%s=%s\r\n", e.key.c_str(), e.value.c_str());
        std::fprintf(f, "\r\n");
    }
    std::fclose(f);
    return true;
}

std::vector<Ini::Entry>* Ini::findSection(const std::string& name) {
    std::string ln = toLower(name);
    for (auto& s : sections_)
        if (toLower(s.first) == ln) return &s.second;
    return nullptr;
}

const std::vector<Ini::Entry>* Ini::findSection(const std::string& name) const {
    return const_cast<Ini*>(this)->findSection(name);
}

std::string Ini::get(const std::string& section, const std::string& key, const std::string& def) const {
    const auto* s = findSection(section);
    if (!s) return def;
    std::string lk = toLower(key);
    for (auto it = s->rbegin(); it != s->rend(); ++it)
        if (toLower(it->key) == lk) return it->value;
    return def;
}

bool Ini::has(const std::string& section, const std::string& key) const {
    const auto* s = findSection(section);
    if (!s) return false;
    std::string lk = toLower(key);
    for (const auto& e : *s)
        if (toLower(e.key) == lk) return true;
    return false;
}

float Ini::getFloat(const std::string& section, const std::string& key, float def) const {
    std::string v = get(section, key);
    if (v.empty()) return def;
    char* end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    return end == v.c_str() ? def : f;
}

int Ini::getInt(const std::string& section, const std::string& key, int def) const {
    long long v;
    return parseInteger(get(section, key), v) ? static_cast<int>(v) : def;
}

bool Ini::getBool(const std::string& section, const std::string& key, bool def) const {
    std::string v = toLower(get(section, key));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

void Ini::set(const std::string& section, const std::string& key, const std::string& value) {
    auto* s = findSection(section);
    if (!s) {
        sections_.push_back({section, {}});
        s = &sections_.back().second;
    }
    std::string lk = toLower(key);
    for (auto& e : *s)
        if (toLower(e.key) == lk) { e.value = value; return; }
    s->push_back({key, value});
}

std::vector<std::pair<std::string, std::string>> Ini::section(const std::string& name) const {
    std::vector<std::pair<std::string, std::string>> out;
    if (const auto* s = findSection(name))
        for (const auto& e : *s) out.emplace_back(e.key, e.value);
    return out;
}

} // namespace rfv
