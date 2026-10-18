#include <doctest/doctest.h>

#include "model/postprocess/blink_hold.h"

#include <cstdint>
#include <vector>

using namespace babble;

namespace {
constexpr uint64_t kStepNs = 1000000000ull / 90;

// Runs open/closed samples at 90 Hz and returns the outputs; closed samples are 0, open are 1.
std::vector<double> run(const std::vector<int> &closed, double hold_ms) {
    eye::BlinkHold h;
    std::vector<double> out;
    for (size_t i = 0; i < closed.size(); i++)
        out.push_back(h.apply(closed[i] ? 0.0 : 1.0, 1000 + i * kStepNs, hold_ms));
    return out;
}
}

TEST_CASE("blink hold: a short blink is held closed for hold_ms") {
    // Closes at sample 2, 3 samples (~33 ms) shut, then open. Hold to 2+80ms = sample 9.2, so samples 2..9 are 0.
    std::vector<int> closed(20, 0);
    for (int i = 2; i < 5; i++) closed[i] = 1;
    const auto out = run(closed, 80);
    for (int i = 0; i < 20; i++) {
        const bool held = i >= 2 && i <= 9;
        CHECK(out[i] == (held ? 0.0 : 1.0));
    }
}

TEST_CASE("blink hold: a long blink opens as soon as it ends") {
    std::vector<int> closed(30, 0);
    for (int i = 2; i < 17; i++) closed[i] = 1;   // 15 samples, ~167 ms
    const auto out = run(closed, 80);
    CHECK(out[16] == 0.0);
    CHECK(out[17] == 1.0);
}

TEST_CASE("blink hold: hold_ms 0 passes the input through") {
    std::vector<int> closed = {0, 1, 0, 0, 1, 1, 0, 1, 0, 0};
    const auto out = run(closed, 0);
    for (size_t i = 0; i < closed.size(); i++)
        CHECK(out[i] == (closed[i] ? 0.0 : 1.0));
}
