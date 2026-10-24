// These tests cover the USB camera runtime without a device: a hand-built GC0308-like descriptor (0c45:6366)
// and the descriptor of the real Babble tracker.
#include <doctest/doctest.h>

#include "camera/usb/babble_usb_source.h"
#include "camera/usb/jpeg.h"
#include "camera/usb/uvc.h"

#include <nlohmann/json.hpp>

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

// The configuration descriptor of the GC0308 camera, with mjpeg_extra added to the MJPEG frames.
Bytes gc0308(const std::vector<Bytes> &mjpeg_extra = {}, bool with_yuyv = true, bool with_default_mjpeg = true,
             bool ss_companion = false) {
    Bytes c{9, 2, 0, 0, 2, 1, 0, 0x80, 50};
    append(c, interface_desc(0, 0, 0, 0x0e, 1));
    append(c, {13, 0x24, 1, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 1, 1});   // VC header, bcdUVC 0x0100
    append(c, interface_desc(1, 0, 0, 0x0e, 2));
    size_t mjpeg_frames = (with_default_mjpeg ? 6 : 0) + mjpeg_extra.size();
    if (mjpeg_frames) {
        append(c, mjpeg_format(1, uint8_t(mjpeg_frames)));
        if (with_default_mjpeg) {
            append(c, frame_desc(kMjpegFrame, 1, 640, 480, {333333}));
            append(c, frame_desc(kMjpegFrame, 2, 320, 240, {333333, 83333}));
            append(c, frame_desc(kMjpegFrame, 3, 160, 120, {333333, 666666, 1000000, 2000000}));
            // The same size again must merge into the earlier one.
            append(c, frame_desc(kMjpegFrame, 4, 320, 240, {83333}));
            append(c, frame_desc(kMjpegFrame, 5, 320, 240, {333333}));
            append(c, frame_desc(kMjpegFrame, 6, 160, 120, {333333}));
        }
        for (const auto &f : mjpeg_extra)
            append(c, f);
    }
    if (with_yuyv) {
        append(c, yuy2_format(2, 3));
        append(c, frame_desc(kYuyvFrame, 1, 640, 480, {333333}));
        append(c, frame_desc(kYuyvFrame, 2, 320, 240, {333333}));
        append(c, frame_desc(kYuyvFrame, 3, 160, 120, {333333}));
    }
    // Alt 0 has no endpoint, and the other alts are isochronous IN endpoints.
    const std::vector<std::pair<uint8_t, uint16_t>> alts = {
        {1, 128}, {2, 512}, {3, 800}, {4, 1024}, {5, 0x1400}};
    for (const auto &[alt, mps] : alts) {
        append(c, interface_desc(1, alt, 1, 0x0e, 2));
        append(c, endpoint_desc(0x81, 0x05, mps));
        if (ss_companion && alt == 2)
            append(c, {6, 0x30, 0, 0, 0x00, 0x0c});   // wBytesPerInterval = 3072
    }
    c[2] = uint8_t(c.size() & 0xff);
    c[3] = uint8_t(c.size() >> 8);
    return c;
}

const uvc::FrameSize *frame_at(const std::vector<uvc::FrameSize> &v, uint8_t idx) {
    for (const auto &f : v)
        if (f.index == idx)
            return &f;
    return nullptr;
}

uvc::Info parse_ok(const Bytes &c) {
    std::string err;
    auto info = uvc::parse_config(c.data(), c.size(), &err);
    REQUIRE_MESSAGE(info, err);
    return *info;
}

uvc::Info babble_info() {
    std::ifstream in(std::string(BABBLE_TEST_DATA_DIR) + "/babble_tracker_descriptors.bin", std::ios::binary);
    Bytes all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(all.size() > 18);
    return parse_ok(Bytes(all.begin() + 18, all.end()));
}

// A payload with a header of header_len bytes, FID and EOF bits, and n data bytes of value fill.
Bytes packet(size_t total, int fid, bool eof, uint8_t fill, size_t header_len = 12) {
    Bytes p(total, fill);
    p[0] = uint8_t(header_len);
    p[1] = uint8_t((fid & 1) | (eof ? 0x02 : 0) | 0x80);
    return p;
}

