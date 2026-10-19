#include "json.hpp"

#include <cstdlib>
#include <stdexcept>

namespace rfv::json {

const Value& Value::null() {
    static const Value v;
    return v;
}

const std::string& Value::str() const {
    static const std::string empty;
    return isString() ? string_ : empty;
}

size_t Value::size() const {
    if (isArray()) return array_.size();
    if (isObject()) return object_.size();
    return 0;
}

const Value& Value::operator[](size_t i) const {
    if (!isArray() || i >= array_.size()) return null();
    return array_[i];
}

const Value& Value::operator[](const std::string& key) const {
    if (!isObject()) return null();
    auto it = object_.find(key);
    return it == object_.end() ? null() : it->second;
}

bool Value::has(const std::string& key) const {
    return isObject() && object_.count(key) != 0;
}

namespace {

class Parser {
public:
    Parser(const char* d, size_t n) : p_(d), end_(d + n) {}

    Value parseDocument() {
        skipWs();
        Value v = parseValue(0);
        skipWs();
        if (p_ != end_) fail("trailing characters");
        return v;
    }

private:
    const char* p_;
    const char* end_;

    [[noreturn]] void fail(const char* msg) { throw std::runtime_error(std::string("JSON parse error: ") + msg); }

    void skipWs() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }

    bool consume(const char* lit) {
        const char* q = p_;
        for (; *lit; ++lit, ++q)
            if (q >= end_ || *q != *lit) return false;
        p_ = q;
        return true;
    }

    Value parseValue(int depth) {
        if (depth > 256) fail("nesting too deep");
        if (p_ >= end_) fail("unexpected end");
        switch (*p_) {
        case '{': return parseObject(depth);
        case '[': return parseArray(depth);
        case '"': return Value::makeString(parseString());
        case 't': if (consume("true")) return Value::makeBool(true); fail("bad literal");
        case 'f': if (consume("false")) return Value::makeBool(false); fail("bad literal");
        case 'n': if (consume("null")) return Value(); fail("bad literal");
        default: return parseNumber();
        }
    }

    Value parseObject(int depth) {
        ++p_; // {
        Value obj = Value::makeObject();
        skipWs();
        if (p_ < end_ && *p_ == '}') { ++p_; return obj; }
        for (;;) {
            skipWs();
            if (p_ >= end_ || *p_ != '"') fail("expected key");
            std::string key = parseString();
            skipWs();
            if (p_ >= end_ || *p_ != ':') fail("expected ':'");
            ++p_;
            skipWs();
            obj.objectRef()[key] = parseValue(depth + 1);
            skipWs();
            if (p_ >= end_) fail("unterminated object");
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == '}') { ++p_; return obj; }
            fail("expected ',' or '}'");
        }
    }

    Value parseArray(int depth) {
        ++p_; // [
        Value arr = Value::makeArray();
        skipWs();
        if (p_ < end_ && *p_ == ']') { ++p_; return arr; }
        for (;;) {
            skipWs();
            arr.arrayRef().push_back(parseValue(depth + 1));
            skipWs();
            if (p_ >= end_) fail("unterminated array");
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == ']') { ++p_; return arr; }
            fail("expected ',' or ']'");
        }
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    unsigned parseHex4() {
        if (end_ - p_ < 4) fail("bad \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i, ++p_) {
            char c = *p_;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else fail("bad hex digit");
        }
        return v;
    }

    std::string parseString() {
        ++p_; // "
        std::string out;
        while (p_ < end_ && *p_ != '"') {
            char c = *p_++;
            if (c != '\\') { out += c; continue; }
            if (p_ >= end_) fail("bad escape");
            char e = *p_++;
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = parseHex4();
                if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                    p_ += 2;
                    unsigned lo = parseHex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default: fail("unknown escape");
            }
        }
        if (p_ >= end_) fail("unterminated string");
        ++p_; // closing "
        return out;
    }

    Value parseNumber() {
        const char* start = p_;
        if (p_ < end_ && (*p_ == '-' || *p_ == '+')) ++p_;
        while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '-' || *p_ == '+')) ++p_;
        if (p_ == start) fail("unexpected character");
        std::string tmp(start, p_);
        char* endp = nullptr;
        double d = std::strtod(tmp.c_str(), &endp);
        if (endp == tmp.c_str()) fail("bad number");
        return Value::makeNumber(d);
    }
};

} // namespace

Value parse(const char* data, size_t len) {
    // Skip UTF-8 BOM if present.
    if (len >= 3 && static_cast<unsigned char>(data[0]) == 0xEF && static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data += 3;
        len -= 3;
    }
    Parser p(data, len);
    return p.parseDocument();
}

} // namespace rfv::json
