// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-features — batch feature analysis with separability measurement.
//
//  Exists to answer one question: do the C++ detectors reproduce the numbers the Python
//  reference produced on the same material? A disagreement means one of them is wrong, and
//  finding out which takes minutes instead of a session.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "bambi/dsp/features.hpp"
#include "bambi/io/wav.hpp"

using namespace bambi;
namespace fs = std::filesystem;

namespace {

constexpr const char* kNames[] = {"level", "attack", "tonal", "low", "mid", "high"};

/// P(a random positive scores above a random negative). 0.5 useless, 1.0 perfect.
double auc(std::vector<double> pos, std::vector<double> neg) {
    if (pos.empty() || neg.empty()) return std::nan("");
    std::vector<std::pair<double, int>> all;
    for (double v : pos) all.emplace_back(v, 1);
    for (double v : neg) all.emplace_back(v, 0);
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    double rankSum = 0.0;
    for (std::size_t i = 0; i < all.size();) {  // average ranks over ties
        std::size_t j = i;
        while (j < all.size() && all[j].first == all[i].first) ++j;
        const double avgRank = (static_cast<double>(i + j + 1)) * 0.5;
        for (std::size_t k = i; k < j; ++k)
            if (all[k].second == 1) rankSum += avgRank;
        i = j;
    }
    const double n1 = static_cast<double>(pos.size()), n2 = static_cast<double>(neg.size());
    return (rankSum - n1 * (n1 + 1) * 0.5) / (n1 * n2);
}

struct FileSummary {
    std::array<double, kNumFeatures> mean{};
};

bool summarise(const fs::path& p, double seconds, FileSummary& out) {
    AudioBuffer buf;
    if (!readWav(p.string(), buf)) return false;
    std::vector<float> mono = buf.mono();
    /*  seconds * 48000 frames, not seconds * this file's rate. That is what the Python
     *  reference does (`sf.read(frames=int(seconds * 48000))`), so a 44.1k file gets 8.7 s
     *  rather than 8. It is a quirk, but cross-validation compares two implementations on
     *  identical input or it compares nothing, and the reference is the one with the
     *  published numbers. */
    const auto limit = static_cast<std::size_t>(seconds * 48000.0);
    if (mono.size() > limit) mono.resize(limit);
    if (mono.size() < 4096) return false;

    const auto rows = FeatureBank::analyseOffline(mono, buf.sampleRate);
    std::array<double, kNumFeatures> acc{};
    int gated = 0;
    for (const auto& r : rows) {
        if (r.back() <= 0.5) continue;  // gated frames only, as the reference did
        for (int f = 0; f < kNumFeatures; ++f) acc[static_cast<std::size_t>(f)] += r[static_cast<std::size_t>(f)];
        ++gated;
    }
    if (gated < 4) return false;
    for (int f = 0; f < kNumFeatures; ++f)
        out.mean[static_cast<std::size_t>(f)] = acc[static_cast<std::size_t>(f)] / gated;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf(
            "bambi-features <library-root> [--limit N] [--seconds S]\n\n"
            "  Categories are the immediate subdirectories. Prints per-category\n"
            "  means and the Drums-vs-Tonal separability of each feature.\n");
        return argc < 2 ? 1 : 0;
    }
    const fs::path root = argv[1];
    int limit = 0;
    double seconds = 8.0;
    std::string extFilter;  // empty = every readable format
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--limit" && i + 1 < argc)
            limit = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc)
            seconds = std::atof(argv[++i]);
        else if (a == "--ext" && i + 1 < argc)
            extFilter = argv[++i];
    }

    std::map<std::string, std::vector<fs::path>> byCat;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        /*  A library that holds both an original and a converted copy of the same audio
         *  otherwise weighs those files twice. That is not a hypothetical: converting this
         *  library's AIFFs in place took the Tonal count from 66 to 89 and moved every AUC. */
        if (!extFilter.empty()) {
            if (ext != extFilter) continue;
        } else if (ext != ".wav" && ext != ".aif" && ext != ".aiff" && ext != ".flac") {
            continue;
        }
        const auto rel = fs::relative(it->path(), root);
        byCat[rel.begin()->string()].push_back(it->path());
    }
    if (byCat.empty()) {
        std::fprintf(stderr, "no audio under %s\n", root.string().c_str());
        return 1;
    }

    std::map<std::string, std::vector<FileSummary>> rows;
    for (auto& [cat, files] : byCat) {
        std::sort(files.begin(), files.end());
        if (limit > 0 && static_cast<int>(files.size()) > limit) {
            std::mt19937 rng(11);
            std::shuffle(files.begin(), files.end(), rng);
            files.resize(static_cast<std::size_t>(limit));
        }
        for (const auto& f : files) {
            FileSummary s;
            if (summarise(f, seconds, s)) rows[cat].push_back(s);
        }
        std::fprintf(stderr, "  %-10s %4zu files\n", cat.c_str(), rows[cat].size());
    }

    std::printf("\nMEAN over gated frames, averaged per category\n\n");
    std::printf("%-10s", "");
    for (const auto& [cat, v] : rows) std::printf("%12.10s", cat.c_str());
    std::printf("\n");
    for (int f = 0; f < kNumFeatures; ++f) {
        std::printf("%-10s", kNames[f]);
        for (const auto& [cat, v] : rows) {
            double m = 0;
            for (const auto& s : v) m += s.mean[static_cast<std::size_t>(f)];
            std::printf("%12.3f", v.empty() ? 0.0 : m / static_cast<double>(v.size()));
        }
        std::printf("\n");
    }

    if (rows.count("Drums") && rows.count("Tonal")) {
        std::printf("\n\nSEPARABILITY — AUC, Drums vs Tonal\n\n");
        std::printf("%-10s%14s%14s\n", "feature", "drums>tonal", "tonal>drums");
        for (int f = 0; f < kNumFeatures; ++f) {
            std::vector<double> d, t;
            for (const auto& s : rows["Drums"]) d.push_back(s.mean[static_cast<std::size_t>(f)]);
            for (const auto& s : rows["Tonal"]) t.push_back(s.mean[static_cast<std::size_t>(f)]);
            std::printf("%-10s%14.3f%14.3f\n", kNames[f], auc(d, t), auc(t, d));
        }
    }
    return 0;
}
