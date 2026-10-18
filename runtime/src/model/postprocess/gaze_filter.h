#pragma once

// The gaze smoothing of frameeyeosc: a One Euro filter, then a backlash deadzone, and the last gaze is kept while an
// eyelid is shut.

#include "model/postprocess/one_euro.h"

#include <array>
#include <optional>

namespace babble {

namespace gaze {

// The defaults are frameeyeosc's.
struct FilterParams {
    OneEuroParams euro{0.3, 1.5, 0.5};
    double deadzone = 0.005;            // This is in the units of the filtered values.
    double hold_closed_below = 0.5;     // The gaze is kept while an eyelid is below this, and 0 turns the hold off.
};

// The output only moves when the input goes more than `width` away from it, as frameeyeosc's Deadzone does.
class Deadzone {
public:
    double apply(double x, double width);
    void reset() { value_.reset(); }

private:
    std::optional<double> value_;
};

// This filter smooths the four gaze values in the order LeftEyeX, LeftEyeY, RightEyeX, RightEyeY.
class Filter {
public:
    explicit Filter(const FilterParams &p = {});
    // A change of the One Euro parameters starts the filters again; the other parameters keep the state.
    void set_params(const FilterParams &p);
    const FilterParams &params() const { return p_; }
    // `lids` are the left and right openness, or nullopt when they are not known. A dt_s of 0 or more than 0.25 s
    // starts the filters again.
    std::array<double, 4> apply(const std::array<double, 4> &v, std::optional<std::array<double, 2>> lids, double dt_s);
    void reset();

private:
    FilterParams p_;
    std::array<OneEuro, 4> euro_;
    std::array<Deadzone, 4> deadzone_;
    std::optional<std::array<double, 4>> last_;
};

}

}
