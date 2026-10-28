#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace babble {

// Cuts JPEG images out of a byte stream by their FF D8 (start) and FF D9 (end) markers, and ignores the bytes between
// them. Feed it the bytes in any chunks. With the ETVR header on, a frame can also come as FF A0 FF A1, a 2-byte
// little-endian size and that many bytes of JPEG, as the ESP32 serial firmware sends it.
class JpegCutter {
public:
    using OnFrame = std::function<void(const uint8_t *data, size_t size)>;
    // The largest JPEG that the cutter collects; a longer frame is dropped and the cutter looks for the next start.
    static constexpr size_t kMaxFrame = 1 << 20;

    explicit JpegCutter(OnFrame on_frame, size_t max_frame = kMaxFrame, bool etvr_header = false)
        : on_frame_(std::move(on_frame)), max_frame_(max_frame), etvr_(etvr_header) {}

    void feed(const uint8_t *data, size_t size);
    // Frames dropped for exceeding max_frame, and with the ETVR header also frames that are not a whole JPEG.
    size_t dropped() const { return dropped_; }

private:
    enum class Stage { Seek, Size, Sized, Marked };
    OnFrame on_frame_;
    size_t max_frame_;
    bool etvr_;
    Stage stage_ = Stage::Seek;
    uint32_t window_ = 0;   // The last four bytes while the cutter looks for a start.
    size_t want_ = 0;       // The size from an ETVR header.
    int size_bytes_ = 0;    // The size bytes read so far.
    std::vector<uint8_t> frame_;
    size_t dropped_ = 0;
};

}
