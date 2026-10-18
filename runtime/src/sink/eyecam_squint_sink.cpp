#include "sink/eyecam_squint_sink.h"

#include "core/clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "platform/eye_server_client.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>

namespace {
const babble::log::Logger LOG("squint");

constexpr uint32_t kSide = 400;                       // eyecam works on 400x400 frames.
constexpr uint64_t kNewWearNs = 5000000000ull;        // A gap in the frames this long means the headset was off.
constexpr uint64_t kEyeSampleAgeNs = 200000000ull;    // An older eye-service sample counts as missing.
constexpr uint64_t kInputLostNs = 1000000000ull;      // No eye pair for this long sends squint 0 once.
const char *const kStepLabels[] = {"lead_in", "close", "normal", "widen", "squint", "look_up", "look_down"};

int step_index(const char *label) {
    for (int i = 0; i < 7; i++)
        if (std::strcmp(kStepLabels[i], label) == 0)
            return i;
    return 0;
}
}

namespace babble {

EyecamSquintSink::EyecamSquintSink(const nlohmann::json &cfg, SinkDeps deps)
    : name_(deps.name), thread_spec_(thread_spec(deps.threads, cfg.value("thread", "squint"), "bbl-squint")),
      state_(deps.state), stats_(deps.stats), eye_server_(deps.eye_server) {
    calib_path_ = paths::config_dir() / cfg.value("calib_file", std::string("eyecam_calib.json"));
    import_from_ = paths::expand_home(cfg.value("import_from", std::string()));
    active_ = cfg.value("active", true);
    max_hz_ = cfg.value("max_hz", 36.0);
    if (auto it = cfg.find("ranges"); it != cfg.end() && it->is_object())
        for (auto r = it->begin(); r != it->end(); ++r)
            set_option("ranges/" + r.key(), r.value());
    for (auto &img : img_)
        img.resize(size_t(kSide) * kSide);

    const char *eyes[2] = {"Left", "Right"};
    for (int e = 0; e < 2; e++) {
        squint_id_[e] = state_.add_signal(std::string(eyes[e]) + "EyeSquint");
        pre_id_[e] = state_.add_signal("_" + std::string(eyes[e]) + "EyeSquintPre");
        fsq_id_[e] = state_.add_signal("_" + std::string(eyes[e]) + "EyeSquintFsq");
    }
    ready_id_ = state_.add_signal("_SquintReady");
    calibrated_id_ = state_.add_signal("_SquintCalibrated");
    imported_id_ = state_.add_signal("_SquintCalImported");
    cal_state_id_ = state_.add_signal("_SquintCalState");
    cal_step_id_ = state_.add_signal("_SquintCalStep");
    cal_seconds_id_ = state_.add_signal("_SquintCalSecondsLeft");
    cal_fail_id_ = state_.add_signal("_SquintCalFail");
}

EyecamSquintSink::~EyecamSquintSink() {
    stop();
    if (engine_)
        eyecam_free(engine_);
}

void EyecamSquintSink::import_calibration() {
    // The marker file remembers that the parameters came from eyecam's file until a calibration here replaces them.
    const auto marker = std::filesystem::path(calib_path_.string() + ".imported");
    std::error_code ec;
    if (!std::filesystem::exists(calib_path_, ec) && !import_from_.empty() && std::filesystem::exists(import_from_, ec)) {
        if (std::filesystem::copy_file(import_from_, calib_path_, ec)) {
            std::ofstream(marker).put('\n');
            LOG.info("%s: imported eyecam's calibration from %s", name_.c_str(), import_from_.c_str());
        } else {
            LOG.warn("%s: cannot copy %s: %s", name_.c_str(), import_from_.c_str(), ec.message().c_str());
        }
    }
    imported_ = std::filesystem::exists(marker, ec);
}

bool EyecamSquintSink::start() {
    import_calibration();
    if (!engine_) {
        const auto beeps = paths::runtime_dir() / "eyecam_beeps";
        engine_ = eyecam_new(calib_path_.c_str(), beeps.c_str());
    }
    for (int e = 0; e < 2; e++) {
        const double f = eyecam_fsq(engine_, e);
        fsq_[e] = std::isfinite(f) ? float(f) : 0.0f;
    }
    LOG.info("%s: %s, user calibration %s", name_.c_str(), calib_path_.c_str(),
             eyecam_calibrated(engine_) ? (imported_ ? "imported from eyecam" : "loaded") : "missing (squint stays 0)");
    stats_.add(name_ + ".process", &process_time_);
    stats_.add(name_ + ".fps", &processed_);
    stop_ = false;
    slot_.reopen();
    thread_ = start_thread(thread_spec_, [this] { run(); });
    return true;
}

void EyecamSquintSink::stop() {
    stop_ = true;
    slot_.close();
    if (thread_.joinable())
        thread_.join();
    stats_.remove_prefix(name_ + ".");
    if (eye_server_)
        eye_server_->want(EyeServerClient::Squint, false);
}

void EyecamSquintSink::offer_frames(const std::string &source, const StereoFrame &frames) {
    if (source == "eye" && active_.load(std::memory_order_relaxed))
        slot_.put(frames);
}

bool EyecamSquintSink::set_option(const std::string &key, const nlohmann::json &value) {
    if (key == "active" && value.is_boolean()) {
        active_ = value.get<bool>();
        return true;
    }
    if (key == "max_hz" && value.is_number() && value.get<double>() > 0) {
        max_hz_ = value.get<double>();
        return true;
    }
    if (key == "calibrate" && value.is_number_integer()) {
        calibrate_ = value.get<int64_t>();
        return true;
    }
    if (key.starts_with("ranges/")) {
        const std::string sig = key.substr(7);
        const int e = sig == "LeftEyeSquint" ? 0 : sig == "RightEyeSquint" ? 1 : -1;
        ManualRange r;
        if (e < 0 || !ManualRange::parse(value, &r))
            return false;
        std::lock_guard lock(ranges_mutex_);
        ranges_[e] = r;
        return true;
    }
    return false;
}

bool EyecamSquintSink::upright(const Frame &f, bool flip, uint8_t *out) {
    if (f.fmt != PixelFormat::Gray8 || f.width != kSide || f.height != kSide || f.stride < kSide || !f.data ||
        f.size < size_t(f.stride) * (kSide - 1) + kSide)
        return false;
    for (uint32_t y = 0; y < kSide; y++)
        std::memcpy(out + size_t(y) * kSide, f.data + size_t(flip ? kSide - 1 - y : y) * f.stride, kSide);
    return true;
}

void EyecamSquintSink::run() {
    bool was_active = false;
    // When the eye frames stop (eye tracking off, headset off), the squint goes to 0 once. Pairs that the rate cap
    // skips still count as arriving.
    uint64_t last_arrival_ns = 0;
    bool input_lost = false;
    while (!stop_) {
        const bool active = active_;
        if (active != was_active) {
            was_active = active;
            if (eye_server_)
                eye_server_->want(EyeServerClient::Squint, active);
            if (!active) {
                // Inactive: the squint goes to 0 once, and a running calibration stops.
                slot_.clear();
                if (cal_state_ == kRunning)
                    calib_fail(4);
                squint_[0] = squint_[1] = 0;
                publish(true, 0, 0);
            }
        }
        const int64_t token = calibrate_;
        if (token != calibrate_seen_) {
            calibrate_seen_ = token;
            if (token > 0 && active) {
                calib_start(mono_ns());
            } else if (token > 0) {
                calib_fail(4);
            } else {
                eyecam_calib_cancel(engine_);
                cal_state_ = kIdle;
                cal_fail_ = 0;
            }
        }
        if (!active) {
            publish(false, 0, 0);
            sleep_ns(100000000ull);
            continue;
        }
        auto sf = slot_.wait_take(std::chrono::milliseconds(100));
        const uint64_t now = mono_ns();
        if (sf)
            last_arrival_ns = now;
        bool processed = false;
        if (sf && sf->left && sf->right) {
            // A calibration takes every frame, as eyecam's does.
            const uint64_t period = cal_state_ == kRunning ? 0 : uint64_t(1e9 / max_hz_);
            if (sf->capture_ns - last_processed_ns_ >= period || sf->capture_ns < last_processed_ns_) {
                process(*sf);
                processed = true;
            }
        }
        calib_tick(now);
        if (processed) {
            input_lost = false;
            std::array<ManualRange, 2> r;
            {
                std::lock_guard lock(ranges_mutex_);
                r = ranges_;
            }
            publish(true, r[0].apply(squint_[0]), r[1].apply(squint_[1]));
        } else if (!input_lost && last_arrival_ns && now - last_arrival_ns > kInputLostNs) {
            input_lost = true;
            if (cal_state_ == kRunning)
                calib_fail(4);
            squint_[0] = squint_[1] = 0;
            publish(true, 0, 0);
        } else {
            publish(false, 0, 0);
        }
    }
}

void EyecamSquintSink::process(const StereoFrame &sf) {
    // The left view is the right eye and upright; the right view is the left eye and upside down.
    const Frame *view[2] = {sf.right.get(), sf.left.get()};
    if (!upright(*view[0], true, img_[0].data()) || !upright(*view[1], false, img_[1].data()))
        return;
    if (last_capture_ns_ && sf.capture_ns > last_capture_ns_ + kNewWearNs) {
        LOG.info("%s: no eye frames for %.0f s, starting a new wear", name_.c_str(),
                 (sf.capture_ns - last_capture_ns_) / 1e9);
        eyecam_reset_wear(engine_);
        if (cal_state_ == kRunning)
            calib_fail(4);
    }
    last_capture_ns_ = last_processed_ns_ = sf.capture_ns;

    double pitch[2] = {NAN, NAN}, open[2] = {NAN, NAN};
    if (eye_server_)
        if (auto s = eye_server_->latest(kEyeSampleAgeNs))
            for (int e = 0; e < 2; e++) {
                pitch[e] = std::asin(std::clamp(double(s->gaze[e][1]), -1.0, 1.0)) * 180.0 / std::numbers::pi;
                open[e] = s->openness[e];
            }
    const uint64_t t0 = mono_ns();
    for (int e = 0; e < 2; e++) {
        EyecamOut out{};
        eyecam_process(engine_, e, img_[e].data(), pitch[e], open[e], view[e]->capture_ns / 1e9, &out);
        squint_[e] = std::isfinite(out.squint) ? out.squint : 0.0f;
    }
    process_time_.add(mono_ns() - t0);
    processed_.tick();
}

// The calibration: eyecam's wear protocol, then its user protocol (eyecam-rec.rs:2085-2093).
namespace {
constexpr struct {
    const char *label;
    double seconds;
    bool user;
} kSteps[] = {
    {"lead_in", 3, false}, {"close", 2, false},  {"normal", 5, false},  {"widen", 3, false},     {"normal", 2, false},
    {"widen", 3, false},   {"lead_in", 3, true}, {"squint", 5, true},   {"look_up", 5, true},    {"look_down", 5, true},
};
constexpr int kStepCount = int(sizeof(kSteps) / sizeof(kSteps[0]));
}

void EyecamSquintSink::calib_start(uint64_t now) {
    eyecam_calib_cancel(engine_);
    if (!eyecam_ready(engine_)) {
        LOG.warn("%s: calibration refused, the wear baseline is not ready yet", name_.c_str());
        calib_fail(4);
        return;
    }
    eyecam_calib_begin(engine_, 0);
    LOG.info("%s: calibration started", name_.c_str());
    cal_state_ = kRunning;
    cal_fail_ = 0;
    cal_index_ = 0;
    calib_begin_step(now);
}

void EyecamSquintSink::calib_begin_step(uint64_t now) {
    const auto &s = kSteps[cal_index_];
    eyecam_calib_step(engine_, s.label, now / 1e9, s.seconds);
    eyecam_beep(engine_, s.label);
    step_end_ns_ = now + uint64_t(s.seconds * 1e9);
}

void EyecamSquintSink::calib_tick(uint64_t now) {
    if (cal_state_ != kRunning || now < step_end_ns_)
        return;
    const bool user = kSteps[cal_index_].user;
    cal_index_++;
    if (cal_index_ < kStepCount && kSteps[cal_index_].user == user) {
        calib_begin_step(now);
        return;
    }
    // A protocol is over: fit it, and after the wear part start the user part.
    cal_state_ = kFitting;
    publish(false, 0, 0);
    if (!calib_finish())
        return;
    if (cal_index_ < kStepCount) {
        eyecam_calib_begin(engine_, 1);
        cal_state_ = kRunning;
        calib_begin_step(mono_ns());
        return;
    }
    cal_state_ = kDone;
    imported_ = false;
    std::error_code ec;
    std::filesystem::remove(calib_path_.string() + ".imported", ec);
    eyecam_beep(engine_, "end");
}

bool EyecamSquintSink::calib_finish() {
    EyecamCalibResult res{};
    const bool user = kSteps[cal_index_ - 1].user;
    if (!eyecam_calib_finish(engine_, &res)) {
        LOG.warn("%s: %s calibration failed (reason %d): %s", name_.c_str(), user ? "user" : "wear", res.fail,
                 res.message);
        calib_fail(res.fail ? res.fail : 4);
        return false;
    }
    LOG.info("%s: %s calibration done%s%s", name_.c_str(), user ? "user" : "wear", res.message[0] ? ": " : "",
             res.message);
    if (user)
        for (int e = 0; e < 2; e++)
            fsq_[e] = std::isfinite(res.f_sq[e]) ? res.f_sq[e] : 0.0f;
    return true;
}

void EyecamSquintSink::calib_fail(int reason) {
    eyecam_calib_cancel(engine_);
    cal_state_ = kFailed;
    cal_fail_ = reason;
    eyecam_beep(engine_, "end");
}

void EyecamSquintSink::publish(bool with_squint, float squint_l, float squint_r) {
    const uint64_t now = mono_ns();
    const bool running = cal_state_ == kRunning;
    const float seconds = running ? float(std::max<int64_t>(0, int64_t(step_end_ns_ - now)) / 1e9) : 0.0f;
    const float step = running ? float(step_index(kSteps[cal_index_].label)) : 0.0f;
    auto sig = [now](float v) { return Signal{v, 1.0f, now}; };
    std::vector<std::pair<SignalId, Signal>> u = {
        {ready_id_, sig(eyecam_ready(engine_) ? 1 : 0)},
        {calibrated_id_, sig(eyecam_calibrated(engine_) ? 1 : 0)},
        {imported_id_, sig(imported_ && eyecam_calibrated(engine_) ? 1 : 0)},
        {cal_state_id_, sig(float(cal_state_))},
        {cal_step_id_, sig(step)},
        {cal_seconds_id_, sig(seconds)},
        {cal_fail_id_, sig(float(cal_fail_))},
        {fsq_id_[0], sig(fsq_[0])},
        {fsq_id_[1], sig(fsq_[1])},
    };
    if (with_squint) {
        u.push_back({squint_id_[0], sig(squint_l)});
        u.push_back({squint_id_[1], sig(squint_r)});
        u.push_back({pre_id_[0], sig(squint_[0])});
        u.push_back({pre_id_[1], sig(squint_[1])});
    }
    state_.update(u);
}

}
