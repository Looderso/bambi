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
}  // namespace

/*  The equirect texture: one RGBA pixel a texel, orange where what arrived shows and blue where only what
    this plugin added does -- never mixed, the input winning. Drawn scaled and smoothed, so the picture is
    continuous rather than showing the grid it was computed on. */
void EnergyLayer::buildEquirect(const EnergyField& field) {
    if (!equirect_.isValid()) equirect_ = juce::Image(juce::Image::ARGB, kEnergyWidth, kEnergyHeight, true);

    const auto alpha = field.alpha();
    const auto share = field.addedOnly();
    if (alpha.size() < static_cast<std::size_t>(kEnergyTexels)) return;

    juce::Image::BitmapData pixels(equirect_, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < kEnergyHeight; ++y)
        for (int x = 0; x < kEnergyWidth; ++x) {
            const auto t = static_cast<std::size_t>(y * kEnergyWidth + x);
            const auto ink = share[t] < 0.5f ? colour::sceneIn : colour::added;
            pixels.setPixelColour(x, y, ink.withAlpha(alpha[t] * kEnergyTopAlpha));
        }
}

/*  The globe resamples the finished alpha and colour -- not the energy -- through a table rebuilt
    only when the camera turns, so the ballistics are computed once. */
void EnergyLayer::buildGlobe(const EnergyField& field, const Camera& camera) {
    if (!globe_.isValid()) globe_ = juce::Image(juce::Image::ARGB, kGlobePixels, kGlobePixels, true);

    const bool moved = std::abs(tableFor_.yaw - camera.yaw) > 1e-12 || std::abs(tableFor_.pitch - camera.pitch) > 1e-12;
    if (moved || globeU_.empty()) {
        tableFor_ = camera;
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
                const double az = azimuth(*d), el = elevation(*d);
                globeU_[p] = static_cast<float>(EnergyField::columnOf(az));
                globeV_[p] = static_cast<float>(
                    std::clamp((0.5 - el / kPi) * kEnergyHeight - 0.5, 0.0, static_cast<double>(kEnergyHeight - 1)));
            }
    }

    const auto alpha = field.alpha();
    const auto share = field.addedOnly();
    if (alpha.size() < static_cast<std::size_t>(kEnergyTexels)) return;

    const auto bilinear = [](std::span<const float> from, int x0, int x1, int y0, int y1, float tx, float ty) {
        const auto at = [&](int x, int y) { return from[static_cast<std::size_t>(y * kEnergyWidth + x)]; };
        return (at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx) * (1.0f - ty) +
               (at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx) * ty;
    };

    juce::Image::BitmapData pixels(globe_, juce::Image::BitmapData::writeOnly);
    for (int py = 0; py < kGlobePixels; ++py)
        for (int px = 0; px < kGlobePixels; ++px) {
            const auto p = static_cast<std::size_t>(py * kGlobePixels + px);
            if (globeInside_[p] == 0) {
                pixels.setPixelColour(px, py, colour::added.withAlpha(0.0f));  // outside the globe
                continue;
            }
            const float fx = globeU_[p], fy = globeV_[p];
            int x0 = static_cast<int>(std::floor(fx));
            const float tx = fx - static_cast<float>(x0);
            x0 = ((x0 % kEnergyWidth) + kEnergyWidth) % kEnergyWidth;
            const int x1 = (x0 + 1) % kEnergyWidth;
            const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, kEnergyHeight - 1);
            const float ty = fy - static_cast<float>(y0);
            const int y1 = std::min(y0 + 1, kEnergyHeight - 1);

            const float a = bilinear(alpha, x0, x1, y0, y1, tx, ty);
            const float f = bilinear(share, x0, x1, y0, y1, tx, ty);
            //  the edge between the two is where the smoothed map crosses a half: a line, not a brown band
            pixels.setPixelColour(px, py, (f < 0.5f ? colour::sceneIn : colour::added).withAlpha(a * kEnergyTopAlpha));
        }
}

bool EnergyLayer::paint(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
                        const EnergyField& field, float opacity) {
    if (field.order() < 0) return false;
    buildEquirect(field);

    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    const juce::Rectangle<float> into{static_cast<float>(vp.cx - vp.rx), static_cast<float>(vp.cy - vp.ry),
                                      static_cast<float>(2.0 * vp.rx), static_cast<float>(2.0 * vp.ry)};
    juce::Graphics::ScopedSaveState keep(g);
    g.setOpacity(opacity);
    if (projection == Projection::Equirect) {
        g.drawImage(equirect_, into, juce::RectanglePlacement::stretchToFit);
        return true;
    }
    buildGlobe(field, camera);
    g.drawImage(globe_, into, juce::RectanglePlacement::stretchToFit);
    return true;
}

}  // namespace bambi::ui
