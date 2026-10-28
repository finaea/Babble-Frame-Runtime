#include <doctest/doctest.h>

#include "model/image_ops.h"
#include "model/image_tap.h"
#include "model/tilt_warp.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace babble;

namespace {

// A textured picture with no symmetry, so a wrong turn or a wrong mirror shows.
std::vector<uint8_t> pattern(uint32_t w, uint32_t h) {
    std::vector<uint8_t> px(size_t(w) * h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            px[size_t(y) * w + x] = uint8_t((x * 7 + y * 3 + (x / 16) * 40 + (y / 24) * 90) % 256);
    return px;
}

int max_diff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    REQUIRE(a.size() == b.size());
    int worst = 0;
    for (size_t i = 0; i < a.size(); i++)
        worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
    return worst;
}

}

TEST_CASE("a 90 degree tilt equals a quarter turn within rounding") {
    // The source is bigger than 224 so no sample falls off the edge; there tilt_resize reads black and resize_linear
    // repeats the edge, which is a different rule and not what this checks.
    const uint32_t w = 300, h = 260;
    const auto src = pattern(w, h);
    for (const int turns : {1, 3}) {
        std::vector<uint8_t> turned(src.size()), want(224 * 224), got(224 * 224);
        img::rotate({src.data(), w, h, w}, {turned.data(), h, w, h}, turns);
        img::resize_linear({turned.data(), h, w, h}, {want.data(), 224, 224, 224});
        img::tilt_resize({src.data(), w, h, w}, {got.data(), 224, 224, 224}, turns == 1 ? 90.0f : -90.0f);
        const int d = max_diff(got, want);
        MESSAGE("max diff for " << (turns == 1 ? 90 : -90) << " degrees: " << d);
        CHECK(d <= 1);
    }
}

TEST_CASE("a 45 degree tilt keeps every corner of the picture") {
    const uint32_t w = 300, h = 300;
    std::vector<uint8_t> dst(224 * 224);
    SUBCASE("a white picture fills the turned square up to the edge midpoints") {
        std::vector<uint8_t> src(size_t(w) * h, 255);
        img::tilt_resize({src.data(), w, h, w}, {dst.data(), 224, 224, 224}, 45);
        // The corners of the turned square touch the middle of each dst edge, so the centre row and column are
        // bright to within a few pixels of the edge.
        for (uint32_t i = 6; i < 218; i++) {
            CHECK(dst[112 * 224 + i] > 200);
            CHECK(dst[i * 224 + 112] > 200);
        }
        // The dst corners are outside the turned square.
        for (const size_t at : {size_t(0), size_t(223), size_t(223 * 224), size_t(223 * 224 + 223)})
            CHECK(dst[at] == 0);
    }
    SUBCASE("a one pixel frame on the border still shows near each edge midpoint") {
        std::vector<uint8_t> src(size_t(w) * h, 0);
        for (uint32_t i = 0; i < w; i++)
            src[i] = src[size_t(h - 1) * w + i] = src[size_t(i) * w] = src[size_t(i) * w + w - 1] = 255;
        img::tilt_resize({src.data(), w, h, w}, {dst.data(), 224, 224, 224}, 45);
        auto lit = [&](uint32_t x0, uint32_t y0) {
            for (uint32_t y = y0; y < y0 + 12; y++)
                for (uint32_t x = x0; x < x0 + 12; x++)
                    if (dst[size_t(y) * 224 + x])
                        return true;
            return false;
        };
        CHECK(lit(106, 0));     // Top.
        CHECK(lit(106, 212));   // Bottom.
        CHECK(lit(0, 106));     // Left.
        CHECK(lit(212, 106));   // Right.
    }
}

TEST_CASE("the image tap copies only while open, at most max_hz times a second") {
    // A 4x2 image inside a 6 wide buffer, so the copy has to follow the stride.
    const std::vector<uint8_t> px = {1, 2, 3, 4, 99, 99, 5, 6, 7, 8, 99, 99};
    const img::View v{px.data(), 4, 2, 6};

    ImageTap tap(20);   // One copy per 50 ms.
    tap.offer(v);
    CHECK(tap.latest() == nullptr);   // Closed.

    tap.set_open(true);
    tap.offer(v);
    auto first = tap.latest();
    REQUIRE(first);
    CHECK(first->width == 4);
    CHECK(first->height == 2);
    CHECK(first->pixels == std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8});

    tap.offer(v);   // Too soon.
    CHECK(tap.latest()->seq == first->seq);

    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    tap.offer(v);
    CHECK(tap.latest()->seq == first->seq + 1);
}
