// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/EnergyLayer.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "bambi/ui/Theme.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;

//  the globe's own texture, in pixels a side, fixed whatever the window's size
constexpr int kGlobePixels = 180;
//  The alpha a fully lit texel is drawn at, from the reference implementation.
constexpr float kEnergyTopAlpha = 225.0f / 255.0f;

/// The fractional row an elevation falls in, clamped to the texture.
float rowOf(double elevationRad) {
    return static_cast<float>(
        std::clamp((0.5 - elevationRad / kPi) * kEnergyHeight - 0.5, 0.0, static_cast<double>(kEnergyHeight - 1)));
}

/// `from` at a fractional column and row: bilinear, the columns wrapping round the back, the rows clamped.
float sampleAt(std::span<const float> from, float fx, float fy) {
    int x0 = static_cast<int>(std::floor(fx));
    const float tx = fx - static_cast<float>(x0);
    x0 = ((x0 % kEnergyWidth) + kEnergyWidth) % kEnergyWidth;
    const int x1 = (x0 + 1) % kEnergyWidth;
    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, kEnergyHeight - 1);
    const float ty = fy - static_cast<float>(y0);
    const int y1 = std::min(y0 + 1, kEnergyHeight - 1);
    const auto at = [&](int x, int y) { return from[static_cast<std::size_t>(y * kEnergyWidth + x)]; };
    return (at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx) * (1.0f - ty) +
           (at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx) * ty;
}
}  // namespace

/*  The equirect texture: one RGBA pixel a texel, orange where what arrived shows and blue where only what
    this plugin added does -- never mixed, the input winning. Drawn scaled and smoothed, so the picture is
    continuous rather than showing the grid it was computed on. Turned, each texel samples the field where the
    turn carries its direction, through a table rebuilt when the turn changes. */
void EnergyLayer::buildEquirect(const EnergyField& field, const Mat3* turn) {
    if (!equirect_.isValid()) equirect_ = juce::Image(juce::Image::ARGB, kEnergyWidth, kEnergyHeight, true);

    const auto alpha = field.alpha();
    const auto share = field.addedOnly();
    if (alpha.size() < static_cast<std::size_t>(kEnergyTexels)) return;

    if (turn != nullptr && (turnedU_.empty() || *turn != turnedFor_)) {
        turnedFor_ = *turn;
        turnedU_.assign(kEnergyTexels, 0.0f);
        turnedV_.assign(kEnergyTexels, 0.0f);
        for (int t = 0; t < kEnergyTexels; ++t) {
            const Vec3 d = apply(*turn, EnergyField::directionOf(t));
            turnedU_[static_cast<std::size_t>(t)] = static_cast<float>(EnergyField::columnOf(azimuth(d)));
            turnedV_[static_cast<std::size_t>(t)] = rowOf(elevation(d));
        }
    }

    juce::Image::BitmapData pixels(equirect_, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < kEnergyHeight; ++y)
        for (int x = 0; x < kEnergyWidth; ++x) {
            const auto t = static_cast<std::size_t>(y * kEnergyWidth + x);
            float a = alpha[t], f = share[t];
            if (turn != nullptr) {
                a = sampleAt(alpha, turnedU_[t], turnedV_[t]);
                f = sampleAt(share, turnedU_[t], turnedV_[t]);
            }
            const auto ink = f < 0.5f ? colour::in : colour::added;
            pixels.setPixelColour(x, y, ink.withAlpha(a * kEnergyTopAlpha));
        }
}

/*  The globe resamples the finished alpha and colour -- not the energy -- through a table rebuilt
    only when the camera or the turn changes, so the ballistics are computed once. */
void EnergyLayer::buildGlobe(const EnergyField& field, const Camera& camera, const Mat3* turn) {
    if (!globe_.isValid()) globe_ = juce::Image(juce::Image::ARGB, kGlobePixels, kGlobePixels, true);

    const Mat3 turnNow = turn != nullptr ? *turn : kIdentity3;
    const bool moved = std::abs(tableFor_.yaw - camera.yaw) > 1e-12 ||
                       std::abs(tableFor_.pitch - camera.pitch) > 1e-12 || turnNow != tableTurn_;
    if (moved || globeU_.empty()) {
        tableFor_ = camera;
        tableTurn_ = turnNow;
        globeU_.assign(kGlobePixels * kGlobePixels, 0.0f);
        globeV_.assign(kGlobePixels * kGlobePixels, 0.0f);
        globeInside_.assign(kGlobePixels * kGlobePixels, 0);
        for (int py = 0; py < kGlobePixels; ++py)
            for (int px = 0; px < kGlobePixels; ++px) {
                const auto p = static_cast<std::size_t>(py * kGlobePixels + px);
                const double nx = (px + 0.5) / kGlobePixels * 2.0 - 1.0;
                const double ny = 1.0 - (py + 0.5) / kGlobePixels * 2.0;
                const auto d = unproject(Projection::Globe, camera, nx, ny);
                if (!d.has_value()) continue;
                globeInside_[p] = 1;
                const Vec3 room = apply(turnNow, *d);
                globeU_[p] = static_cast<float>(EnergyField::columnOf(azimuth(room)));
                globeV_[p] = rowOf(elevation(room));
            }
    }

    const auto alpha = field.alpha();
    const auto share = field.addedOnly();
    if (alpha.size() < static_cast<std::size_t>(kEnergyTexels)) return;

    juce::Image::BitmapData pixels(globe_, juce::Image::BitmapData::writeOnly);
    for (int py = 0; py < kGlobePixels; ++py)
        for (int px = 0; px < kGlobePixels; ++px) {
            const auto p = static_cast<std::size_t>(py * kGlobePixels + px);
            if (globeInside_[p] == 0) {
                pixels.setPixelColour(px, py, colour::added.withAlpha(0.0f));  // outside the globe
                continue;
            }
            const float a = sampleAt(alpha, globeU_[p], globeV_[p]);
            const float f = sampleAt(share, globeU_[p], globeV_[p]);
            //  the edge between the two is where the smoothed map crosses a half: a line, not a brown band
            pixels.setPixelColour(px, py, (f < 0.5f ? colour::in : colour::added).withAlpha(a * kEnergyTopAlpha));
        }
}

bool EnergyLayer::paint(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
                        const EnergyField& field, float opacity, const Mat3* turn) {
    if (field.order() < 0) return false;
    buildEquirect(field, turn);

    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    const juce::Rectangle<float> into{static_cast<float>(vp.cx - vp.rx), static_cast<float>(vp.cy - vp.ry),
                                      static_cast<float>(2.0 * vp.rx), static_cast<float>(2.0 * vp.ry)};
    juce::Graphics::ScopedSaveState keep(g);
    g.setOpacity(opacity);
    if (projection == Projection::Equirect) {
        g.drawImage(equirect_, into, juce::RectanglePlacement::stretchToFit);
        return true;
    }
    buildGlobe(field, camera, turn);
    g.drawImage(globe_, into, juce::RectanglePlacement::stretchToFit);
    return true;
}

}  // namespace bambi::ui
