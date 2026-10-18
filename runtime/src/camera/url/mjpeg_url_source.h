#pragma once

#include "camera/tracking_source.h"
#include "core/metrics.h"
#include "core/threading.h"
#include "frame/frame_pool.h"
#include "frame/mailbox.h"
#include "frame/rate_limiter.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace babble {

namespace mjpeg {

// The parts of an http:// URL that the stream reader needs.
struct Url {
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
    std::string text;   // The normalized URL, for logs and errors.
};

// Reads "http://host[:port][/path]", and a bare host or host:port becomes http://<it>/. Other schemes fail.
bool parse_url(const std::string &in, Url *out, std::string *err);

// Splits a multipart/x-mixed-replace HTTP response into its JPEG parts. Feed it the bytes in any chunks.
class StreamParser {
public:
    using OnPart = std::function<void(const uint8_t *data, size_t size)>;
    explicit StreamParser(OnPart on_part) : on_part_(std::move(on_part)) {}

    // Returns false on a response that is not a 200 multipart stream, and error() tells why.
    bool feed(const uint8_t *data, size_t size);
    const std::string &error() const { return error_; }
    const std::string &boundary() const { return boundary_; }

private:
    enum class Stage { Response, Seek, PartHeaders, Body };
    bool parse_response(std::string_view head);

    OnPart on_part_;
    Stage stage_ = Stage::Response;
    std::string buf_, boundary_, error_;
    size_t body_len_ = 0;   // The Content-Length of the current part, or 0 if the part has none.
};

}

// A mouth camera that is an MJPEG stream over HTTP, such as mouthcam on the Frame.
class MjpegUrlSource : public ITrackingSource {
public:
    enum class State { Disabled, Waiting, Streaming, Backoff, Paused };

    MjpegUrlSource(const nlohmann::json &cfg, SourceDeps deps);
    ~MjpegUrlSource() override;

    void tick(uint64_t now_ns) override;
    void shutdown() override;
    void set_process_fps(double fps) override;
    void set_tracking(bool on) override;
    bool tracking() const override { return tracking_; }
    bool streaming() const override { return state_ == State::Streaming; }
    void set_frame_tap(IFrameTap *tap) override { tap_.store(tap); }
    SourceActivity activity() const override;
    const char *state_name() const override;
    const std::string &last_error() const override { return last_error_; }

private:
    void start();
    void stop(const char *why);
    void run();
    void on_jpeg(const uint8_t *data, size_t size);

    std::string name_;
    ThreadSpec thread_spec_;
    LatestSlot<StereoFrame> &out_;
    StatsRegistry &stats_;
    const SteamVrClient *steamvr_ = nullptr;
    bool pause_when_absent_ = true;
    bool tracking_ = true;   // Only the main thread uses this.
    mjpeg::Url url_;
    std::string url_error_;   // This is set if the URL setting cannot be used.

    std::shared_ptr<FramePool> pool_;
    RateLimiter limiter_;
    std::atomic<IFrameTap *> tap_{nullptr};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> connected_{false};   // True after the server answered with a stream.
    std::atomic<bool> got_frame_{false};
    bool refused_ = false;         // The thread writes this before it sets failed_; true if nothing answered.
    std::string thread_error_;     // The thread writes this before it sets failed_.
    uint32_t seq_ = 0;             // Only the thread uses this.

    State state_ = State::Disabled;
    uint64_t retry_at_ns_ = 0, backoff_ns_ = 0, backoff_min_ns_, backoff_max_ns_;
    std::string last_error_;
    bool logged_waiting_ = false;

    RateStat frames_, processed_;
    Counter restarts_, pool_exhausted_, undecodable_;
    LatencyStat decode_time_;
};

}
