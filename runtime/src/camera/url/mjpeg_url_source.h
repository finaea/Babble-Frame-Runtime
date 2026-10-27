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
#include <vector>

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

// The largest JPEG that the parser collects; a longer frame is dropped and the parser looks for the next start.
constexpr size_t kMaxFrame = 1 << 20;

// Builds the value of an "Authorization: Basic" header from username:password.
std::string basic_auth(const std::string &user, const std::string &password);

// Extracts JPEG images from an HTTP response body by their FF D8 (start) and FF D9 (end) markers. The Content-Type
// and any multipart boundaries are ignored, so both multipart/x-mixed-replace and raw concatenated JPEGs work.
// Feed it the bytes in any chunks.
class StreamParser {
public:
    using OnPart = std::function<void(const uint8_t *data, size_t size)>;
    explicit StreamParser(OnPart on_part, size_t max_frame = kMaxFrame)
        : on_part_(std::move(on_part)), max_frame_(max_frame) {}

    // Returns false on a response that is not a 200 or is malformed, and error() tells why.
    bool feed(const uint8_t *data, size_t size);
    const std::string &error() const { return error_; }
    bool connected() const { return stage_ != Stage::Response; }   // True after a 200 response header.
    size_t dropped() const { return dropped_; }                       // Frames dropped for exceeding max_frame.

private:
    enum class Stage { Response, Body };
    enum class Chunk { Size, Data, DataEnd };
    bool parse_response(std::string_view head);
    bool body(const uint8_t *data, size_t size);
    void scan(const uint8_t *data, size_t size);

    OnPart on_part_;
    size_t max_frame_;
    Stage stage_ = Stage::Response;
    std::string head_, error_;
    bool chunked_ = false;
    Chunk chunk_ = Chunk::Size;
    std::string chunk_line_;
    size_t chunk_left_ = 0;
    bool in_frame_ = false;
    uint8_t prev_ = 0;
    std::vector<uint8_t> frame_;
    size_t dropped_ = 0;
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
    std::string auth_;        // The Basic credentials, or empty for none.

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
    Counter restarts_, pool_exhausted_, undecodable_, oversized_;
    LatencyStat decode_time_;
};

}
