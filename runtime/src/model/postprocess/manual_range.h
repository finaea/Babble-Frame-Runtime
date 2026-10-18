#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>

namespace babble {

// A manual calibration range in the Baballonia style. Raising min means that a bigger movement is needed before the
// value starts, and lowering max means that a smaller movement reaches 100%. The defaults change nothing.
struct ManualRange {
    float min = 0.0f, max = 1.0f;

    // Maps v from [min, max] to [0, 1] and clamps it.
    float apply(float v) const {
        const float t = max - min > 1e-6f ? (v - min) / (max - min) : v;
        return std::clamp(t, 0.0f, 1.0f);
    }
    // Maps v from [min, max] to [-1, 1] and clamps it, for gaze axes whose range defaults to [-1, 1].
    float apply_signed(float v) const {
        const float t = max - min > 1e-6f ? (v - min) / (max - min) * 2.0f - 1.0f : v;
        return std::clamp(t, -1.0f, 1.0f);
    }
    // Reads [min, max] with min < max, and returns false for anything else.
    static bool parse(const nlohmann::json &j, ManualRange *out) {
        if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number())
            return false;
        const float lo = j[0].get<float>(), hi = j[1].get<float>();
        if (!(hi > lo))
            return false;
        *out = {lo, hi};
        return true;
    }
};

}
