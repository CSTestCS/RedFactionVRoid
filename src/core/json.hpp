// Minimal JSON DOM parser (enough for glTF / VRM).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rfv::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    static const Value& null();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    double num(double def = 0) const { return isNumber() ? number_ : def; }
    int integer(int def = 0) const { return isNumber() ? static_cast<int>(number_) : def; }
    bool boolean(bool def = false) const { return isBool() ? bool_ : def; }
    const std::string& str() const;
    std::string str(const std::string& def) const { return isString() ? string_ : def; }

    size_t size() const;
    const Value& operator[](size_t i) const;
    const Value& operator[](int i) const { return i < 0 ? null() : (*this)[static_cast<size_t>(i)]; }
    const Value& operator[](const std::string& key) const;
    const Value& operator[](const char* key) const { return (*this)[std::string(key)]; }
    bool has(const std::string& key) const;
    const std::vector<Value>& items() const { return array_; }
    const std::map<std::string, Value>& members() const { return object_; }

    // Builders (used by the parser).
    static Value makeBool(bool b) { Value v; v.type_ = Type::Bool; v.bool_ = b; return v; }
    static Value makeNumber(double d) { Value v; v.type_ = Type::Number; v.number_ = d; return v; }
    static Value makeString(std::string s) { Value v; v.type_ = Type::String; v.string_ = std::move(s); return v; }
    static Value makeArray() { Value v; v.type_ = Type::Array; return v; }
    static Value makeObject() { Value v; v.type_ = Type::Object; return v; }
    std::vector<Value>& arrayRef() { return array_; }
    std::map<std::string, Value>& objectRef() { return object_; }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Value> array_;
    std::map<std::string, Value> object_;
};

// Throws std::runtime_error on malformed input.
Value parse(const char* data, size_t len);
inline Value parse(const std::string& s) { return parse(s.data(), s.size()); }

} // namespace rfv::json
