// Tiny INI reader/writer. Keys are case-insensitive; section order and comments are not preserved on write.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace rfv {

class Ini {
public:
    bool loadFile(const std::string& utf8Path);
    void loadString(const std::string& text);
    bool saveFile(const std::string& utf8Path) const;

    std::string get(const std::string& section, const std::string& key, const std::string& def = "") const;
    float getFloat(const std::string& section, const std::string& key, float def) const;
    int getInt(const std::string& section, const std::string& key, int def) const;
    bool getBool(const std::string& section, const std::string& key, bool def) const;
    void set(const std::string& section, const std::string& key, const std::string& value);
    bool has(const std::string& section, const std::string& key) const;

    // All key/value pairs of a section in file order (original key spelling).
    std::vector<std::pair<std::string, std::string>> section(const std::string& name) const;

private:
    struct Entry { std::string key, value; };
    std::vector<std::pair<std::string, std::vector<Entry>>> sections_;
    std::vector<Entry>* findSection(const std::string& name);
    const std::vector<Entry>* findSection(const std::string& name) const;
};

std::string toLower(std::string s);
std::string trim(const std::string& s);
// Parses decimal or 0x-prefixed hexadecimal integers. Returns false if not a number.
bool parseInteger(const std::string& s, long long& out);

} // namespace rfv
