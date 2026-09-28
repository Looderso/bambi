// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The Json value type is ours — it is a handful of accessors shaped for this project's
//  schema. Parsing and serialising are yyjson's (MIT), because a recursive-descent parser
//  written by hand is exactly the kind of code that harbours quiet bugs, and this one is
//  reading files users hand-edit.

#include "bambi/patch/json.hpp"

#include <cmath>

#include "yyjson.h"

namespace bambi {
namespace {

//  yyjson parses iteratively, so it has no stack limit of its own -- but this conversion
//  walk recurses, so the limit has to live here. Same value the hand-rolled parser used:
//  no legitimate bambi document comes close, and a file that does is hostile.
constexpr int kMaxDepth = 64;

Json fromYy(yyjson_val* v, int depth, bool& tooDeep) {
    if (v == nullptr) return {};
    if (depth > kMaxDepth) {
        tooDeep = true;
        return {};
    }
    switch (yyjson_get_type(v)) {
        case YYJSON_TYPE_BOOL: return Json(yyjson_get_bool(v));
        case YYJSON_TYPE_NUM: return Json(yyjson_get_num(v));
        case YYJSON_TYPE_STR: return Json(std::string(yyjson_get_str(v), yyjson_get_len(v)));
        case YYJSON_TYPE_ARR: {
            Json a = Json::array();
            yyjson_val* item;
            yyjson_arr_iter it = yyjson_arr_iter_with(v);
            while ((item = yyjson_arr_iter_next(&it))) a.push(fromYy(item, depth + 1, tooDeep));
            return a;
        }
        case YYJSON_TYPE_OBJ: {
            Json o = Json::object();
            yyjson_val *key, *val;
            yyjson_obj_iter it = yyjson_obj_iter_with(v);
            while ((key = yyjson_obj_iter_next(&it))) {
                val = yyjson_obj_iter_get_val(key);
                o.set(std::string(yyjson_get_str(key), yyjson_get_len(key)), fromYy(val, depth + 1, tooDeep));
            }
            return o;
        }
        default: return {};
    }
}

yyjson_mut_val* toYy(yyjson_mut_doc* doc, const Json& j) {
    switch (j.type()) {
        case Json::Type::Null: return yyjson_mut_null(doc);
        case Json::Type::Bool: return yyjson_mut_bool(doc, j.boolOr(false));
        case Json::Type::Number: {
            const double d = j.numberOr(0.0);
            return yyjson_mut_real(doc, std::isfinite(d) ? d : 0.0);
        }
        case Json::Type::String: {
            const auto s = j.stringOr("");
            return yyjson_mut_strncpy(doc, s.data(), s.size());
        }
        case Json::Type::Array: {
            yyjson_mut_val* a = yyjson_mut_arr(doc);
            for (const auto& item : j.items()) yyjson_mut_arr_add_val(a, toYy(doc, item));
            return a;
        }
        case Json::Type::Object: {
            yyjson_mut_val* o = yyjson_mut_obj(doc);
            for (const auto& [k, v] : j.members())
                yyjson_mut_obj_add(o, yyjson_mut_strncpy(doc, k.data(), k.size()), toYy(doc, v));
            return o;
        }
    }
    return yyjson_mut_null(doc);
}

}  // namespace

Json Json::object() {
    Json j;
    j.type_ = Type::Object;
    return j;
}
Json Json::array() {
    Json j;
    j.type_ = Type::Array;
    return j;
}

const Json* Json::find(std::string_view key) const {
    if (!isObject()) return nullptr;
    for (const auto& kv : obj_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

std::size_t Json::size() const {
    if (isArray()) return arr_.size();
    if (isObject()) return obj_.size();
    return 0;
}

void Json::set(std::string_view key, Json v) {
    if (!isObject()) *this = object();
    for (auto& kv : obj_)
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    obj_.emplace_back(std::string(key), std::move(v));
}

void Json::erase(std::string_view key) {
    std::erase_if(obj_, [key](const auto& kv) { return kv.first == key; });
}

void Json::push(Json v) {
    if (!isArray()) *this = array();
    arr_.push_back(std::move(v));
}

Json Json::parse(std::string_view text, std::string* error) {
    if (error) error->clear();
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(text.data()), text.size(), 0, nullptr, &err);
    if (doc == nullptr) {
        if (error) *error = std::string(err.msg ? err.msg : "parse error") + " at byte " + std::to_string(err.pos);
        return {};
    }
    bool tooDeep = false;
    Json out = fromYy(yyjson_doc_get_root(doc), 0, tooDeep);
    yyjson_doc_free(doc);
    if (tooDeep) {
        if (error) *error = "nesting too deep";
        return {};
    }
    return out;
}

std::string Json::dump(int indent) const {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_doc_set_root(doc, toYy(doc, *this));
    const yyjson_write_flag flags = indent > 0 ? YYJSON_WRITE_PRETTY_TWO_SPACES : YYJSON_WRITE_NOFLAG;
    std::size_t len = 0;
    char* text = yyjson_mut_write(doc, flags, &len);
    std::string out = text ? std::string(text, len) : std::string{};
    if (text) free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

}  // namespace bambi
