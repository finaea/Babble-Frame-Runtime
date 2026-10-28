#pragma once

#include "model/executorch_runner.h"
#include "model/image_tap.h"
#include "model/pipeline.h"
#include "model/postprocess/face_post.h"
#include "model/postprocess/one_euro.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace babble {

namespace face {
// The outputs of the Baballonia mono face model are also the signal names.
inline constexpr std::array<const char *, 45> kShapes = {
    "cheekPuffLeft",   "cheekPuffRight",   "cheekSuckLeft",     "cheekSuckRight",     "jawOpen",
    "jawForward",      "jawLeft",          "jawRight",          "noseSneerLeft",      "noseSneerRight",
    "mouthFunnel",     "mouthPucker",      "mouthLeft",         "mouthRight",         "mouthRollUpper",
    "mouthRollLower",  "mouthShrugUpper",  "mouthShrugLower",   "mouthClose",         "mouthSmileLeft",
    "mouthSmileRight", "mouthFrownLeft",   "mouthFrownRight",   "mouthDimpleLeft",    "mouthDimpleRight",
    "mouthUpperUpLeft", "mouthUpperUpRight", "mouthLowerDownLeft", "mouthLowerDownRight", "mouthPressLeft",
    "mouthPressRight", "mouthStretchLeft", "mouthStretchRight", "tongueOut",          "tongueUp",
    "tongueDown",      "tongueLeft",       "tongueRight",       "tongueRoll",         "tongueBendDown",
    "tongueCurlUp",    "tongueSquish",     "tongueFlat",        "tongueTwistLeft",    "tongueTwistRight",
};
// These are the Baballonia OneEuroFilter defaults for this model.
inline constexpr OneEuroParams kOneEuro{0.5, 3.0, 1.0};
}

// The "babble_mono_face" pipeline runs the Baballonia single-camera face model on a Babble face tracker.
class BabbleMonoFace : public PipelineBase {
public:
    BabbleMonoFace(const nlohmann::json &cfg, PipelineDeps deps) : PipelineBase(cfg, std::move(deps), "infM") {}
    bool init() override;
    // Changes calibration/mode, calibration/ranges/<shape>, roi, rotate, flip, gain or tilt_deg while the pipeline runs.
    bool set_option(const std::string &key, const nlohmann::json &value) override;

    // Convert one camera image into the model input, which is public for the tests.
    void prepare(const Frame &f, float *input);
    // The 224x224 model input for the Image tab of the Mouth page.
    ImageTap &model_view() { return model_view_; }

private:
    void process(const StereoFrame &frames) override;
    std::vector<std::pair<SignalId, Signal>> neutral(uint64_t now_ns) const override;
    void reset_session() override { reset_pending_ = true; }

    struct Roi {
        uint32_t x = 0, y = 0, w = 0, h = 0;
    };
    Roi roi_;
    float gain_ = 1.0f;
    float tilt_deg_ = 0.0f;   // Counter-clockwise, -45 to 45, after the crop, flips and quarter turns.
    ImageTap model_view_;
    int quarter_turns_ = 0;
    bool flip_h_ = false, flip_v_ = false;
    bool usb_mode_ = false;   // The face source is a USB camera, which turns and flips the frame itself.
    std::unique_ptr<ExecuTorchRunner> runner_;
    std::unique_ptr<OneEuro> filter_;
    std::unique_ptr<face::Calibration> calibration_;
    std::array<std::pair<float, float>, face::kShapes.size()> ranges_;   // The manual ranges, kept for a mode change.
    std::mutex options_mutex_;   // Lock this to use roi_, gain_, tilt_deg_, the flips, the rotation, calibration_ or ranges_, because set_option() changes them.
    std::array<SignalId, face::kShapes.size()> ids_{};
    std::array<SignalId, face::kShapes.size()> pre_ids_{};   // "_<shape>Pre" is the value before the calibration.

    std::vector<uint8_t> tmp_a_, tmp_b_, small_;
    std::vector<float> input_, out_;
    std::vector<uint16_t> half_in_, half_out_;
    std::vector<float> values_;
    uint64_t last_capture_ns_ = 0;
    std::atomic<bool> reset_pending_{false};
};

}