bool feed(uvc::FrameAssembler &a, const Bytes &p) { return a.push(p.data(), p.size()); }

}

TEST_CASE("USB camera: GC0308 descriptor parses with MJPEG, YUY2 and every iso alt") {
    const auto info = parse_ok(gc0308());
    CHECK(info.bcd_uvc == 0x0100);
    CHECK(info.control_iface == 0);
    CHECK(info.stream_iface == 1);
    CHECK_FALSE(info.bulk);
    CHECK(info.endpoint == 0x81);
    CHECK(info.mjpeg_format == 1);
    CHECK(info.yuyv_format == 2);
    REQUIRE(info.mjpeg_frames.size() == 6);
    REQUIRE(info.yuyv_frames.size() == 3);
    const auto *f = frame_at(info.mjpeg_frames, 3);
    REQUIRE(f);
    CHECK(f->width == 160);
    CHECK(f->height == 120);
    CHECK_FALSE(f->continuous);
    CHECK(f->intervals == std::vector<uint32_t>{333333, 666666, 1000000, 2000000});
    CHECK(info.yuyv_frames[1].width == 320);
    CHECK(info.yuyv_frames[1].height == 240);

    REQUIRE(info.alts.size() == 5);
    const uint32_t want[] = {128, 512, 800, 1024, 3072};   // 0x1400 is 1024 x 3.
    for (size_t i = 0; i < 5; i++) {
        CHECK(info.alts[i].alt == i + 1);
        CHECK(info.alts[i].endpoint == 0x81);
        CHECK_FALSE(info.alts[i].bulk);
        CHECK(info.alts[i].packet_bytes == want[i]);
    }
}

TEST_CASE("USB camera: a SuperSpeed companion overrides the packet bytes") {
    const auto info = parse_ok(gc0308({}, true, true, true));
    REQUIRE(info.alts.size() == 5);
    CHECK(info.alts[1].packet_bytes == 3072);
    CHECK(info.alts[0].packet_bytes == 128);
    CHECK(info.alts[2].packet_bytes == 800);
}

TEST_CASE("USB camera: a YUY2-only camera has no MJPEG format") {
    const auto info = parse_ok(gc0308({}, true, false));
    CHECK(info.mjpeg_format == 0);
    CHECK(info.mjpeg_frames.empty());
    CHECK(info.yuyv_format == 2);
}

TEST_CASE("USB camera: the Babble tracker still parses as bulk MJPEG and picks 240x240") {
    const auto info = babble_info();
    CHECK(info.bulk);
    CHECK(info.yuyv_format == 0);
    CHECK(info.yuyv_frames.empty());
    CHECK(info.mjpeg_format == 1);
    REQUIRE(info.mjpeg_frames.size() == 4);
    CHECK(info.mjpeg_frames[0].continuous);

    std::string err;
    auto c = uvc::pick_mode(info, "auto", 240, 240, 30, &err);
    REQUIRE_MESSAGE(c, err);
    CHECK_FALSE(c->yuyv);
    CHECK(c->format_index == 1);
    CHECK(c->frame.index == 1);
    // 30 fps is outside the continuous range, so the raw rate is requested like upstream did.
    CHECK(c->interval == 333333);
    c = uvc::pick_mode(info, "auto", 240, 240, 0, &err);
    REQUIRE(c);
    CHECK(c->interval == 166666);
}

