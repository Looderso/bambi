// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/reverb/images.hpp"

#include <algorithm>
#include <cmath>

namespace bambi {

int imageIndices(int order, std::array<ImageIndex, kMaxImages>& out) {
    int count = 0;
    for (int nx = -order; nx <= order; ++nx)
        for (int ny = -order; ny <= order; ++ny)
            for (int nz = -order; nz <= order; ++nz) {
                const int o = std::abs(nx) + std::abs(ny) + std::abs(nz);
                if (o > 0 && o <= order && count < kMaxImages) out[static_cast<std::size_t>(count++)] = {nx, ny, nz, o};
            }
    return count;
}

double sourceRadius(const Room& room, Vec3 d, double distance) {
    const double half[3] = {room.x * 0.5, room.y * 0.5, room.z * 0.5}, dir[3] = {d.x, d.y, d.z};
    double r = distance;
    for (int k = 0; k < 3; ++k)
        if (std::abs(dir[k]) > 1e-6) r = std::min(r, 0.9 * half[k] / std::abs(dir[k]));
    return r;
}

int reflectionsOf(const Room& room, Vec3 direction, double distance, std::array<Reflection, kMaxImages>& out) {
    const Vec3 d = unit(direction);
    const double r = sourceRadius(room, d, distance);
    const double half[3] = {room.x * 0.5, room.y * 0.5, room.z * 0.5}, source[3] = {d.x * r, d.y * r, d.z * r};
    //  a wall keeps sqrt(1 - absorption) of the pressure at a bounce; distance costs 1/r as usual
    const double beta = std::sqrt(1.0 - room.absorbMid), mixing = room.mixingTime;

    std::array<ImageIndex, kMaxImages> index{};
    const int count = imageIndices(kReflectionOrder, index);
    for (int i = 0; i < count; ++i) {
        const ImageIndex& n = index[static_cast<std::size_t>(i)];
        const int mirrors[3] = {n.nx, n.ny, n.nz};
        double image[3];
        for (int k = 0; k < 3; ++k) {
            const double L = 2.0 * half[k], inRoom = source[k] + half[k];  // the room as [0, L] along this axis
            const double mirrored = mirrors[k] % 2 == 0 ? mirrors[k] * L + inRoom : (mirrors[k] + 1) * L - inRoom;
            image[k] = mirrored - half[k];
        }
        Reflection& t = out[static_cast<std::size_t>(i)];
        t.order = n.order;
        t.distance = std::sqrt(image[0] * image[0] + image[1] * image[1] + image[2] * image[2]);
        t.direction = {image[0] / t.distance, image[1] / t.distance, image[2] / t.distance};
        t.delay = std::max(0.0, (t.distance - r) / kSpeedOfSound);
        t.gain = (r / t.distance) * std::pow(beta, n.order);

        //  still a mirror image: what has not scattered, faded out across the mixing time
        const double fade = t.delay < 0.7 * mixing ? 1.0
                            : t.delay > 1.3 * mixing
                                ? 0.0
                                : 0.5 + 0.5 * std::cos(kPi * (t.delay - 0.7 * mixing) / (0.6 * mixing));
        const double keep = std::pow(1.0 - room.scatter, n.order) * fade * fade;
        t.mirror = t.gain * std::sqrt(keep);
        t.scattered = t.gain * std::sqrt(1.0 - keep);

        //  every bounce dulls it by what the highs lose beyond the mids; and the air along the path --
        //  about 3 dB at 16 kHz after 7.5 m, at 8 kHz after 30 m
        const double wall = 20000.0 * std::pow((1.0 - room.absorbHigh) / (1.0 - room.absorbMid), 2.0 * n.order);
        const double air = 16000.0 * std::sqrt(7.5 / std::max(t.distance, 7.5));
        t.cutoffHz = std::clamp(std::min(wall, air), 800.0, 20000.0);
    }
    return count;
}

std::array<Vec3, kVirtualSources> virtualSourceDirections() {
    std::array<Vec3, kVirtualSources> out{};
    const double golden = kPi * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < kVirtualSources; ++i) {
        const double z = 1.0 - 2.0 * (i + 0.5) / kVirtualSources, r = std::sqrt(1.0 - z * z), a = i * golden;
        out[static_cast<std::size_t>(i)] = {r * std::cos(a), r * std::sin(a), z};
    }
    return out;
}

double maxReBeam(int order, double cosine) {
    //  Legendre by recurrence at both arguments: the max-rE weights are P_n at the cosine of 137.9 / (N + 1.51) degrees
    const double at = std::cos(137.9 * kDeg2Rad / (order + 1.51));
    double w0 = 1.0, w1 = at, p0 = 1.0, p1 = cosine, sum = 1.0, norm = 1.0;
    for (int n = 1; n <= order; ++n) {
        sum += (2 * n + 1) * w1 * p1;
        norm += (2 * n + 1) * w1;
        const double w2 = ((2 * n + 1) * at * w1 - n * w0) / (n + 1),
                     p2 = ((2 * n + 1) * cosine * p1 - n * p0) / (n + 1);
        w0 = w1;
        w1 = w2;
        p0 = p1;
        p1 = p2;
    }
    return sum / norm;
}

void busReflections(const Room& room, double distance, BusReflections& out) {
    out.direction = virtualSourceDirections();
    for (int v = 0; v < kVirtualSources; ++v)
        out.count = reflectionsOf(room, out.direction[static_cast<std::size_t>(v)], distance,
                                  out.taps[static_cast<std::size_t>(v)]);

    //  How far a copy may sit from the average of its image: all the way if they are close already,
    //  otherwise scaled until the farthest any could be -- 2 x distance / c -- is the spread.
    const double f = std::min(1.0, kSpreadSeconds / std::max(2.0 * distance / kSpeedOfSound, 1e-9));
    for (int i = 0; i < out.count; ++i) {
        double mean = 0.0;
        for (int v = 0; v < kVirtualSources; ++v)
            mean += out.taps[static_cast<std::size_t>(v)][static_cast<std::size_t>(i)].delay;
        mean /= kVirtualSources;
        for (int v = 0; v < kVirtualSources; ++v) {
            double& delay = out.taps[static_cast<std::size_t>(v)][static_cast<std::size_t>(i)].delay;
            delay = mean + f * (delay - mean);
        }
    }
}

}  // namespace bambi
