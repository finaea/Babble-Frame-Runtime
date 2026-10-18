#include <doctest/doctest.h>

#include "model/postprocess/gaze_filter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

using namespace babble;

namespace {

// frameeyeosc 0.7.10's OneEuro, Deadzone and the hold of Smoother::filter, in float like the Rust.
struct RefEuro {
    float min_cutoff = 0.3f, beta = 1.5f, d_cutoff = 0.5f;
    std::optional<float> value;
    float velocity = 0;
    static float alpha(float cutoff, float dt) {
        const float tau = 1.0f / (2.0f * 3.14159265358979f * cutoff);
        return 1.0f / (1.0f + tau / dt);
    }
    float filter(float x, float dt) {
        if (!value) {
            value = x;
            return x;
        }
        const float prev = *value;
        velocity += alpha(d_cutoff, dt) * ((x - prev) / dt - velocity);
        const float cutoff = min_cutoff + beta * std::fabs(velocity);
        const float y = prev + alpha(cutoff, dt) * (x - prev);
        value = y;
        return y;
    }
};

struct RefFilter {
    std::array<RefEuro, 4> euro;
    std::array<std::optional<float>, 4> dz;
    std::optional<std::array<float, 4>> last;
    std::array<float, 4> apply(const std::array<float, 4> &v, std::optional<std::array<float, 2>> lids, float dt) {
        if (lids && std::min((*lids)[0], (*lids)[1]) < 0.5f && last)
            return *last;
        std::array<float, 4> out;
        for (int i = 0; i < 4; i++) {
            const float x = euro[i].filter(v[i], dt);
            const float y = dz[i] ? std::clamp(*dz[i], x - 0.005f, x + 0.005f) : x;
            dz[i] = y;
            out[i] = y;
        }
        last = out;
        return out;
    }
};

constexpr double kDt = 1.0 / 90;

struct Rig {
    gaze::Filter f{gaze::FilterParams{{0.3, 1.5, 0.5}, 0.005, 0.5}};
    RefFilter ref;
    std::array<double, 4> last_out{};
    // Feeds one sample to both and checks they agree; returns the real output.
    std::array<double, 4> step(double x, std::optional<std::array<double, 2>> lids = std::nullopt) {
        const std::array<double, 4> v{x, x * 0.5, -x, x + 0.1};
        std::optional<std::array<float, 2>> rl;
        if (lids)
            rl = std::array<float, 2>{float((*lids)[0]), float((*lids)[1])};
        const auto want = ref.apply({float(v[0]), float(v[1]), float(v[2]), float(v[3])}, rl, float(kDt));
        const auto got = f.apply(v, lids, kDt);
        for (int i = 0; i < 4; i++)
            CHECK(got[i] == doctest::Approx(want[i]).epsilon(1e-4).scale(1.0));
        return last_out = got;
    }
};

}

TEST_CASE("gaze filter matches frameeyeosc: step") {
    Rig r;
    for (int i = 0; i < 30; i++) r.step(0.0);
    for (int i = 0; i < 90; i++) r.step(0.4);
    CHECK(r.last_out[0] > 0.3);   // It has moved toward the target.
}

TEST_CASE("gaze filter matches frameeyeosc: jitter is pinned by the deadzone") {
    Rig r;
    for (int i = 0; i < 20; i++) r.step(0.2);
    const double settled = r.last_out[0];
    for (int i = 0; i < 90; i++) {
        r.step(0.2 + ((i & 1) ? 0.003 : -0.003));
        CHECK(std::fabs(r.last_out[0] - settled) < 0.0035);
    }
}

TEST_CASE("gaze filter matches frameeyeosc: closed lids keep the last output") {
    Rig r;
    for (int i = 0; i < 20; i++) r.step(0.0);
    std::array<double, 4> last_open{};
    for (int i = 0; i < 10; i++) last_open = r.step(0.05 * i);
    for (int i = 0; i < 10; i++) {
        const auto out = r.step(0.5 + 0.05 * i, std::array<double, 2>{0.2, 0.9});
        for (int k = 0; k < 4; k++) CHECK(out[k] == last_open[k]);
    }
    for (int i = 0; i < 20; i++) r.step(0.9, std::array<double, 2>{0.9, 0.9});   // It reopens and follows again.
}

TEST_CASE("gaze filter: unknown lids never hold") {
    gaze::Filter f;
    f.apply({0, 0, 0, 0}, std::nullopt, kDt);
    for (int i = 0; i < 90; i++) f.apply({0.4, 0.4, 0.4, 0.4}, std::nullopt, kDt);
    CHECK(f.apply({0.4, 0.4, 0.4, 0.4}, std::nullopt, kDt)[0] > 0.1);
}
