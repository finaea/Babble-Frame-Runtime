#pragma once

#include "core/metrics.h"
#include "core/threading.h"
#include "frame/frame.h"
#include "frame/mailbox.h"
#include "model/postprocess/manual_range.h"
#include "sink/eyecam_ffi.h"
#include "sink/sink.h"
#include "state/expression_state.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace babble {

class EyeServerClient;

// Config type "eyecam_squint" runs eyecam's live engine on the eye frames and publishes LeftEyeSquint and
// RightEyeSquint, and runs the guided squint calibration that /sinks/<name>/calibrate starts.
class EyecamSquintSink : public ISink {
public:
    EyecamSquintSink(const nlohmann::json &cfg, SinkDeps deps);
    ~EyecamSquintSink() override;

    bool start() override;
    void stop() override;

    bool wants_frames() const override { return true; }
    void offer_frames(const std::string &source, const StereoFrame &frames) override;
    bool set_option(const std::string &key, const nlohmann::json &value) override;

    // Copies a 400x400 Gray8 frame into out (400x400) the way eyecam wants it, and returns false for any other frame.
    static bool upright(const Frame &f, bool flip, uint8_t *out);

private:
    enum CalState { kIdle = 0, kRunning = 1, kFitting = 2, kDone = 3, kFailed = 4 };

    void run();
    void process(const StereoFrame &sf);
    void calib_start(uint64_t now);
    void calib_tick(uint64_t now);
    void calib_begin_step(uint64_t now);
    bool calib_finish();
    void calib_fail(int reason);
    void publish(bool with_squint, float squint_l, float squint_r);
    void import_calibration();

    std::string name_;
    ThreadSpec thread_spec_;
    ExpressionState &state_;
    StatsRegistry &stats_;
    EyeServerClient *eye_server_;
    std::filesystem::path calib_path_, import_from_;

    std::atomic<bool> active_{true};
    std::atomic<double> max_hz_{36};
    std::atomic<int64_t> calibrate_{0};
    std::mutex ranges_mutex_;
    std::array<ManualRange, 2> ranges_{};   // Index 0 is the left eye.

    LatestSlot<StereoFrame> slot_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    EyecamSquint *engine_ = nullptr;
    LatencyStat process_time_;
    RateStat processed_;

    // Only the sink thread uses these.
    std::vector<uint8_t> img_[2];
    uint64_t last_capture_ns_ = 0, last_processed_ns_ = 0;
    int64_t calibrate_seen_ = 0;
    bool imported_ = false;
    CalState cal_state_ = kIdle;
    int cal_fail_ = 0, cal_index_ = 0;
    uint64_t step_end_ns_ = 0;
    float fsq_[2] = {0, 0};
    float squint_[2] = {0, 0};

    SignalId squint_id_[2], pre_id_[2], fsq_id_[2];
    SignalId ready_id_, calibrated_id_, imported_id_, cal_state_id_, cal_step_id_, cal_seconds_id_, cal_fail_id_;
};

}
