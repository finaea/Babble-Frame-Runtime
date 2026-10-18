#include "sink/frame_preview_sink.h"

#include "core/clock.h"

#include <cstring>

namespace babble {

FramePreviewSink::FramePreviewSink(const nlohmann::json &cfg, SinkDeps deps)
    : source_(cfg.value("source", "face")), max_hz_(cfg.value("max_hz", 15.0)) {}

void FramePreviewSink::offer_frames(const std::string &source, const StereoFrame &frames) {
    if (!open_ || source != source_ || !frames.left)
        return;
    const double hz = max_hz_;
    const uint64_t now = mono_ns();
    if (hz > 0 && now - last_ns_ < uint64_t(1e9 / hz))
        return;
    last_ns_ = now;
    const Frame &f = *frames.left;
    if (!f.data || f.width == 0 || f.height == 0)
        return;
    auto img = std::make_shared<PreviewImage>();
    img->width = f.width;
    img->height = f.height;
    img->pixels.resize(size_t(f.width) * f.height);
    const uint32_t stride = f.stride ? f.stride : f.width;
    for (uint32_t y = 0; y < f.height; y++)
        std::memcpy(&img->pixels[size_t(y) * f.width], f.data + size_t(y) * stride, f.width);
    std::lock_guard lock(mutex_);
    img->seq = ++seq_;
    image_ = std::move(img);
}

bool FramePreviewSink::set_option(const std::string &key, const nlohmann::json &value) {
    if (key == "max_hz" && value.is_number() && value.get<double>() >= 0) {
        max_hz_ = value.get<double>();
        return true;
    }
    return false;
}

std::shared_ptr<const PreviewImage> FramePreviewSink::latest() const {
    std::lock_guard lock(mutex_);
    return image_;
}

}