TEST_CASE("USB camera: list_modes order, rates and too_large") {
    const auto info = parse_ok(gc0308());
    auto modes = uvc::list_modes(info);
    REQUIRE(modes.size() == 6);
    const uint16_t w[] = {640, 320, 160, 640, 320, 160};
    for (size_t i = 0; i < 6; i++) {
        CHECK(modes[i].yuyv == (i >= 3));
        CHECK(modes[i].width == w[i]);
        CHECK_FALSE(modes[i].too_large);
    }
    REQUIRE(modes[2].fps.size() == 4);
    const double rates[] = {30, 15, 10, 5};
    for (size_t i = 0; i < 4; i++)
        CHECK(modes[2].fps[i] == doctest::Approx(rates[i]).epsilon(0.001));
    REQUIRE(modes[1].fps.size() == 2);   // 320x240 is merged from three frame descriptors.
    CHECK(modes[1].fps[0] == doctest::Approx(120).epsilon(0.001));
    CHECK(modes[1].fps[1] == doctest::Approx(30).epsilon(0.001));

    // A 1280x720 MJPEG frame is listed first, is marked too large, and pick_mode skips it.
    const auto big = parse_ok(gc0308({frame_desc(kMjpegFrame, 7, 1280, 720, {333333})}));
    modes = uvc::list_modes(big);
    REQUIRE(modes.size() == 7);
    CHECK(modes[0].width == 1280);
    CHECK(modes[0].too_large);
    CHECK_FALSE(modes[1].too_large);
    auto c = uvc::pick_mode(big, "auto", 1280, 720, 30);
    REQUIRE(c);
    CHECK(c->frame.width == 640);
}

TEST_CASE("USB camera: pick_mode rules") {
    const auto info = parse_ok(gc0308());
    std::string err;

    // The exact size wins on the Babble tracker, and auto without a matching size takes the largest MJPEG.
    auto c = uvc::pick_mode(info, "auto", 240, 240, 30, &err);
    REQUIRE_MESSAGE(c, err);
    CHECK_FALSE(c->yuyv);
    CHECK(c->frame.width == 640);
    CHECK(c->frame.height == 480);

    c = uvc::pick_mode(info, "yuyv", 320, 240, 30, &err);
    REQUIRE(c);
    CHECK(c->yuyv);
    CHECK(c->format_index == 2);
    CHECK(c->frame.width == 320);

    // The size is missing in YUYV, so the auto rule gives the largest MJPEG.
    c = uvc::pick_mode(info, "yuyv", 200, 200, 30, &err);
    REQUIRE(c);
    CHECK_FALSE(c->yuyv);
    CHECK(c->frame.width == 640);

    // The closest rate: 100 is closest to 120, and 12 is closer to 10 (2) than to 15 (3).
    c = uvc::pick_mode(info, "mjpeg", 320, 240, 100);
    REQUIRE(c);
    CHECK(c->interval == 83333);
    c = uvc::pick_mode(info, "mjpeg", 160, 120, 12);
    REQUIRE(c);
    CHECK(c->interval == 1000000);
    c = uvc::pick_mode(info, "mjpeg", 160, 120, 0);
    REQUIRE(c);
    CHECK(c->interval == 333333);

    // Nothing at or under 640x480.
    const auto huge = parse_ok(gc0308({frame_desc(kMjpegFrame, 1, 1280, 720, {333333})}, false, false));
    CHECK_FALSE(uvc::pick_mode(huge, "auto", 240, 240, 30, &err));
    CHECK(err == "no mode at or under 640\xC3\x97" "480");
}

