#pragma once

#include "model/image_ops.h"
#include "sink/frame_preview_sink.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace babble {

// Keeps a copy of an image that a pipeline makes, such as the model input, for a page of the Babble tab. It copies
// only while the page is open, at most max_hz times a second, so a closed page costs one atomic load per frame.
class ImageTap {
public:
    explicit ImageTap(double max_hz = 15.0) : max_hz_(max_hz) {}

    // The dashboard sets this while the page shows the image.
    void set_open(bool open) { open_ = open; }
    void offer(img::View v);
    // Returns the newest copy, or null if there is none yet.
    std::shared_ptr<const PreviewImage> latest() const;

private:
    std::atomic<bool> open_{false};
    const double max_hz_;
    uint64_t last_ns_ = 0;   // Only the thread that calls offer() uses this.
    mutable std::mutex mutex_;   // This mutex guards image_ and seq_.
    std::shared_ptr<const PreviewImage> image_;
    uint64_t seq_ = 0;
};

}
