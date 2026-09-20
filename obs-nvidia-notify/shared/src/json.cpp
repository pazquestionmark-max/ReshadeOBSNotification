// SPDX-License-Identifier: MIT
#include "obsn/json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace obsn::json {

double canonical_float(float v) noexcept {
    if (!std::isfinite(v)) return static_cast<double>(v);
    // Nine significant digits always round-trip a binary32, so the loop is bounded and the
    // fallback is exact rather than approximate.
    char buffer[40];
    for (int precision = 1; precision <= 9; ++precision) {
        const int n = std::snprintf(buffer, sizeof(buffer), "%.*g", precision,
                                    static_cast<double>(v));
        if (n <= 0) break;
        if (std::strtof(buffer, nullptr) == v) return std::strtod(buffer, nullptr);
    }
    return static_cast<double>(v);
}

namespace {

const std::string kEmptyString;
const Array kEmptyArray;
const Object kEmptyObject;

bool is_ws(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    Parser(std::string_view text, const Limits& limits) : s_(text), lim_(limits) {}

    ParseResult run() {
        ParseResult r;
        if (s_.size() > lim_.max_total_bytes) {
            r.error = "document exceeds max_total_bytes";
            return r;
        }
        if (!is_valid_utf8(s_)) {
            r.error = "document is not valid UTF-8";
            return r;
        }
        skip_ws();
        Value v;
        if (!parse_value(v, 0)) {
            r.error = err_.empty() ? "parse error" : err_;
            r.error_offset = pos_;
            return r;
        }
        skip_ws();
        if (pos_ != s_.size()) {
            r.error = "trailing content after document";
            r.error_offset = pos_;
            return r;
        }
        r.ok = true;
        r.value = std::move(v);
        return r;
    }

private:
    std::string_view s_;
    Limits lim_;
    std::size_t pos_ = 0;
    std::string err_;

    bool fail(const char* m) {
        if (err_.empty()) err_ = m;
        return false;
    }
    void skip_ws() {
        while (pos_ < s_.size() && is_ws(s_[pos_])) ++pos_;
    }
    bool eof() const { return pos_ >= s_.size(); }
    char peek() const { return s_[pos_]; }

    bool literal(std::string_view lit) {
        if (s_.size() - pos_ < lit.size()) return false;
        if (s_.compare(pos_, lit.size(), lit) != 0) return false;
        pos_ += lit.size();
        return true;
    }

    bool parse_value(Value& out, std::size_t depth) {
        if (depth > lim_.max_depth) return fail("max_depth exceeded");
        if (eof()) return fail("unexpected end of input");
        switch (peek()) {
            case 'n':
                if (!literal("null")) return fail("invalid literal");
                out = Value();
                return true;
            case 't':
                if (!literal("true")) return fail("invalid literal");
                out = Value(true);
                return true;
            case 'f':
                if (!literal("false")) return fail("invalid literal");
                out = Value(false);
                return true;
            case '"': {
                std::string str;
                if (!parse_string(str)) return false;
                out = Value(std::move(str));
                return true;
            }
            case '[': return parse_array(out, depth);
            case '{': return parse_object(out, depth);
            default: return parse_number(out);
        }
    }

    bool parse_string(std::string& out) {
        if (eof() || peek() != '"') return fail("expected string");
        ++pos_;
        out.clear();
        while (true) {
            if (eof()) return fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) return fail("unescaped control character in string");
            if (out.size() > lim_.max_string_bytes) return fail("max_string_bytes exceeded");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            ++pos_;
            if (eof()) return fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate; a low surrogate must follow to form a valid pair.
                        if (pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t lo = 0;
                            if (!hex4(lo)) return false;
                            if (lo < 0xDC00 || lo > 0xDFFF) return fail("invalid low surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            return fail("unpaired high surrogate");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired low surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("invalid escape");
            }
        }
    }

    bool hex4(uint32_t& out) {
        if (s_.size() - pos_ < 4) return fail("truncated \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_ + static_cast<std::size_t>(i)];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("invalid hex digit in \\u escape");
        }
        pos_ += 4;
        out = v;
        return true;
    }

    bool parse_number(Value& out) {
        const std::size_t start = pos_;
        if (!eof() && peek() == '-') ++pos_;
        if (eof() || peek() < '0' || peek() > '9') return fail("invalid number");
        if (peek() == '0') {
            ++pos_;
        } else {
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        bool is_double = false;
        if (!eof() && peek() == '.') {
            is_double = true;
            ++pos_;
            if (eof() || peek() < '0' || peek() > '9') return fail("invalid fraction");
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            is_double = true;
            ++pos_;
            if (!eof() && (peek() == '+' || peek() == '-')) ++pos_;
            if (eof() || peek() < '0' || peek() > '9') return fail("invalid exponent");
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        const std::string text(s_.substr(start, pos_ - start));
        if (!is_double) {
            errno = 0;
            char* end = nullptr;
            const long long v = std::strtoll(text.c_str(), &end, 10);
            if (errno == 0 && end == text.c_str() + text.size()) {
                out = Value(v);
                return true;
            }
            // Integer literal that does not fit in 64 bits degrades to double rather than
            // failing the whole document; the field validators clamp it afterwards.
        }
        char* end = nullptr;
        const double d = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size()) return fail("invalid number");
        out = Value(d);
        return true;
    }

    bool parse_array(Value& out, std::size_t depth) {
        ++pos_;  // '['
        Array arr;
        skip_ws();
        if (!eof() && peek() == ']') {
            ++pos_;
            out = Value(std::move(arr));
            return true;
        }
        while (true) {
            if (arr.size() >= lim_.max_array_elements) return fail("max_array_elements exceeded");
            skip_ws();
            Value v;
            if (!parse_value(v, depth + 1)) return false;
            arr.push_back(std::move(v));
            skip_ws();
            if (eof()) return fail("unterminated array");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                out = Value(std::move(arr));
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parse_object(Value& out, std::size_t depth) {
        ++pos_;  // '{'
        Object obj;
        skip_ws();
        if (!eof() && peek() == '}') {
            ++pos_;
            out = Value(std::move(obj));
            return true;
        }
        while (true) {
            if (obj.size() >= lim_.max_object_members) return fail("max_object_members exceeded");
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (eof() || peek() != ':') return fail("expected ':'");
            ++pos_;
            skip_ws();
            Value v;
            if (!parse_value(v, depth + 1)) return false;
            obj.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (eof()) return fail("unterminated object");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                out = Value(std::move(obj));
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }
};

}  // namespace

bool Value::as_bool(bool fallback) const noexcept {
    return type_ == Type::Bool ? bool_ : fallback;
}

long long Value::as_int(long long fallback) const noexcept {
    if (type_ == Type::Int) return int_;
    if (type_ == Type::Double) {
        if (!std::isfinite(dbl_)) return fallback;
        return static_cast<long long>(dbl_);
    }
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    return fallback;
}

double Value::as_double(double fallback) const noexcept {
    if (type_ == Type::Double) return dbl_;
    if (type_ == Type::Int) return static_cast<double>(int_);
    if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
    return fallback;
}

const std::string& Value::as_string() const noexcept {
    return type_ == Type::String ? str_ : kEmptyString;
}

std::string Value::as_string_or(const std::string& fallback) const {
    return type_ == Type::String ? str_ : fallback;
}

const Array& Value::as_array() const noexcept {
    return type_ == Type::Array ? arr_ : kEmptyArray;
}

const Object& Value::as_object() const noexcept {
    return type_ == Type::Object ? obj_ : kEmptyObject;
}

Array& Value::array_ref() {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        arr_.clear();
    }
    return arr_;
}

Object& Value::object_ref() {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        obj_.clear();
    }
    return obj_;
}

const Value* Value::find(std::string_view key) const noexcept {
    if (type_ != Type::Object) return nullptr;
    for (const auto& m : obj_) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}

bool Value::get_bool(std::string_view key, bool fallback) const noexcept {
    const Value* v = find(key);
    return v ? v->as_bool(fallback) : fallback;
}

long long Value::get_int(std::string_view key, long long fallback) const noexcept {
    const Value* v = find(key);
    return v ? v->as_int(fallback) : fallback;
}

double Value::get_double(std::string_view key, double fallback) const noexcept {
    const Value* v = find(key);
    return v ? v->as_double(fallback) : fallback;
}

std::string Value::get_string(std::string_view key, std::string_view fallback) const {
    const Value* v = find(key);
    if (v && v->is_string()) return v->as_string();
    return std::string(fallback);
}

void Value::set(std::string key, Value v) {
    Object& o = object_ref();
    for (auto& m : o) {
        if (m.first == key) {
            m.second = std::move(v);
            return;
        }
    }
    o.emplace_back(std::move(key), std::move(v));
}

void Value::dump_to(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Int: {
            char buf[24];
            const int n = std::snprintf(buf, sizeof(buf), "%lld", int_);
            out.append(buf, static_cast<std::size_t>(n > 0 ? n : 0));
            break;
        }
        case Type::Double: {
            if (!std::isfinite(dbl_)) {
                // JSON has no representation for NaN/Inf; 0 is the only safe lossless-enough
                // choice that keeps the document parseable by any conformant reader.
                out += '0';
                break;
            }
            char buf[40];
            int n = std::snprintf(buf, sizeof(buf), "%.9g", dbl_);
            if (n < 0) n = 0;
            out.append(buf, static_cast<std::size_t>(n));
            break;
        }
        case Type::String: escape_string(str_, out); break;
        case Type::Array: {
            out += '[';
            bool first = true;
            for (const auto& e : arr_) {
                if (!first) out += ',';
                first = false;
                e.dump_to(out);
            }
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& m : obj_) {
                if (!first) out += ',';
                first = false;
                escape_string(m.first, out);
                out += ':';
                m.second.dump_to(out);
            }
            out += '}';
            break;
        }
    }
}

