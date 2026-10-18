#pragma once

#include "sink/sink.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace babble {

// A grayscale copy of one camera frame, with no row padding.
struct PreviewImage {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    uint64_t seq = 0;   // Increments for each new copy; 0 means no frame yet.
};

// Config type "frame_preview" keeps the newest frame of one source for the crop preview on the Mouth page.
// It copies frames only while the page is open, at most max_hz times a second.
class FramePreviewSink : public ISink {
public:
    FramePreviewSink(const nlohmann::json &cfg, SinkDeps deps);

    bool start() override { return true; }
    void stop() override { open_ = false; }
    // The frame-tap list is fixed at start, so this is true from construction.
    bool wants_frames() const override { return true; }
    void offer_frames(const std::string &source, const StereoFrame &frames) override;
    bool set_option(const std::string &key, const nlohmann::json &value) override;

    // The dashboard sets this while the Mouth page is visible.
    void set_open(bool open) { open_ = open; }
    // Returns the newest copy, or null if there is none yet.
    std::shared_ptr<const PreviewImage> latest() const;

private:
    std::string source_;
    std::atomic<bool> open_{false};
    std::atomic<double> max_hz_;
    std::atomic<uint64_t> last_ns_{0};
    mutable std::mutex mutex_;   // This mutex guards image_ and seq_.
    std::shared_ptr<const PreviewImage> image_;
    uint64_t seq_ = 0;
};

}
