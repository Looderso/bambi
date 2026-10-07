// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <string>
#include <vector>

#include "bambi/math/vec3.hpp"

/*  Nobody's head, and the two sliders that make it the listener's. The model is a set of maps over
 *  the sphere, each a real spherical-harmonic expansion (SN3D, ACN) of a curve over an ERB axis:
 *  the smooth part of the two ears' summed level, the level difference between the ears and how it
 *  changes with the head's size, the time difference between the ears, and a few notches given by
 *  their centre, depth, width and presence. A Head evaluates the maps once for a fixed set of
 *  directions and frequencies, then synthesises both ears for a slider setting from those tables.
 *
 *  Units: dB; the axis in ERB numbers, erb = 21.4 log10(1 + 0.00437 f); microseconds for the time
 *  difference, left arrival minus right, so negative for a source on the left. Directions are unit
 *  vectors, x front, y left, z up.
 */
namespace bambi {

struct HeadModel {
    struct Axis {
        double erb0{0}, step{0};  ///< point k lies at erb0 + k * step
        int points{0};
    };
    /// numChannels(order) rows of axis.points values: row p is SH channel p's curve over the axis.
    struct Table {
        int order{0};
        std::vector<double> coef;
    };
    /// One value per direction: numChannels(order) coefficients.
    struct Map {
        int order{0};
        std::vector<double> coef;
    };
    /// One notch's maps over the sphere, numChannels(Notches::order) coefficients each.
    struct Notch {
        std::vector<double> centre, depth, width, presence;  ///< ERB; dB, at most 0; ERB; 0..1
    };
    struct Notches {
        int order{0};
        double depthFactor{1};            ///< the learned depths are multiplied by this
        double fadeIn{0}, fadeOut{1};     ///< presence below fadeIn: no notch; above fadeOut: the whole depth
        double bandLo{0}, bandHi{0};      ///< a shifted centre stays inside this band, ERB
        double widthMin{0}, widthMax{0};  ///< ERB
        std::vector<Notch> list;
    };

    std::string learnedFrom;  ///< the databases the model was learned from, for an about box
    Axis axis;
    Table smooth;       ///< both ears' summed level, dB, with the reference head's timbre folded in
    Table ild;          ///< left minus right, dB, at head size 1
    Table ildWithSize;  ///< the change of ild per unit of head size; its order is ild's
    Map itd;            ///< microseconds at head size 1
    Notches notches;

    /// Every table holds numChannels(order) * axis.points values, every notch map numChannels(notches.order),
    /// the orders, axis and ranges make sense. `why` names the first thing that does not.
    bool consistent(std::string* why = nullptr) const;
};

/*  The synthesiser. prepare() allocates and evaluates every map at the directions and frequencies
 *  asked for; it runs once, off the audio thread. synthesise() allocates nothing and is meant for a
 *  worker thread that rebuilds the decoder's filters when a slider moves -- never the audio thread,
 *  whose filters are swapped in whole.
 */
class Head {
public:
    /// `erbs` are the axis points the ears are wanted at (ERB numbers); empty means the model's own axis.
    /// Between the model's axis points a table is linear, beyond its ends it holds the end value.
    void prepare(const HeadModel& model, std::span<const Vec3> directions, std::span<const double> erbs = {});

    /// earShiftErb moves every notch by that many ERB (0: as learned). headSize scales the time difference and
    /// morphs the low-frequency level difference with it (1: the population's head).
    void synthesise(double earShiftErb, double headSize);

    int directions() const { return numDirections_; }
    int points() const { return numPoints_; }
    std::span<const double> erbs() const { return erb_; }
    std::span<const double> left(int d) const { return row(left_, d); }  ///< dB at every point
    std::span<const double> right(int d) const { return row(right_, d); }
    double itd(int d) const { return itd_[static_cast<std::size_t>(d)]; }  ///< microseconds

private:
    std::span<const double> row(const std::vector<double>& table, int d) const {
        return {table.data() + static_cast<std::size_t>(d) * static_cast<std::size_t>(numPoints_),
                static_cast<std::size_t>(numPoints_)};
    }

    int numDirections_{0}, numPoints_{0}, numNotches_{0};
    double bandLo_{0}, bandHi_{0};
    std::vector<double> erb_;                            // points
    std::vector<double> smooth_, ildBase_, ildSlope_;    // directions x points
    std::vector<double> itdBase_;                        // directions
    std::vector<double> centre_, depth_, width_;         // directions x notches; depth with factor and fade applied
    std::vector<double> shifted_;                        // notches: the centres of one direction after the shift
    std::vector<double> left_, right_, itd_;             // the result
};

}  // namespace bambi