std::string Value::dump() const {
    std::string out;
    out.reserve(256);
    dump_to(out);
    return out;
}

ParseResult parse(std::string_view text, const Limits& limits) {
    Parser p(text, limits);
    return p.run();
}

bool is_valid_utf8(std::string_view in) noexcept {
    std::size_t i = 0;
    const std::size_t n = in.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::size_t len;
        uint32_t cp;
        if (c < 0x80) { len = 1; cp = c; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; }
        else return false;
        if (i + len > n) return false;
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        // Reject overlong encodings, surrogates and out-of-range code points.
        if (len == 2 && cp < 0x80) return false;
        if (len == 3 && cp < 0x800) return false;
        if (len == 4 && cp < 0x10000) return false;
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += len;
    }
    return true;
}

void escape_string(std::string_view in, std::string& out) {
    out += '"';
    std::size_t i = 0;
    const std::size_t n = in.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
            ++i;
            continue;
        }
        // Multi-byte: copy through only if the whole sequence is well-formed, otherwise emit
        // U+FFFD, so serialiser output is guaranteed-valid UTF-8 even for damaged input.
        std::size_t len = 0;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (len == 0 || i + len > n || !is_valid_utf8(in.substr(i, len))) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(in.substr(i, len));
            i += len;
        }
    }
    out += '"';
}

std::string truncate_utf8(std::string_view in, std::size_t max_chars) {
    std::size_t i = 0, chars = 0;
    const std::size_t n = in.size();
    while (i < n && chars < max_chars) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > n) break;
        i += len;
        ++chars;
    }
    return std::string(in.substr(0, i));
}

}  // namespace obsn::json