TEST_CASE("USB camera: duplicate-size descriptors with disjoint rates pick the one that offers the rate") {
    const auto info = parse_ok(gc0308({frame_desc(kMjpegFrame, 1, 320, 240, {333333}),
                                       frame_desc(kMjpegFrame, 2, 320, 240, {83333})},
                                      false, false));
    const auto modes = uvc::list_modes(info);
    REQUIRE(modes.size() == 1);
    REQUIRE(modes[0].fps.size() == 2);
    CHECK(modes[0].fps[0] == doctest::Approx(120).epsilon(0.001));
    CHECK(modes[0].fps[1] == doctest::Approx(30).epsilon(0.001));

    auto c = uvc::pick_mode(info, "mjpeg", 320, 240, 120);
    REQUIRE(c);
    CHECK(c->frame.index == 2);
    CHECK(c->interval == 83333);
    c = uvc::pick_mode(info, "mjpeg", 320, 240, 30);
    REQUIRE(c);
    CHECK(c->frame.index == 1);
    CHECK(c->interval == 333333);
    c = uvc::pick_mode(info, "mjpeg", 320, 240, 0);
    REQUIRE(c);
    CHECK(c->frame.index == 1);
    CHECK(c->interval == 333333);
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

    // A real frame still decodes with padding after EOI.
    std::ifstream in(std::string(BABBLE_TEST_DATA_DIR) + "/babble_tracker_frame.jpg", std::ios::binary);
    Bytes jpg((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(jpg.size() > 4);
    jpg.insert(jpg.end(), 8, 0);
    CHECK(check(jpg));
    Bytes px(240 * 240);
    int w = 0, h = 0;
    REQUIRE(jpeg::decode_gray(jpg.data(), jpg.size(), px.data(), px.size(), &w, &h));
    CHECK(w == 240);
    CHECK(h == 240);
}

TEST_CASE("USB camera: pick_alt takes the smallest iso packet that fits") {
    const auto info = parse_ok(gc0308());
    auto bytes = [&](uint32_t n) { return uvc::pick_alt(info, n); };
    REQUIRE(bytes(800));
    CHECK(bytes(800)->alt == 3);
    REQUIRE(bytes(801));
    CHECK(bytes(801)->alt == 4);
    REQUIRE(bytes(2000));
    CHECK(bytes(2000)->alt == 5);
    CHECK(bytes(2000)->packet_bytes == 3072);
    CHECK(bytes(4000) == nullptr);

    // A bulk camera has no iso alt.
    CHECK(uvc::pick_alt(babble_info(), 100) == nullptr);
}

TEST_CASE("USB camera: yuyv_to_gray copies the Y bytes") {
    const Bytes yuyv = {10, 128, 20, 129, 30, 130, 40, 131,
                        50, 132, 60, 133, 70, 134, 80, 135};
    Bytes gray(8, 0);
    CHECK(uvc::yuyv_to_gray(yuyv.data(), yuyv.size(), 4, 2, gray.data()));
    CHECK(gray == Bytes{10, 20, 30, 40, 50, 60, 70, 80});
    CHECK_FALSE(uvc::yuyv_to_gray(yuyv.data(), yuyv.size() - 1, 4, 2, gray.data()));
    Bytes longer = yuyv;
    longer.push_back(0);
    CHECK_FALSE(uvc::yuyv_to_gray(longer.data(), longer.size(), 4, 2, gray.data()));
}

TEST_CASE("USB camera: FrameAssembler builds frames from iso-sized packets and mark_bad drops one") {
    uvc::FrameAssembler a;
    // Frame data per frame: 788 + 788 + 100 bytes after the 12-byte headers.
    auto send_frame = [&](int fid, uint8_t fill, bool bad_in_middle) {
        bool done = false;
        done |= feed(a, packet(800, fid, false, fill));
        if (bad_in_middle)
            a.mark_bad();
        done |= feed(a, packet(800, fid, false, fill));
        done |= feed(a, packet(112, fid, true, fill));
        return done;
    };

    REQUIRE(send_frame(0, 0xa1, false));
    CHECK(a.frame().size() == 788 * 2 + 100);
    CHECK(a.frame().front() == 0xa1);
    CHECK(a.dropped() == 0);

    // The frame with the lost packet is dropped and the next frame comes through.
    CHECK_FALSE(send_frame(1, 0xb2, true));
    CHECK(a.dropped() == 1);
    REQUIRE(send_frame(0, 0xc3, false));
    CHECK(a.frame().size() == 788 * 2 + 100);
    CHECK(a.frame().front() == 0xc3);
    CHECK(a.dropped() == 1);
}

TEST_CASE("USB camera: option validation") {
    using S = BabbleUsbSource;
    using nlohmann::json;
    CHECK(S::valid_option("format", json("auto")));
    CHECK(S::valid_option("format", json("mjpeg")));
    CHECK(S::valid_option("format", json("yuyv")));
    CHECK_FALSE(S::valid_option("format", json("h264")));
    CHECK_FALSE(S::valid_option("format", json(1)));
    CHECK(S::valid_option("width", json(320)));
    CHECK_FALSE(S::valid_option("width", json(0)));
    CHECK_FALSE(S::valid_option("height", json(70000)));
    CHECK_FALSE(S::valid_option("height", json("240")));
    CHECK(S::valid_option("fps", json(30)));
    CHECK_FALSE(S::valid_option("fps", json(0)));
    CHECK(S::valid_option("camera", json("0c45:6366")));
    CHECK_FALSE(S::valid_option("camera", json(5)));
}
