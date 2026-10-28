#include "model/tilt_warp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace babble::img {

void tilt_resize(View src, Mut dst, float deg) {
    const double a = double(deg) * std::numbers::pi / 180.0, c = std::cos(a), s = std::sin(a);
    const double k = 1.0 / (std::fabs(c) + std::fabs(s));
    const int sw = int(src.width), sh = int(src.height);
    auto at = [&](int x, int y) -> double {
        return x >= 0 && y >= 0 && x < sw && y < sh ? src.data[size_t(y) * src.stride + x] : 0.0;
    };
    for (uint32_t v = 0; v < dst.height; v++) {
        // The positions are in units of the picture size, with 0 at the centre, so the stretch and the turn are one map.
        const double py = (v + 0.5) / dst.height - 0.5;
        for (uint32_t u = 0; u < dst.width; u++) {
            const double px = (u + 0.5) / dst.width - 0.5;
            const double fx = ((px * c - py * s) / k + 0.5) * sw - 0.5;
            const double fy = ((px * s + py * c) / k + 0.5) * sh - 0.5;
            const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            const double ax = fx - x0, ay = fy - y0;
            const double top = at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax;
            const double bottom = at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax;
            dst.data[size_t(v) * dst.stride + u] = uint8_t(std::clamp(std::lround(top * (1 - ay) + bottom * ay), 0L, 255L));
        }
    }
}

}
