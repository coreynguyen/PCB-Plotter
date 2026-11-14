#pragma once
// ============================================================================
// json.h — minimal JSON DOM (parse / serialize). Portable, no deps.
// Used for the native .pcbp project format and the plugin C ABI payloads.
// ============================================================================
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace pcbp::json {

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : t_(Type::Bool), b_(b) {}
    Value(int v) : t_(Type::Number), n_(v) {}
    Value(unsigned v) : t_(Type::Number), n_(v) {}
    Value(int64_t v) : t_(Type::Number), n_((double)v) {}
    Value(uint64_t v) : t_(Type::Number), n_((double)v) {}
    Value(double v) : t_(Type::Number), n_(v) {}
    Value(const char* s) : t_(Type::String), s_(s) {}
    Value(std::string s) : t_(Type::String), s_(std::move(s)) {}

    static Value array() { Value v; v.t_ = Type::Array; return v; }
    static Value object() { Value v; v.t_ = Type::Object; return v; }

    Type type() const { return t_; }
    bool is_null() const { return t_ == Type::Null; }
    bool is_object() const { return t_ == Type::Object; }
    bool is_array() const { return t_ == Type::Array; }

    // accessors with defaults (never throw)
    bool b(bool def = false) const { return t_ == Type::Bool ? b_ : (t_ == Type::Number ? n_ != 0 : def); }
    double num(double def = 0) const { return t_ == Type::Number ? n_ : (t_ == Type::Bool ? (b_ ? 1 : 0) : def); }
    int i(int def = 0) const { return t_ == Type::Number ? (int)n_ : def; }
    const std::string& str() const { static const std::string e; return t_ == Type::String ? s_ : e; }
    std::string str(const std::string& def) const { return t_ == Type::String ? s_ : def; }

    // array
    size_t size() const { return t_ == Type::Array ? a_.size() : (t_ == Type::Object ? o_.size() : 0); }
    const Value& operator[](size_t i) const;
    const Value& operator[](int i) const { return (*this)[(size_t)i]; }
    Value& push(Value v);
    const std::vector<Value>& items() const { return a_; }

    // object
    const Value& operator[](const std::string& k) const;
    const Value& operator[](const char* k) const { return (*this)[std::string(k)]; }
    Value& set(const std::string& k, Value v);
    bool has(const std::string& k) const;
    const std::vector<std::pair<std::string, Value>>& members() const { return o_; }

    std::string dump(int indent = -1) const;
    static bool parse(const std::string& text, Value& out, std::string& err);

private:
    void dump_to(std::string& out, int indent, int depth) const;
    Type t_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> a_;
    std::vector<std::pair<std::string, Value>> o_;
};

std::string escape(const std::string& s);

} // namespace pcbp::json
