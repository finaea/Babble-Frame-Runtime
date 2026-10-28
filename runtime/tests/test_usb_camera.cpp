// These tests cover the fork's USB camera additions on top of upstream's USB stack, without a device: a hand-built
// GC0308-like descriptor (0c45:6366) and the JPEG check that allows the GC0308's padding after EOI.
#include <doctest/doctest.h>

#include "camera/usb/uvc.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace babble;

namespace {

using Bytes = std::vector<uint8_t>;

void append(Bytes &out, const Bytes &d) { out.insert(out.end(), d.begin(), d.end()); }

Bytes interface_desc(uint8_t iface, uint8_t alt, uint8_t endpoints, uint8_t cls, uint8_t sub) {
    return {9, 4, iface, alt, endpoints, cls, sub, 0, 0};
}

Bytes endpoint_desc(uint8_t addr, uint8_t attr, uint16_t mps) {
    return {7, 5, addr, attr, uint8_t(mps & 0xff), uint8_t(mps >> 8), 1};
}

void put32(Bytes &b, uint32_t v) {
    for (int i = 0; i < 4; i++)
        b.push_back(uint8_t(v >> (8 * i)));
}

// A VS_FRAME_MJPEG (sub 7) or VS_FRAME_UNCOMPRESSED (sub 5) descriptor with discrete intervals.
Bytes frame_desc(uint8_t sub, uint8_t idx, uint16_t w, uint16_t h, const std::vector<uint32_t> &intervals) {
    Bytes b{0, 0x24, sub, idx, 0, uint8_t(w & 0xff), uint8_t(w >> 8), uint8_t(h & 0xff), uint8_t(h >> 8)};
    put32(b, 0);
    put32(b, 0);
    put32(b, uint32_t(w) * h * 2);
    put32(b, intervals.at(0));
    b.push_back(uint8_t(intervals.size()));
    for (uint32_t i : intervals)
        put32(b, i);
    b[0] = uint8_t(b.size());
    return b;
}

constexpr uint8_t kMjpegFrame = 7, kYuyvFrame = 5;

Bytes mjpeg_format(uint8_t idx, uint8_t frames) { return {11, 0x24, 6, idx, frames, 0, 1, 0, 0, 0, 0}; }

Bytes yuy2_format(uint8_t idx, uint8_t frames) {
    Bytes b{27, 0x24, 4, idx, frames, 'Y', 'U', 'Y', '2', 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    b.insert(b.end(), {16, 1, 0, 0, 0, 0});
    return b;
}

// The configuration descriptor of the GC0308 camera: MJPEG and YUY2 formats and isochronous alternate settings.
Bytes gc0308() {
    Bytes c{9, 2, 0, 0, 2, 1, 0, 0x80, 50};
    append(c, interface_desc(0, 0, 0, 0x0e, 1));
    append(c, {13, 0x24, 1, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 1, 1});   // VC header, bcdUVC 0x0100
    append(c, interface_desc(1, 0, 0, 0x0e, 2));
    append(c, mjpeg_format(1, 3));
    append(c, frame_desc(kMjpegFrame, 1, 640, 480, {333333}));
    append(c, frame_desc(kMjpegFrame, 2, 320, 240, {333333, 83333}));
    append(c, frame_desc(kMjpegFrame, 3, 160, 120, {333333, 666666, 1000000, 2000000}));
    append(c, yuy2_format(2, 3));
    append(c, frame_desc(kYuyvFrame, 1, 640, 480, {333333}));
    append(c, frame_desc(kYuyvFrame, 2, 320, 240, {333333}));
    append(c, frame_desc(kYuyvFrame, 3, 160, 120, {333333}));
    // Alt 0 has no endpoint, and the other alts are isochronous IN endpoints.
    const std::vector<std::pair<uint8_t, uint16_t>> alts = {{1, 128}, {2, 512}, {3, 800}, {4, 1024}, {5, 0x1400}};
    for (const auto &[alt, mps] : alts) {
        append(c, interface_desc(1, alt, 1, 0x0e, 2));
        append(c, endpoint_desc(0x81, 0x05, mps));
    }
    c[2] = uint8_t(c.size() & 0xff);
    c[3] = uint8_t(c.size() >> 8);
    return c;
}

}

TEST_CASE("USB camera: GC0308 descriptor parses with MJPEG, YUY2 and every iso alt") {
    const Bytes c = gc0308();
    std::string err;
    const auto info = uvc::parse_config(c.data(), c.size(), &err);
    REQUIRE_MESSAGE(info, err);
    CHECK(info->bcd_uvc == 0x0100);
    CHECK(info->control_iface == 0);
    CHECK(info->stream_iface == 1);
    CHECK_FALSE(info->bulk);
    CHECK(info->endpoint == 0x81);
    REQUIRE(info->formats.size() == 2);
    CHECK(info->formats[0].kind == uvc::FormatKind::Mjpeg);
    CHECK(info->formats[0].frames.size() == 3);
    CHECK(info->formats[1].kind == uvc::FormatKind::Yuy2);
    CHECK(info->formats[1].frames.size() == 3);

    REQUIRE(info->alts.size() == 5);
    const uint32_t want[] = {128, 512, 800, 1024, 3072};   // 0x1400 is 1024 x 3.
    for (size_t i = 0; i < 5; i++) {
        CHECK(info->alts[i].alt == i + 1);
        CHECK(info->alts[i].endpoint == 0x81);
        CHECK(info->alts[i].bytes == want[i]);
    }

    // The YUY2 format and size that the user picks is the one that streams.
    const auto p = uvc::pick_mode(*info, {uvc::FormatKind::Yuy2, 320, 240, 30});
    REQUIRE(p);
    CHECK(p->format->kind == uvc::FormatKind::Yuy2);
    CHECK(p->frame->width == 320);
    CHECK(p->frame->height == 240);
}

TEST_CASE("USB camera: looks_like_jpeg allows padding after EOI") {
    auto check = [](const Bytes &b) { return uvc::looks_like_jpeg(b.data(), b.size()); };
    CHECK(check({0xff, 0xd8, 1, 2, 3, 0xff, 0xd9}));
    CHECK(check({0xff, 0xd8, 1, 2, 3, 0xff, 0xd9, 0, 0, 0, 0, 0, 0}));   // The GC0308 pads with zero bytes.
    CHECK_FALSE(check({0xff, 0xd8, 1, 2, 3, 4, 5, 6}));                  // No EOI.
    CHECK_FALSE(check({0x00, 0x00, 1, 2, 3, 0xff, 0xd9}));               // No SOI.
    CHECK_FALSE(check({0xff, 0xd8, 0xff}));                              // Too short.

    Bytes far(10 * 1024, 0);
    far[0] = 0xff;
    far[1] = 0xd8;
    far[100] = 0xff;
    far[101] = 0xd9;
    CHECK_FALSE(check(far));   // EOI is more than 4096 bytes from the end.

    // A real frame with padding after EOI goes through to_gray, the path that the USB source uses.
    std::ifstream in(std::string(BABBLE_TEST_DATA_DIR) + "/babble_tracker_frame.jpg", std::ios::binary);
    Bytes jpg((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(jpg.size() > 4);
    jpg.insert(jpg.end(), 8, 0);
    CHECK(check(jpg));
    Bytes px(240 * 240);
    int w = 0, h = 0;
    REQUIRE(uvc::to_gray(uvc::FormatKind::Mjpeg, jpg.data(), jpg.size(), 0, 0, px.data(), px.size(), &w, &h));
    CHECK(w == 240);
    CHECK(h == 240);
}
