// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/headmodelfile.hpp"

#include <cstdio>

#include "bambi/patch/json.hpp"

namespace bambi {

namespace {
struct Reader {
    std::string* error;
    bool ok{true};

    bool fail(const std::string& what) {
        if (ok && error) *error = what;
        ok = false;
        return false;
    }
    const Json* object(const Json& in, const char* key) {
        const Json* v = in.find(key);
        if (v == nullptr || !v->isObject()) {
            fail(std::string("missing object: ") + key);
            return nullptr;
        }
        return v;
    }
    double number(const Json& in, const char* key) {
        const Json* v = in.find(key);
        if (v == nullptr || !v->isNumber()) {
            fail(std::string("missing number: ") + key);
            return 0;
        }
        return v->numberOr(0);
    }
    int integer(const Json& in, const char* key) { return static_cast<int>(number(in, key)); }
    void numbers(const Json& in, const char* key, std::vector<double>& out) {
        const Json* v = in.find(key);
        if (v == nullptr || !v->isArray()) {
            fail(std::string("missing array: ") + key);
            return;
        }
        out.clear();
        out.reserve(v->items().size());
        for (const Json& x : v->items()) {
            if (!x.isNumber()) {
                fail(std::string("not a number in: ") + key);
                return;
            }
            out.push_back(x.numberOr(0));
        }
    }
    void pair(const Json& in, const char* key, double& lo, double& hi) {
        std::vector<double> v;
        numbers(in, key, v);
        if (ok && v.size() != 2) {
            fail(std::string("not a pair: ") + key);
            return;
        }
        if (ok) {
            lo = v[0];
            hi = v[1];
        }
    }
    void table(const Json& in, const char* key, HeadModel::Table& t) {
        if (const Json* o = object(in, key)) {
            t.order = integer(*o, "order");
            numbers(*o, "coef", t.coef);
        }
    }
};
}  // namespace

bool parseHeadModel(std::string_view json, HeadModel& out, std::string* error) {
    const Json root = Json::parse(json, error);
    if (!root.isObject()) {
        if (error && error->empty()) *error = "not a JSON object";
        return false;
    }
    Reader r{error};
    if (r.integer(root, "version") != 1) return r.fail("not a version-1 head model");
    out.learnedFrom.clear();
    if (const Json* from = root.find("learnedFrom")) {
        if (from->isString()) out.learnedFrom = std::string(from->stringOr(""));
        for (const Json& name : from->items())
            out.learnedFrom += (out.learnedFrom.empty() ? "" : ", ") + std::string(name.stringOr(""));
    }
    if (const Json* a = r.object(root, "axis")) {
        out.axis.erb0 = r.number(*a, "erb0");
        out.axis.step = r.number(*a, "step");
        out.axis.points = r.integer(*a, "points");
    }
    r.table(root, "smooth", out.smooth);
    r.table(root, "ild", out.ild);
    if (const Json* ild = root.find("ild"); ild != nullptr && ild->isObject()) {
        out.ildWithSize.order = out.ild.order;
        r.numbers(*ild, "withSize", out.ildWithSize.coef);
    }
    if (const Json* itd = r.object(root, "itd")) {
        out.itd.order = r.integer(*itd, "order");
        r.numbers(*itd, "coef", out.itd.coef);
    }
    if (const Json* n = r.object(root, "notches")) {
        HeadModel::Notches& N = out.notches;
        N.order = r.integer(*n, "order");
        if (const Json* shape = n->find("shape"); shape == nullptr || shape->stringOr("") != "peak")
            r.fail("notches: only the peak shape is known");
        N.depthFactor = r.number(*n, "depthFactor");
        r.pair(*n, "fade", N.fadeIn, N.fadeOut);
        r.pair(*n, "band", N.bandLo, N.bandHi);
        r.pair(*n, "widthRange", N.widthMin, N.widthMax);
        const Json* list = n->find("list");
        if (list == nullptr || !list->isArray())
            r.fail("notches: missing list");
        else {
            N.list.clear();
            for (const Json& item : list->items()) {
                HeadModel::Notch& notch = N.list.emplace_back();
                r.numbers(item, "centre", notch.centre);
                r.numbers(item, "depth", notch.depth);
                r.numbers(item, "width", notch.width);
                r.numbers(item, "presence", notch.presence);
            }
        }
    }
    if (!r.ok) return false;
    return out.consistent(error);
}

namespace {
void appendNumber(std::string& out, double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {  // trailing zeros carry nothing
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    out += s;
}
void appendNumbers(std::string& out, const std::vector<double>& v, int decimals) {
    out += '[';
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ',';
        appendNumber(out, v[i], decimals);
    }
    out += ']';
}
}  // namespace

std::string headModelJson(const HeadModel& m, int decimals) {
    std::string out;
    out.reserve(16 * (m.smooth.coef.size() + 2 * m.ild.coef.size()) + 4096);
    out += "{\"version\":1,\"learnedFrom\":[";
    //  the names as a list, split where the parser joins them
    std::string_view from = m.learnedFrom;
    bool first = true;
    while (!from.empty()) {
        const std::size_t cut = from.find(", ");
        std::string_view name = from.substr(0, cut);
        if (!first) out += ',';
        first = false;
        out += '"';
        for (char c : name) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        out += '"';
        from = cut == std::string_view::npos ? std::string_view() : from.substr(cut + 2);
    }
    out +=
        "],\"convention\":\"spherical harmonics real, SN3D, ACN; x front, y left, z up; dB; the axis in ERB "
        "numbers: erb0 + k * step, f = (10^(e / 21.4) - 1) / 0.00437\",";
    out += "\"axis\":{\"erb0\":";
    appendNumber(out, m.axis.erb0, 6);
    out += ",\"step\":";
    appendNumber(out, m.axis.step, 6);
    out += ",\"points\":" + std::to_string(m.axis.points) + "},";
    out += "\"smooth\":{\"order\":" + std::to_string(m.smooth.order) + ",\"coef\":";
    appendNumbers(out, m.smooth.coef, decimals);
    out += "},\"ild\":{\"order\":" + std::to_string(m.ild.order) + ",\"coef\":";
    appendNumbers(out, m.ild.coef, decimals);
    out += ",\"withSize\":";
    appendNumbers(out, m.ildWithSize.coef, decimals);
    out += "},\"itd\":{\"order\":" + std::to_string(m.itd.order) + ",\"coef\":";
    appendNumbers(out, m.itd.coef, 1);
    const HeadModel::Notches& N = m.notches;
    out += "},\"notches\":{\"order\":" + std::to_string(N.order) + ",\"shape\":\"peak\",\"depthFactor\":";
    appendNumber(out, N.depthFactor, 6);
    out += ",\"fade\":";
    appendNumbers(out, {N.fadeIn, N.fadeOut}, 6);
    out += ",\"band\":";
    appendNumbers(out, {N.bandLo, N.bandHi}, 6);
    out += ",\"widthRange\":";
    appendNumbers(out, {N.widthMin, N.widthMax}, 6);
    out += ",\"list\":[";
    for (std::size_t i = 0; i < N.list.size(); ++i) {
        if (i > 0) out += ',';
        out += "{\"centre\":";
        appendNumbers(out, N.list[i].centre, decimals);
        out += ",\"depth\":";
        appendNumbers(out, N.list[i].depth, decimals);
        out += ",\"width\":";
        appendNumbers(out, N.list[i].width, decimals);
        out += ",\"presence\":";
        appendNumbers(out, N.list[i].presence, decimals);
        out += '}';
    }
    out += "]}}\n";
    return out;
}

}  // namespace bambi
