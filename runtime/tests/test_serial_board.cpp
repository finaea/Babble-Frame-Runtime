// These tests cut JPEGs out of synthetic byte streams and match ESP32 serial boards on synthetic descriptors. No USB
// device is opened.
#include <doctest/doctest.h>

#include "camera/jpeg_cutter.h"
#include "camera/usb/serial_board.h"
#include "camera/usb/uvc.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace babble;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes read_file(const std::string &name) {
    std::ifstream in(std::string(BABBLE_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void append(Bytes &out, const Bytes &more) { out.insert(out.end(), more.begin(), more.end()); }

// Makes a JPEG-shaped frame: SOI, n filler bytes that never contain FF, and EOI. The seed tells the frames apart.
Bytes jpeg(size_t n, uint8_t seed = 1) {
    Bytes b{0xFF, 0xD8};
    for (size_t i = 0; i < n; i++)
        b.push_back(uint8_t(0x10 + (seed + i) % 0x80));
    b.push_back(0xFF);
    b.push_back(0xD9);
    return b;
}

// Wraps a payload in the ETVR header: FF A0 FF A1 and a 2-byte little-endian size.
Bytes etvr(const Bytes &payload) {
    Bytes b{0xFF, 0xA0, 0xFF, 0xA1, uint8_t(payload.size() & 0xff), uint8_t(payload.size() >> 8)};
    append(b, payload);
    return b;
}

struct Cut {
    std::vector<Bytes> frames;
    JpegCutter cutter;
    explicit Cut(size_t max_frame = JpegCutter::kMaxFrame, bool etvr_header = false)
        : cutter([this](const uint8_t *d, size_t n) { frames.emplace_back(d, d + n); }, max_frame, etvr_header) {}
    void feed(const Bytes &b, size_t from = 0, size_t to = size_t(-1)) {
        if (to > b.size())
            to = b.size();
        cutter.feed(b.data() + from, to - from);
    }
    // Feeds the bytes in chunks of this size.
    void feed_chunked(const Bytes &b, size_t chunk) {
        for (size_t i = 0; i < b.size(); i += chunk)
            feed(b, i, i + chunk);
    }
};

// Feeds the stream whole, one byte at a time, in odd chunks, and split in two at every byte boundary, and checks that
// each way emits exactly the expected frames and the expected drop count.
void check_every_split(const Bytes &stream, const std::vector<Bytes> &expect, size_t max_frame, bool etvr_header,
                       size_t expect_dropped = 0) {
    auto check = [&](Cut &c) {
        CHECK(c.frames == expect);
        CHECK(c.cutter.dropped() == expect_dropped);
    };
    {
        Cut c(max_frame, etvr_header);
        c.feed(stream);
        check(c);
    }
    for (size_t chunk : {size_t(1), size_t(3), size_t(5), size_t(7)}) {
        Cut c(max_frame, etvr_header);
        c.feed_chunked(stream, chunk);
        check(c);
    }
    for (size_t k = 0; k <= stream.size(); k++) {
        Cut c(max_frame, etvr_header);
        c.feed(stream, 0, k);
        c.feed(stream, k);
        check(c);
    }
}

// Makes a made-up configuration descriptor of an ESP32-S3 serial board, 303a:1001.
Bytes serial_config(bool with_iad) {
    Bytes cfg{9, 2, 0, 0, 2, 1, 0, 0x80, 50};
    if (with_iad)
        append(cfg, {8, 11, 0, 2, 2, 2, 0, 0});
    append(cfg, {9, 4, 0, 0, 1, 2, 2, 0, 0});            // Interface 0: CDC ACM.
    append(cfg, {5, 0x24, 0, 0x10, 1});                  // CDC header.
    append(cfg, {5, 0x24, 1, 0, 1});                     // CDC call management.
    append(cfg, {4, 0x24, 2, 2});                        // CDC ACM.
    append(cfg, {5, 0x24, 6, 0, 1});                     // CDC union.
    append(cfg, {7, 5, 0x82, 3, 8, 0, 16});              // Interrupt IN.
    append(cfg, {9, 4, 1, 0, 2, 0x0a, 0, 0, 0});         // Interface 1: CDC data.
    append(cfg, {7, 5, 0x01, 2, 64, 0, 0});              // Bulk OUT.
    append(cfg, {7, 5, 0x81, 2, 64, 0, 0});              // Bulk IN.
    cfg[2] = uint8_t(cfg.size());
    return cfg;
}

usb::Found device(uint16_t vendor, uint16_t product) {
    usb::Found f;
    f.vendor = vendor;
    f.product = product;
    return f;
}

}

TEST_CASE("jpeg cutter without a header finds two frames between junk at every split") {
    const Bytes a = jpeg(30, 1), b = jpeg(12, 40);
    Bytes stream{0x00, 0x11, 0xFF, 0x22};   // Junk, with a lone FF.
    append(stream, a);
    append(stream, {0xAA, 0xBB, 0xCC});
    append(stream, b);
    append(stream, {0xFF, 0x33});
    for (bool etvr_header : {false, true})
        check_every_split(stream, {a, b}, JpegCutter::kMaxFrame, etvr_header);
}

TEST_CASE("jpeg cutter without a header finds back to back frames") {
    const Bytes a = jpeg(5, 1), b = jpeg(6, 2), c = jpeg(0, 3);
    Bytes stream = a;
    append(stream, b);
    append(stream, c);
    for (bool etvr_header : {false, true})
        check_every_split(stream, {a, b, c}, JpegCutter::kMaxFrame, etvr_header);
}

TEST_CASE("jpeg cutter after a truncated frame still delivers the full frame at its end") {
    // A frame that lost its end runs into the next start, so the cutter emits one run that ends with the full frame.
    const Bytes truncated{0xFF, 0xD8, 0x11, 0x22, 0x33};
    const Bytes full = jpeg(10, 7);
    Bytes stream = truncated;
    append(stream, full);
    for (bool etvr_header : {false, true}) {
        Cut c(JpegCutter::kMaxFrame, etvr_header);
        c.feed_chunked(stream, 1);
        REQUIRE(c.frames.size() == 1);
        CHECK(c.frames[0] == stream);
        CHECK(c.cutter.dropped() == 0);
    }
}

TEST_CASE("jpeg cutter drops an oversized frame, counts it and finds the next frame") {
    const Bytes big = jpeg(40, 1), next = jpeg(6, 9);
    Bytes stream{0x00};
    append(stream, big);
    append(stream, {0x22});
    append(stream, next);
    for (bool etvr_header : {false, true})
        check_every_split(stream, {next}, 16, etvr_header, 1);
}

TEST_CASE("jpeg cutter keeps a frame that is exactly at the size limit") {
    const Bytes exact = jpeg(14, 1);   // 18 bytes.
    for (bool etvr_header : {false, true})
        check_every_split(exact, {exact}, exact.size(), etvr_header);
}

TEST_CASE("jpeg cutter with the ETVR header emits the JPEG without the header") {
    const Bytes a = jpeg(20, 1), b = jpeg(9, 2), c = jpeg(33, 3);
    Bytes stream = etvr(a);
    append(stream, etvr(b));
    append(stream, etvr(c));
    check_every_split(stream, {a, b, c}, JpegCutter::kMaxFrame, true);
}

TEST_CASE("jpeg cutter with the ETVR header skips junk between frames") {
    const Bytes a = jpeg(20, 1), b = jpeg(9, 2);
    Bytes stream{0x01, 0xFF, 0xA0, 0x02};   // The junk has half a header marker.
    append(stream, etvr(a));
    append(stream, {0x55, 0x66});
    append(stream, etvr(b));
    check_every_split(stream, {a, b}, JpegCutter::kMaxFrame, true);
}

TEST_CASE("jpeg cutter with the ETVR header drops a sized frame that does not end FF D9 and recovers") {
    const Bytes bad{0xFF, 0xD8, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};   // Sized 8, but no EOI.
    const Bytes good = jpeg(10, 4);
    Bytes stream = etvr(bad);
    append(stream, etvr(good));
    check_every_split(stream, {good}, JpegCutter::kMaxFrame, true, 1);
}

TEST_CASE("jpeg cutter with the ETVR header drops a sized frame that does not start FF D8") {
    Bytes bad = jpeg(6, 1);
    bad[0] = 0x00;
    const Bytes good = jpeg(10, 4);
    Bytes stream = etvr(bad);
    append(stream, etvr(good));
    check_every_split(stream, {good}, JpegCutter::kMaxFrame, true, 1);
}

TEST_CASE("jpeg cutter with the ETVR header drops a size under 4 and recovers") {
    const Bytes good = jpeg(10, 4);
    for (uint8_t size : {uint8_t(0), uint8_t(1), uint8_t(3)}) {
        Bytes stream{0xFF, 0xA0, 0xFF, 0xA1, size, 0, 0x11, 0x22};   // Junk follows the header.
        append(stream, etvr(good));
        check_every_split(stream, {good}, JpegCutter::kMaxFrame, true, 1);
    }
}

TEST_CASE("jpeg cutter with the ETVR header accepts the smallest size of 4") {
    const Bytes tiny = jpeg(0);
    REQUIRE(tiny.size() == 4);
    check_every_split(etvr(tiny), {tiny}, JpegCutter::kMaxFrame, true);
}

TEST_CASE("jpeg cutter with the ETVR header drops a size over the limit and recovers") {
    // The payload is filler without FF, so the cutter finds nothing in it before the next header.
    Bytes stream{0xFF, 0xA0, 0xFF, 0xA1, 20, 0};
    append(stream, Bytes(20, 0x11));
    const Bytes good = jpeg(6, 4);   // 10 bytes, within the limit of 16.
    append(stream, etvr(good));
    check_every_split(stream, {good}, 16, true, 1);
}

TEST_CASE("jpeg cutter with the ETVR header reads a size that needs both size bytes") {
    const Bytes big = jpeg(0x1ff - 4, 5);   // 0x1ff bytes, so the size is 01 FF in two bytes.
    Bytes stream = etvr(big);
    REQUIRE(stream[4] == 0xff);
    REQUIRE(stream[5] == 0x01);
    Cut c(JpegCutter::kMaxFrame, true);
    c.feed_chunked(stream, 1);
    REQUIRE(c.frames.size() == 1);
    CHECK(c.frames[0] == big);
}

TEST_CASE("jpeg cutter with the ETVR header takes a stream that mixes header and plain frames") {
    const Bytes a = jpeg(8, 1), b = jpeg(9, 2), c = jpeg(10, 3), d = jpeg(11, 4);
    Bytes stream = etvr(a);
    append(stream, b);   // Plain.
    append(stream, {0x77});
    append(stream, etvr(c));
    append(stream, d);   // Plain.
    check_every_split(stream, {a, b, c, d}, JpegCutter::kMaxFrame, true);
}

TEST_CASE("jpeg cutter without the ETVR header treats a header as junk and still finds the JPEG inside") {
    const Bytes a = jpeg(8, 1);
    check_every_split(etvr(a), {a}, JpegCutter::kMaxFrame, false);
}

TEST_CASE("serial board config of an ESP32 CDC device parses to its data interface") {
    for (bool with_iad : {false, true}) {
        const Bytes cfg = serial_config(with_iad);
        const auto port = serial::parse_config(cfg.data(), cfg.size());
        REQUIRE(port);
        CHECK(port->iface == 1);
        CHECK(port->endpoint == 0x81);
        CHECK(port->max_packet == 64);
    }
}

TEST_CASE("serial board matches an Espressif device with a serial port and nothing else") {
    const Bytes cfg = serial_config(true);
    CHECK(serial::is_board(device(0x303a, 0x1001), cfg));
    CHECK_FALSE(serial::is_board(device(0x1234, 0x1001), cfg));
    CHECK(serial::label(device(0x303a, 0x1001), cfg) == "ESP32 serial board");
    CHECK(serial::label(device(0x303a, 0x1001), cfg) == serial::kLabel);
    // Not a board without a bulk IN endpoint on the data interface.
    Bytes out_only = cfg;
    out_only.resize(out_only.size() - 7);
    out_only[2] = uint8_t(out_only.size());
    CHECK_FALSE(serial::parse_config(out_only.data(), out_only.size()));
    CHECK_FALSE(serial::is_board(device(0x303a, 0x1001), out_only));
    // Not a board with an empty or cut descriptor.
    CHECK_FALSE(serial::is_board(device(0x303a, 0x1001), Bytes{}));
    CHECK_FALSE(serial::is_board(device(0x303a, 0x1001), Bytes{9, 2, 9, 0, 1, 1}));
}

TEST_CASE("serial board camera info is one 240x240 MJPEG format") {
    const Bytes cfg = serial_config(true);
    const auto info = serial::camera_info(device(0x303a, 0x1001), cfg);
    REQUIRE(info);
    REQUIRE(info->formats.size() == 1);
    CHECK(info->formats[0].kind == uvc::FormatKind::Mjpeg);
    REQUIRE(info->formats[0].frames.size() == 1);
    CHECK(info->formats[0].frames[0].width == 240);
    CHECK(info->formats[0].frames[0].height == 240);
}

TEST_CASE("the Babble tracker with a CDC part and UVC is never a serial board") {
    const auto all = read_file("babble_tracker_descriptors.bin");
    REQUIRE(all.size() > 18);
    const Bytes cfg(all.begin() + 18, all.end());
    CHECK_FALSE(serial::parse_config(cfg.data(), cfg.size()));
    CHECK_FALSE(serial::is_board(device(0x303a, 0x8000), cfg));
    CHECK(serial::label(device(0x303a, 0x8000), cfg) != serial::kLabel);
    // The camera info is still the UVC parse of the tracker.
    const auto info = serial::camera_info(device(0x303a, 0x8000), cfg);
    REQUIRE(info);
    CHECK(info->bulk);
    CHECK(info->endpoint == 0x83);
    CHECK(info->stream_iface == 3);
    REQUIRE(info->formats.size() == 1);
    CHECK(info->formats[0].frames.size() == 4);
}

TEST_CASE("an isochronous UVC camera is not a serial board") {
    const auto all = read_file("hd_camera_1e45_0209_descriptors.bin");
    REQUIRE(all.size() > 18);
    const Bytes cfg(all.begin() + 18, all.end());
    CHECK_FALSE(serial::parse_config(cfg.data(), cfg.size()));
    CHECK_FALSE(serial::is_board(device(0x1e45, 0x0209), cfg));
    CHECK_FALSE(serial::is_board(device(0x303a, 0x0209), cfg));
    const auto info = serial::camera_info(device(0x1e45, 0x0209), cfg);
    REQUIRE(info);
    CHECK_FALSE(info->bulk);
    CHECK(info->formats.size() == 2);
}
