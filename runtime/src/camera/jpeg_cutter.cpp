#include "camera/jpeg_cutter.h"

namespace babble {

void JpegCutter::feed(const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        const uint8_t cur = data[i];
        switch (stage_) {
        case Stage::Seek:
            window_ = window_ << 8 | cur;
            if (etvr_ && window_ == 0xFFA0FFA1) {
                stage_ = Stage::Size;
                want_ = 0;
                size_bytes_ = 0;
                window_ = 0;
            } else if ((window_ & 0xFFFF) == 0xFFD8) {
                frame_.clear();
                frame_.push_back(0xFF);
                frame_.push_back(0xD8);
                stage_ = Stage::Marked;
            }
            break;
        case Stage::Size:
            want_ |= size_t(cur) << (8 * size_bytes_++);
            if (size_bytes_ < 2)
                break;
            if (want_ < 4 || want_ > max_frame_) {   // A size that cannot be a JPEG.
                dropped_++;
                stage_ = Stage::Seek;
            } else {
                frame_.clear();
                stage_ = Stage::Sized;
            }
            break;
        case Stage::Sized:
            frame_.push_back(cur);
            if (frame_.size() < want_)
                break;
            // As in Baballonia, a frame that does not start with SOI and end with EOI is dropped.
            if (frame_[0] == 0xFF && frame_[1] == 0xD8 && frame_[want_ - 2] == 0xFF && frame_[want_ - 1] == 0xD9)
                on_frame_(frame_.data(), frame_.size());
            else
                dropped_++;
            frame_.clear();
            stage_ = Stage::Seek;
            break;
        case Stage::Marked:
            if (frame_.size() >= max_frame_) {
                // A frame without an end within the limit is dropped, and the cutter looks for the next start.
                frame_.clear();
                stage_ = Stage::Seek;
                window_ = cur;
                dropped_++;
                break;
            }
            frame_.push_back(cur);
            if (frame_[frame_.size() - 2] == 0xFF && cur == 0xD9) {
                on_frame_(frame_.data(), frame_.size());
                frame_.clear();
                stage_ = Stage::Seek;
                window_ = 0;   // The D9 must not pair with a following byte.
            }
            break;
        }
    }
}

}
