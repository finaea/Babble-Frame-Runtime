// These tests use a real Logitech C615 frame, which has no Huffman tables (DHT) in its MJPEG data.
#include <doctest/doctest.h>

#include "camera/usb/jpeg.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// This file has its own static copy of the stb_image JPEG decoder, to show what a frame without tables decoded to before.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb_image.h>
#pragma GCC diagnostic pop

using namespace babble;

namespace {

std::vector<uint8_t> read_file(const std::string &name) {
    std::ifstream in(std::string(BABBLE_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Decodes with plain stb_image to grayscale, the way the decoder worked before it inserted tables.
std::vector<uint8_t> raw_stb_gray(const std::vector<uint8_t> &jpg, int *w, int *h) {
    int comp = 0;
    uint8_t *p = stbi_load_from_memory(jpg.data(), int(jpg.size()), w, h, &comp, 1);
    if (!p) return {};
    std::vector<uint8_t> out(p, p + size_t(*w) * size_t(*h));
    stbi_image_free(p);
    return out;
}

// Mean absolute difference between the pixels of a row and the row above.
double mean_row_step(const std::vector<uint8_t> &px, int w, int h) {
    double sum = 0;
    for (int y = 1; y < h; y++)
        for (int x = 0; x < w; x++) sum += std::abs(int(px[size_t(y) * w + x]) - int(px[size_t(y - 1) * w + x]));
    return sum / (double(w) * double(h - 1));
}

bool has_marker(const std::vector<uint8_t> &d, uint8_t m) {
    for (size_t i = 0; i + 1 < d.size(); i++)
        if (d[i] == 0xff && d[i + 1] == m) return true;
    return false;
}

}

TEST_CASE("a C615 frame without Huffman tables decodes to a picture, not stripes") {
    const auto jpg = read_file("c615_no_dht.jpg");
    REQUIRE(jpg.size() > 1000);
    REQUIRE_FALSE(has_marker(jpg, 0xc4));

    int rw = 0, rh = 0;
    const auto raw = raw_stb_gray(jpg, &rw, &rh);
    REQUIRE(!raw.empty());

    std::vector<uint8_t> px(640 * 480);
    int w = 0, h = 0;
    REQUIRE(jpeg::decode_gray(jpg.data(), jpg.size(), px.data(), px.size(), &w, &h));
    CHECK(w == 640);
    CHECK(h == 480);

    const double before = mean_row_step(raw, rw, rh);
    const double after = mean_row_step(px, w, h);
    MESSAGE("mean row step: raw stb " << before << ", decode_gray " << after);
    // The raw stb result is not stable between runs because it reads empty tables, so only the fixed result is checked.
    // A real picture has some change between rows (1.25 measured), and the striped result has almost none or pure noise.
    CHECK(after > 0.6);
    CHECK(after < 5.0);
}

TEST_CASE("a frame that already has tables decodes exactly as plain stb_image does") {
    const auto jpg = read_file("babble_tracker_frame.jpg");
    REQUIRE(has_marker(jpg, 0xc4));

    int rw = 0, rh = 0;
    const auto raw = raw_stb_gray(jpg, &rw, &rh);
    REQUIRE(!raw.empty());

    std::vector<uint8_t> px(size_t(rw) * size_t(rh));
    int w = 0, h = 0;
    REQUIRE(jpeg::decode_gray(jpg.data(), jpg.size(), px.data(), px.size(), &w, &h));
    CHECK(w == rw);
    CHECK(h == rh);
    CHECK(px == raw);
}

TEST_CASE("a frame that ends inside a run of 0xff fill bytes is refused without reading past the end") {
    const std::vector<std::vector<uint8_t>> frames = {{0xff, 0xd8, 0xff, 0xff}, {0xff, 0xd8, 0xff, 0xff, 0xff}};
    for (const auto &f : frames) {
        std::vector<uint8_t> px(16);
        int w = 0, h = 0;
        CHECK_FALSE(jpeg::decode_gray(f.data(), f.size(), px.data(), px.size(), &w, &h));
    }
}
