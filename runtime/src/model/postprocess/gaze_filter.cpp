#include "model/postprocess/gaze_filter.h"

#include <algorithm>

namespace babble {

namespace gaze {

namespace {
// Gaps longer than this start the filters again, as frameeyeosc's MAX_GAP does.
constexpr double kMaxGap = 0.25;
}

double Deadzone::apply(double x, double width) {
    const double y = value_ ? std::clamp(*value_, x - width, x + width) : x;
    value_ = y;
    return y;
}

Filter::Filter(const FilterParams &p) : p_(p) { reset(); }

void Filter::set_params(const FilterParams &p) {
    const bool euro_changed = !(p.euro == p_.euro);
    p_ = p;
    if (euro_changed)
        for (auto &e : euro_)
            e = OneEuro(1, p_.euro);
}

void Filter::reset() {
    for (auto &e : euro_)
        e = OneEuro(1, p_.euro);
    for (auto &d : deadzone_)
        d.reset();
    last_.reset();
}

std::array<double, 4> Filter::apply(const std::array<double, 4> &v, std::optional<std::array<double, 2>> lids,
                                    double dt_s) {
    if (dt_s <= 0 || dt_s > kMaxGap)
        reset();
    // The gaze jumps around while the eyes are mostly shut, so the last gaze is kept.
    const bool shut = p_.hold_closed_below > 0 && lids &&
                      std::min((*lids)[0], (*lids)[1]) < p_.hold_closed_below;
    if (shut && last_)
        return *last_;
    std::array<double, 4> out;
    for (int i = 0; i < 4; i++)
        out[i] = deadzone_[i].apply(euro_[i].apply(v[i], dt_s), p_.deadzone);
    last_ = out;
    return out;
}

}

}
