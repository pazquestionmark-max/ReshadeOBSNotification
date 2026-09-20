// SPDX-License-Identifier: MIT
// Minimal, bounded JSON value / parser / serialiser.
//
// Deliberately hand-written rather than vendored: this code is loaded into the user's game
// process and into OBS, and the message format is one we define ourselves. Every recursion
// and every allocation here is bounded by `Limits` so that a malformed or hostile peer cannot
// exhaust memory or blow the stack.
#ifndef OBSN_JSON_HPP
#define OBSN_JSON_HPP

#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <string>
#include <utility>
#include <vector>

namespace obsn::json {

enum class Type { Null, Bool, Int, Double, String, Array, Object };

/// The double nearest the shortest decimal string that round-trips back to `v` as a float.
/// See the float constructor below for why this exists.
double canonical_float(float v) noexcept;

class Value;
using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
/// Insertion-ordered. Objects in this protocol have a handful of members, so a flat vector beats
/// a tree both in lookup time and in allocation count, and it keeps output stable for tests.
using Object = std::vector<Member>;

class Value {
public:
    Value() noexcept : type_(Type::Null) {}
    Value(std::nullptr_t) noexcept : type_(Type::Null) {}
    Value(bool b) noexcept : type_(Type::Bool), bool_(b) {}
    // Constrained rather than overloaded per width: std::int64_t is `long` on LP64 and
    // `long long` on Windows, so a fixed overload set is ambiguous on one platform or the other.
    template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Value(T v) noexcept : type_(Type::Int), int_(static_cast<long long>(v)) {}
    // A float is canonicalised on the way in. Widening 0.55f to a double gives
    // 0.550000011920928955, which serialises as "0.550000012" and makes every configuration
    // file look like it was written by a machine that does not know what 0.55 is. Rounding to
    // the shortest decimal that still round-trips *as a float* costs one conversion here and
    // makes the whole file readable, without losing a single representable value.
    template <typename T, std::enable_if_t<std::is_same_v<T, float>, int> = 0>
    Value(T v) noexcept : type_(Type::Double), dbl_(canonical_float(v)) {}
    template <typename T, std::enable_if_t<std::is_floating_point_v<T> &&
                                               !std::is_same_v<T, float>, int> = 0>
    Value(T v) noexcept : type_(Type::Double), dbl_(static_cast<double>(v)) {}
    Value(const char* s) : type_(Type::String), str_(s ? s : "") {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Value(Array a) : type_(Type::Array), arr_(std::move(a)) {}
    Value(Object o) : type_(Type::Object), obj_(std::move(o)) {}

    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::Null; }
    bool is_bool() const noexcept { return type_ == Type::Bool; }
    bool is_number() const noexcept { return type_ == Type::Int || type_ == Type::Double; }
    bool is_int() const noexcept { return type_ == Type::Int; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_array() const noexcept { return type_ == Type::Array; }
    bool is_object() const noexcept { return type_ == Type::Object; }

    // Accessors never throw. A type mismatch yields the supplied fallback, because every caller
    // in this codebase is parsing untrusted input and "absent or wrong type" is the same
    // recoverable situation as "absent".
    bool as_bool(bool fallback = false) const noexcept;
    long long as_int(long long fallback = 0) const noexcept;
    double as_double(double fallback = 0.0) const noexcept;
    const std::string& as_string() const noexcept;
    std::string as_string_or(const std::string& fallback) const;
    const Array& as_array() const noexcept;
    const Object& as_object() const noexcept;

    Array& array_ref();
    Object& object_ref();

    /// Object member lookup. Returns nullptr when absent or when this is not an object.
    const Value* find(std::string_view key) const noexcept;
    bool has(std::string_view key) const noexcept { return find(key) != nullptr; }

    /// Convenience typed getters used pervasively by the decoders.
    bool get_bool(std::string_view key, bool fallback = false) const noexcept;
    long long get_int(std::string_view key, long long fallback = 0) const noexcept;
    double get_double(std::string_view key, double fallback = 0.0) const noexcept;
    std::string get_string(std::string_view key, std::string_view fallback = {}) const;

    /// Sets (or replaces) an object member. Converts a null Value into an object first.
    void set(std::string key, Value v);

    std::string dump() const;
    void dump_to(std::string& out) const;

private:
    Type type_;
    bool bool_ = false;
    long long int_ = 0;
    double dbl_ = 0.0;
    std::string str_;
    Array arr_;
    Object obj_;
};

struct Limits {
    std::size_t max_depth = 16;
    std::size_t max_array_elements = 4096;
    std::size_t max_object_members = 256;
    std::size_t max_string_bytes = 8192;
    std::size_t max_total_bytes = 65536;
};

struct ParseResult {
    bool ok = false;
    Value value;
    std::string error;       ///< Human-readable, safe to log: never contains payload content.
    std::size_t error_offset = 0;
};

/// Parses a complete JSON document. Trailing whitespace is allowed; trailing content is not.
ParseResult parse(std::string_view text, const Limits& limits = {});

/// Escapes and appends a JSON string literal (including quotes). Invalid UTF-8 sequences are
/// replaced with U+FFFD rather than emitted raw, so output is always valid UTF-8.
void escape_string(std::string_view in, std::string& out);

/// True if `in` is well-formed UTF-8.
bool is_valid_utf8(std::string_view in) noexcept;

/// Truncates to at most `max_chars` Unicode code points without splitting a sequence.
std::string truncate_utf8(std::string_view in, std::size_t max_chars);

}  // namespace obsn::json

#endif  // OBSN_JSON_HPP
