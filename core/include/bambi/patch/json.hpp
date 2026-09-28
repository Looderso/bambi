// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bambi {

/*  A small JSON value, sized for preset and plugin state and nothing else. Objects keep
 *  insertion order so a saved preset diffs cleanly in git.
 */
class Json {
public:
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Json() = default;
    explicit Json(bool b) : type_(Type::Bool), bool_(b) {}
    explicit Json(double n) : type_(Type::Number), num_(n) {}
    explicit Json(int n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    explicit Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    explicit Json(std::string_view s) : type_(Type::String), str_(s) {}
    explicit Json(const char* s) : type_(Type::String), str_(s) {}

    static Json object();
    static Json array();

    /// Returns a Null value and sets `error` when the text is not valid JSON.
    static Json parse(std::string_view text, std::string* error = nullptr);

    /// `indent` of 0 emits a single line.
    std::string dump(int indent = 2) const;

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Reads never throw. An absent or wrongly-typed value yields the fallback, which is
    // what makes loading an older or newer document a non-event rather than an error path.
    bool boolOr(bool def) const { return isBool() ? bool_ : def; }
    double numberOr(double def) const { return isNumber() ? num_ : def; }
    std::string_view stringOr(std::string_view def) const { return isString() ? str_ : def; }

    /// Object member, or nullptr when absent. The basis of forward/backward compatibility.
    const Json* find(std::string_view key) const;

    const std::vector<Json>& items() const { return arr_; }
    const std::vector<std::pair<std::string, Json>>& members() const { return obj_; }
    std::size_t size() const;

    void set(std::string_view key, Json v);  ///< object: replaces an existing key in place
    void push(Json v);                       ///< array
    void erase(std::string_view key);        ///< object: nothing happens when the key is absent

private:
    Type type_{Type::Null};
    bool bool_{};
    double num_{};
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

}  // namespace bambi
