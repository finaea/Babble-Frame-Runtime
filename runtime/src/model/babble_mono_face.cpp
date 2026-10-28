#include "model/babble_mono_face.h"

#include "core/clock.h"
#include "core/log.h"
#include "model/half.h"
#include "model/image_ops.h"
#include "model/postprocess/manual_range.h"

#include <algorithm>
#include <cmath>

namespace {
const babble::log::Logger LOG("pipe.face");
}

namespace babble {

namespace {

constexpr uint32_t kSize = 224;
constexpr size_t kMaxSide = 1920;   // This is the largest image side, in pixels, that prepare() uses (USB frames go up to 1920x1080).

bool is_float(DType t) { return t == DType::F32 || t == DType::F16; }

}

bool BabbleMonoFace::init() {
    if (!bind_input())
        return false;

    const auto roi = cfg_.value("roi", nlohmann::json::array());
    if (roi.is_array() && roi.size() == 4)
        roi_ = {roi[0].get<uint32_t>(), roi[1].get<uint32_t>(), roi[2].get<uint32_t>(), roi[3].get<uint32_t>()};
    gain_ = cfg_.value("gain", 1.0f);
    const int rot = cfg_.value("rotate", 0);
    if (rot != 0 && rot != 90 && rot != 180 && rot != 270) {
        LOG.error("pipeline %s: rotate must be 0/90/180/270, got %d", name_.c_str(), rot);
        return false;
    }
    quarter_turns_ = rot / 90;
    const std::string flip = cfg_.value("flip", "none");
    flip_h_ = flip.find('h') != std::string::npos;
    flip_v_ = flip.find('v') != std::string::npos;

    runner_ = std::make_unique<ExecuTorchRunner>();
    if (!runner_->load(model_path("model")))
        return false;
    const auto in = runner_->inputs();
    const auto out = runner_->outputs();
    const std::vector<int64_t> want_in{1, 1, kSize, kSize};
    if (in.size() != 1 || in[0].shape != want_in || !is_float(in[0].dtype) || out.size() != 1 ||
        !is_float(out[0].dtype) || out[0].elements() != face::kShapes.size()) {
        LOG.error("pipeline %s: model does not match the mono face contract ([1,1,224,224] -> [1,45])", name_.c_str());
        return false;
    }

    if (const auto euro = one_euro_params(cfg_.value("one_euro", nlohmann::json::object()), face::kOneEuro))
        filter_ = std::make_unique<OneEuro>(face::kShapes.size(), *euro);

    const auto cal = cfg_.value("calibration", nlohmann::json::object());
    face::CalibMode mode = face::CalibMode::Auto;
    if (!face::parse_mode(cal.value("mode", "auto"), &mode)) {
        LOG.error("pipeline %s: calibration mode must be auto, raw or manual", name_.c_str());
        return false;
    }
    calibration_ = std::make_unique<face::Calibration>(face::kShapes.size(), mode);
    const auto ranges = cal.value("ranges", nlohmann::json::object());
    ranges_.fill({0.0f, 1.0f});
    for (size_t i = 0; i < face::kShapes.size(); i++) {
        const auto r = ranges.find(face::kShapes[i]);
        if (r != ranges.end() && r->is_array() && r->size() == 2) {
            ranges_[i] = {(*r)[0].get<float>(), (*r)[1].get<float>()};
            calibration_->set_range(i, ranges_[i].first, ranges_[i].second);
        }
    }

    for (size_t i = 0; i < face::kShapes.size(); i++) {
        ids_[i] = deps_.state.add_signal(face::kShapes[i]);
        pre_ids_[i] = deps_.state.add_signal(std::string("_") + face::kShapes[i] + "Pre");
    }

    small_.resize(kSize * kSize);
    input_.resize(kSize * kSize);
    out_.resize(face::kShapes.size());
    values_.resize(face::kShapes.size());
    LOG.info("pipeline %s: mono face model on %s (%s, roi %s, rotate %d, flip %s, gain %.2f, calibration %s)",
             name_.c_str(), source_.c_str(), in[0].dtype == DType::F16 ? "fp16" : "fp32",
             roi_.w ? (std::to_string(roi_.x) + "," + std::to_string(roi_.y) + " " + std::to_string(roi_.w) + "x" +
                       std::to_string(roi_.h)).c_str()
                    : "full",
             rot, flip.c_str(), gain_, cal.value("mode", "auto").c_str());
    return true;
}

bool BabbleMonoFace::set_option(const std::string &key, const nlohmann::json &value) {
    std::lock_guard lock(options_mutex_);
    if (!calibration_)
        return false;
    if (key == "calibration/mode") {
        face::CalibMode mode = face::CalibMode::Auto;
        if (!value.is_string() || !face::parse_mode(value.get<std::string>(), &mode))
            return false;
        if (mode != calibration_->mode()) {
            calibration_ = std::make_unique<face::Calibration>(face::kShapes.size(), mode);
            for (size_t i = 0; i < ranges_.size(); i++)
                calibration_->set_range(i, ranges_[i].first, ranges_[i].second);
            LOG.info("pipeline %s: calibration %s", name_.c_str(), value.get<std::string>().c_str());
        }
        return true;
    }
    if (key.starts_with("calibration/ranges/")) {
        const std::string shape = key.substr(19);
        const auto it = std::find_if(face::kShapes.begin(), face::kShapes.end(),
                                     [&](const char *s) { return shape == s; });
        ManualRange r;
        if (it == face::kShapes.end() || !ManualRange::parse(value, &r))
            return false;
        const size_t i = size_t(it - face::kShapes.begin());
        ranges_[i] = {r.min, r.max};
        calibration_->set_range(i, r.min, r.max);
        return true;
    }
    if (key == "roi") {
        // An empty array selects the full image, and four numbers are x, y, width and height.
        if (!value.is_array() || (!value.empty() && value.size() != 4))
            return false;
        for (const auto &v : value)
            if (!v.is_number() || v.get<double>() < 0)
                return false;
        auto at = [&](size_t i) { return uint32_t(std::lround(value[i].get<double>())); };
        roi_ = value.empty() ? Roi{} : Roi{at(0), at(1), at(2), at(3)};
        LOG.info("pipeline %s: roi %s", name_.c_str(), value.dump().c_str());
        return true;
    }
    if (key == "roi_square")
        return value.is_boolean();   // The square lock of the crop box is for the tab only.
    if (key == "rotate") {
        // The Babble tab sends numbers as doubles, so accept a double that is a whole number.
        if (!value.is_number())
            return false;
        const double rot = value.get<double>();
        if (rot != 0 && rot != 90 && rot != 180 && rot != 270)
            return false;
        quarter_turns_ = int(rot) / 90;
        LOG.info("pipeline %s: rotate %d", name_.c_str(), int(rot));
        return true;
    }
    if (key == "flip") {
        if (!value.is_string())
            return false;
        const std::string flip = value.get<std::string>();
        if (flip != "none" && flip != "h" && flip != "v" && flip != "hv" && flip != "vh")
            return false;
        flip_h_ = flip.find('h') != std::string::npos;
        flip_v_ = flip.find('v') != std::string::npos;
        LOG.info("pipeline %s: flip %s", name_.c_str(), flip.c_str());
        return true;
    }
    return false;
}

void BabbleMonoFace::prepare(const Frame &f, float *input) {
    // An ROI that is empty or does not fit selects the full image, as in the Baballonia ImageTransformer.
    img::View cur{f.data, f.width, f.height, f.stride};
    if (roi_.w && roi_.h && roi_.x + roi_.w <= f.width && roi_.y + roi_.h <= f.height)
        cur = {f.data + size_t(roi_.y) * f.stride + roi_.x, roi_.w, roi_.h, f.stride};
    const uint32_t w = std::min<uint32_t>(cur.width, kMaxSide), h = std::min<uint32_t>(cur.height, kMaxSide);
    cur.width = w;
    cur.height = h;
    // The USB source already applies the rotation and flips of each camera, so these are for special setups.
    if ((gain_ != 1.0f || flip_h_ || flip_v_ || quarter_turns_) && tmp_a_.size() < size_t(w) * h) {
        tmp_a_.resize(size_t(w) * h);
        tmp_b_.resize(size_t(w) * h);
    }

    if (gain_ != 1.0f) {   // The gain saturates and rounds as the OpenCV convertTo does.
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                tmp_a_[size_t(y) * w + x] =
                    uint8_t(std::clamp(std::lround(cur.data[size_t(y) * cur.stride + x] * gain_), 0L, 255L));
        cur = {tmp_a_.data(), w, h, w};
    }
    if (flip_h_ || flip_v_) {
        uint8_t *dst = cur.data == tmp_a_.data() ? tmp_b_.data() : tmp_a_.data();
        if (flip_h_) {
            img::flip_horizontal(cur, {dst, w, h, w});
            cur = {dst, w, h, w};
        }
        if (flip_v_) {
            uint8_t *dst2 = cur.data == tmp_a_.data() ? tmp_b_.data() : tmp_a_.data();
            img::flip_vertical(cur, {dst2, w, h, w});
            cur = {dst2, w, h, w};
        }
    }
    if (quarter_turns_) {
        uint8_t *dst = cur.data == tmp_a_.data() ? tmp_b_.data() : tmp_a_.data();
        const bool swap_wh = quarter_turns_ % 2 == 1;
        img::Mut rotated{dst, swap_wh ? h : w, swap_wh ? w : h, swap_wh ? h : w};
        img::rotate(cur, rotated, quarter_turns_);
        cur = rotated;
    }
    img::Mut small{small_.data(), kSize, kSize, kSize};
    img::resize_linear(cur, small);
    for (size_t i = 0; i < small_.size(); i++)
        input[i] = float(small_[i]) / 255.0f;
}

std::vector<std::pair<SignalId, Signal>> BabbleMonoFace::neutral(uint64_t now) const {
    std::vector<std::pair<SignalId, Signal>> updates;
    for (SignalId id : ids_)
        updates.emplace_back(id, Signal{0.0f, 0.0f, now});
    return updates;
}

void BabbleMonoFace::process(const StereoFrame &sf) {
    if (!sf.left)
        return;
    if (reset_pending_.exchange(false)) {
        if (filter_)
            filter_->reset();
        std::lock_guard lock(options_mutex_);
        calibration_->reset();
        last_capture_ns_ = 0;
    }
    const uint64_t t0 = mono_ns();
    {
        std::lock_guard lock(options_mutex_);
        prepare(*sf.left, input_.data());
    }
    const uint64_t t1 = mono_ns();

    const TensorSpec &is = runner_->inputs()[0], &os = runner_->outputs()[0];
    void *in = input_.data(), *out = out_.data();
    if (is.dtype == DType::F16) {
        half_in_.resize(is.elements());
        to_half(input_.data(), half_in_.data(), is.elements());
        in = half_in_.data();
    }
    if (os.dtype == DType::F16) {
        half_out_.resize(os.elements());
        out = half_out_.data();
    }
    TensorView iv[] = {{in, &is}};
    TensorView ov[] = {{out, &os}};
    if (!runner_->run(iv, ov))
        return;
    if (os.dtype == DType::F16)
        from_half(half_out_.data(), out_.data(), os.elements());
    const uint64_t t2 = mono_ns();

    values_.assign(out_.begin(), out_.end());
    if (filter_) {
        const double dt = last_capture_ns_ && sf.capture_ns > last_capture_ns_
                              ? double(sf.capture_ns - last_capture_ns_) / 1e9
                              : 0.0;
        filter_->apply(values_, dt);
    }
    last_capture_ns_ = sf.capture_ns;

    std::vector<std::pair<SignalId, Signal>> updates;
    updates.reserve(ids_.size() * 2);
    for (size_t i = 0; i < pre_ids_.size(); i++)
        updates.emplace_back(pre_ids_[i], Signal{values_[i], 1.0f, sf.capture_ns});
    {
        std::lock_guard lock(options_mutex_);
        calibration_->apply(values_);
    }
    for (size_t i = 0; i < ids_.size(); i++)
        updates.emplace_back(ids_[i], Signal{values_[i], 1.0f, sf.capture_ns});
    publish(updates);

    preprocess_.add(t1 - t0);
    inference_.add(t2 - t1);
}

}
