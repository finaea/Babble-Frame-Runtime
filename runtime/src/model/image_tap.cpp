#include "model/image_tap.h"

#include "core/clock.h"

#include <cstring>

namespace babble {

void ImageTap::offer(img::View v) {
    if (!open_ || !v.data || v.width == 0 || v.height == 0)
        return;
    const uint64_t now = mono_ns();
    if (max_hz_ > 0 && now - last_ns_ < uint64_t(1e9 / max_hz_))
        return;
    last_ns_ = now;
    auto img = std::make_shared<PreviewImage>();
    img->width = v.width;
    img->height = v.height;
    img->pixels.resize(size_t(v.width) * v.height);
    for (uint32_t y = 0; y < v.height; y++)
        std::memcpy(&img->pixels[size_t(y) * v.width], v.data + size_t(y) * v.stride, v.width);
    std::lock_guard lock(mutex_);
    img->seq = ++seq_;
    image_ = std::move(img);
}

std::shared_ptr<const PreviewImage> ImageTap::latest() const {
    std::lock_guard lock(mutex_);
    return image_;
}

}
