#include "camera/url/mjpeg_url_source.h"

#include "camera/usb/jpeg.h"
#include "core/clock.h"
#include "core/log.h"
#include "platform/steamvr_client.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
const babble::log::Logger LOG("url");
}

namespace babble {

namespace {

// The largest decoded image that the frame buffers accept.
constexpr size_t kMaxPixels = 1280 * 960;
// The longest side that the mono face model's preprocessing takes whole (babble_mono_face.cpp kMaxSide).
constexpr int kMaxSide = 1280;
constexpr uint64_t kConnectTimeoutNs = 3'000'000'000ull;
constexpr uint64_t kStallTimeoutNs = 5'000'000'000ull;

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto &c : out)
        c = char(std::tolower(uint8_t(c)));
    return out;
}

// Returns the value of the header name in the header block head, or an empty string.
std::string header(std::string_view head, std::string_view name) {
    const std::string h = lower(head), want = "\n" + lower(name) + ":";
    const size_t at = ("\n" + h).find(want);
    if (at == std::string::npos)
        return "";
    size_t start = at + want.size() - 1, end = head.find('\n', start);
    if (end == std::string_view::npos)
        end = head.size();
    std::string v(head.substr(start, end - start));
    while (!v.empty() && std::isspace(uint8_t(v.back())))
        v.pop_back();
    while (!v.empty() && std::isspace(uint8_t(v.front())))
        v.erase(v.begin());
    return v;
}

}

namespace mjpeg {

bool parse_url(const std::string &in, Url *out, std::string *err) {
    std::string s = in;
    while (!s.empty() && std::isspace(uint8_t(s.back())))
        s.pop_back();
    while (!s.empty() && std::isspace(uint8_t(s.front())))
        s.erase(s.begin());
    if (s.empty()) {
        *err = "no stream URL set";
        return false;
    }
    if (const size_t scheme = s.find("://"); scheme != std::string::npos) {
        if (lower(s.substr(0, scheme)) != "http") {
            *err = "only http:// stream URLs work, not " + s;
            return false;
        }
        s = s.substr(scheme + 3);
    }
    Url u;
    const size_t slash = s.find('/');
    std::string hostport = s.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : s.substr(slash);
    if (const size_t colon = hostport.rfind(':'); colon != std::string::npos) {
        const std::string port = hostport.substr(colon + 1);
        char *end = nullptr;
        const long p = std::strtol(port.c_str(), &end, 10);
        if (port.empty() || *end || p <= 0 || p > 65535) {
            *err = "bad port in stream URL " + in;
            return false;
        }
        u.port = uint16_t(p);
        hostport.resize(colon);
    }
    if (hostport.empty()) {
        *err = "no host in stream URL " + in;
        return false;
    }
    u.host = hostport;
    u.text = "http://" + u.host + (u.port == 80 ? "" : ":" + std::to_string(u.port)) + u.path;
    *out = std::move(u);
    return true;
}

std::string basic_auth(const std::string &user, const std::string &password) {
    static const char *const k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::string in = user + ":" + password;
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        if (i + 1 < in.size())
            v |= uint32_t(uint8_t(in[i + 1])) << 8;
        if (i + 2 < in.size())
            v |= uint8_t(in[i + 2]);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < in.size() ? k[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? k[v & 63] : '=';
    }
    return out;
}

bool StreamParser::parse_response(std::string_view head) {
    const size_t eol = head.find('\n');
    const std::string status(head.substr(0, eol));
    if (status.rfind("HTTP/", 0) != 0 || status.find(" 200") == std::string::npos) {
        error_ = "server answered \"" + std::string(status.substr(0, status.find('\r'))) + "\"";
        return false;
    }
    chunked_ = lower(header(head, "transfer-encoding")).find("chunked") != std::string::npos;
    return true;
}

// The JPEG marker scan: everything outside FF D8 ... FF D9 (boundaries, part headers) is ignored.
void StreamParser::scan(const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        const uint8_t cur = data[i];
        if (!in_frame_) {
            if (prev_ == 0xFF && cur == 0xD8) {
                frame_.clear();
                frame_.push_back(0xFF);
                frame_.push_back(0xD8);
                in_frame_ = true;
            }
        } else if (frame_.size() >= max_frame_) {
            // A frame without an end within the limit is dropped, and the scan looks for the next start.
            frame_.clear();
            in_frame_ = false;
            dropped_++;
        } else {
            frame_.push_back(cur);
            if (prev_ == 0xFF && cur == 0xD9) {
                on_part_(frame_.data(), frame_.size());
                frame_.clear();
                in_frame_ = false;
                prev_ = 0;   // The D9 must not pair with a following byte.
                continue;
            }
        }
        prev_ = cur;
    }
}

bool StreamParser::body(const uint8_t *data, size_t size) {
    if (!chunked_) {
        scan(data, size);
        return true;
    }
    size_t i = 0;
    while (i < size) {
        switch (chunk_) {
        case Chunk::Size: {
            const uint8_t c = data[i++];
            if (c != '\n') {
                chunk_line_ += char(c);
                if (chunk_line_.size() > 1024) {
                    error_ = "bad chunk size line";
                    return false;
                }
                break;
            }
            char *end = nullptr;
            chunk_left_ = size_t(std::strtoull(chunk_line_.c_str(), &end, 16));
            if (end == chunk_line_.c_str()) {
                error_ = "bad chunk size line";
                return false;
            }
            chunk_line_.clear();
            if (chunk_left_ == 0) {
                error_ = "the stream ended";
                return false;
            }
            chunk_ = Chunk::Data;
            break;
        }
        case Chunk::Data: {
            const size_t n = std::min(chunk_left_, size - i);
            scan(data + i, n);
            i += n;
            chunk_left_ -= n;
            if (chunk_left_ == 0)
                chunk_ = Chunk::DataEnd;
            break;
        }
        case Chunk::DataEnd:
            if (data[i++] == '\n')
                chunk_ = Chunk::Size;
            break;
        }
    }
    return true;
}

bool StreamParser::feed(const uint8_t *data, size_t size) {
    if (stage_ == Stage::Response) {
        head_.append(reinterpret_cast<const char *>(data), size);
        const size_t end = head_.find("\r\n\r\n");
        if (end == std::string::npos)
            return head_.size() < 64 * 1024 || (error_ = "no HTTP response header", false);
        if (!parse_response(std::string_view(head_).substr(0, end + 2)))
            return false;
        stage_ = Stage::Body;
        const std::string rest = head_.substr(end + 4);
        head_.clear();
        head_.shrink_to_fit();
        return body(reinterpret_cast<const uint8_t *>(rest.data()), rest.size());
    }
    return body(data, size);
}

}

MjpegUrlSource::MjpegUrlSource(const nlohmann::json &cfg, SourceDeps deps)
    : name_(deps.name), thread_spec_(thread_spec(deps.threads, cfg.value("thread", "face"), "bbl-face")),
      out_(deps.out), stats_(deps.stats), steamvr_(deps.steamvr),
      pause_when_absent_(cfg.value("pause_when_absent", true)), tracking_(cfg.value("tracking", true)),
      limiter_(cfg.value("process_fps", 30.0)) {
    const auto backoff = cfg.value("reopen_backoff_ms", nlohmann::json::object());
    backoff_min_ns_ = uint64_t(backoff.value("min", 1000)) * 1000000ull;
    backoff_max_ns_ = uint64_t(backoff.value("max", 10000)) * 1000000ull;
    state_ = cfg.value("enabled", true) ? State::Waiting : State::Disabled;
    if (!mjpeg::parse_url(cfg.value("url", ""), &url_, &url_error_))
        last_error_ = url_error_;
    if (const std::string user = cfg.value("username", ""); !user.empty())
        auth_ = mjpeg::basic_auth(user, cfg.value("password", ""));

    stats_.add(name_ + ".fps", &frames_);
    stats_.add(name_ + ".process_fps", &processed_);
    stats_.add(name_ + ".restarts", &restarts_);
    stats_.add(name_ + ".pool_exhausted", &pool_exhausted_);
    stats_.add(name_ + ".undecodable", &undecodable_);
    stats_.add(name_ + ".oversized", &oversized_);
    stats_.add(name_ + ".decode", &decode_time_);
}

MjpegUrlSource::~MjpegUrlSource() {
    shutdown();
    stats_.remove_prefix(name_ + ".");
}

void MjpegUrlSource::set_process_fps(double fps) {
    limiter_.set_fps(fps);
    LOG.info("%s: process rate %.1f fps", name_.c_str(), fps);
}

void MjpegUrlSource::set_tracking(bool on) {
    if (on != tracking_)
        LOG.info("%s: tracking turned %s", name_.c_str(), on ? "on" : "off");
    tracking_ = on;
}

SourceActivity MjpegUrlSource::activity() const {
    switch (state_) {
    case State::Disabled: return SourceActivity::Off;
    case State::Waiting: return url_error_.empty() ? SourceActivity::Starting : SourceActivity::Fault;
    case State::Streaming: return got_frame_ ? SourceActivity::Streaming : SourceActivity::Starting;
    // A server that is not up yet is a wait, but a server that answers wrongly is a fault.
    case State::Backoff: return refused_ ? SourceActivity::Starting : SourceActivity::Fault;
    case State::Paused: return tracking_ ? SourceActivity::Paused : SourceActivity::Off;
    }
    return SourceActivity::Fault;
}

const char *MjpegUrlSource::state_name() const {
    switch (state_) {
    case State::Disabled: return "Disabled";
    case State::Waiting: return "Waiting";
    case State::Streaming: return "Streaming";
    case State::Backoff: return "Backoff";
    case State::Paused: return "Paused";
    }
    return "?";
}

void MjpegUrlSource::start() {
    pool_ = FramePool::create_owned(6, kMaxPixels);
    stop_ = false;
    failed_ = false;
    connected_ = false;
    got_frame_ = false;
    refused_ = false;
    limiter_.reset();
    thread_ = start_thread(thread_spec_, [this] { run(); });
}

void MjpegUrlSource::stop(const char *why) {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    pool_.reset();
    LOG.info("%s: stopped (%s)", name_.c_str(), why);
}

void MjpegUrlSource::on_jpeg(const uint8_t *data, size_t size) {
    const uint64_t now = mono_ns();
    frames_.tick();
    // The frame tap gets all frames, but tracking gets only the frames that the limiter accepts.
    IFrameTap *tap = tap_.load(std::memory_order_relaxed);
    const bool want_tap = tap && tap->tap_wanted();
    const bool forward = limiter_.accept(now);
    if (!forward && !want_tap)
        return;   // Skip the decode because the limiter did not accept the frame.
    auto frame = pool_->acquire();
    if (!frame) {
        pool_exhausted_.add();
        return;
    }
    int w = 0, h = 0;
    uint8_t *dst = const_cast<uint8_t *>(frame->data);
    if (size < 2 || data[0] != 0xFF || data[1] != 0xD8 || !jpeg::decode_gray(data, size, dst, kMaxPixels, &w, &h) ||
        w > kMaxSide || h > kMaxSide) {
        undecodable_.add();
        return;
    }
    decode_time_.add(mono_ns() - now);
    got_frame_ = true;
    frame->origin = FrameOrigin::Face;
    frame->fmt = PixelFormat::Gray8;
    frame->width = uint32_t(w);
    frame->height = uint32_t(h);
    frame->stride = uint32_t(w);
    frame->size = size_t(w) * size_t(h);
    frame->capture_ns = now;   // Use the arrival time because the stream has no timestamps.
    frame->arrival_ns = now;
    frame->seq = ++seq_;
    StereoFrame sf;
    sf.left = std::move(frame);
    sf.capture_ns = now;
    if (want_tap)
        tap->tap_frames(name_, sf);
    if (!forward)
        return;
    processed_.tick();
    out_.put(std::move(sf));
}

void MjpegUrlSource::run() {
    const std::string waiting = "waiting for stream at " + url_.text;
    auto fail = [&](const std::string &err, bool refused) {
        thread_error_ = err;
        refused_ = refused;
        failed_ = true;
    };

    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(url_.host.c_str(), std::to_string(url_.port).c_str(), &hints, &res) != 0 || !res)
        return fail(waiting + " (cannot resolve " + url_.host + ")", true);
    struct Closer {
        int fd = -1;
        ~Closer() {
            if (fd >= 0)
                close(fd);
        }
    } closer;

    // Wait in short steps so that shutdown never waits for the network.
    auto wait_for = [&](short events, uint64_t timeout_ns) -> int {
        const uint64_t until = mono_ns() + timeout_ns;
        while (!stop_) {
            pollfd p{closer.fd, events, 0};
            const int n = poll(&p, 1, 200);
            if (n > 0)
                return 1;
            if (n < 0 && errno != EINTR)
                return -1;
            if (mono_ns() >= until)
                return 0;
        }
        return -1;
    };
    // Try each address in turn, because "localhost" gives ::1 first and many streamers only listen on IPv4.
    std::string err = "no address";
    bool connected = false;
    for (const addrinfo *a = res; a && !stop_ && !connected; a = a->ai_next) {
        const int s = socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, a->ai_protocol);
        if (s < 0) {
            err = std::string("socket: ") + strerror(errno);
            continue;
        }
        if (closer.fd >= 0)
            close(closer.fd);
        closer.fd = s;
        if (connect(s, a->ai_addr, a->ai_addrlen) != 0 && errno != EINPROGRESS) {
            err = strerror(errno);
            continue;
        }
        if (wait_for(POLLOUT, kConnectTimeoutNs) <= 0) {
            err = "no answer";
            continue;
        }
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr) {
            err = strerror(soerr);
            continue;
        }
        connected = true;
    }
    freeaddrinfo(res);
    if (stop_)
        return;
    if (!connected)
        return fail(waiting + " (" + err + ")", true);
    const int fd = closer.fd;

    // HTTP/1.0 asks the server not to send a chunked body, though the parser handles one too.
    std::string req = "GET " + url_.path + " HTTP/1.0\r\nHost: " + url_.host + ":" + std::to_string(url_.port) +
                      "\r\nUser-Agent: babble-frame-runtime\r\nAccept: */*\r\n";
    if (!auth_.empty())
        req += "Authorization: Basic " + auth_ + "\r\n";
    req += "\r\n";
    if (send(fd, req.data(), req.size(), MSG_NOSIGNAL) != ssize_t(req.size()))
        return fail(waiting + " (" + strerror(errno) + ")", true);

    mjpeg::StreamParser parser([this](const uint8_t *d, size_t n) { on_jpeg(d, n); });
    std::vector<uint8_t> buf(64 * 1024);
    while (!stop_) {
        const int w = wait_for(POLLIN, kStallTimeoutNs);
        if (w < 0)
            return stop_ ? void() : fail(std::string("poll: ") + strerror(errno), false);
        if (w == 0)
            return fail("the stream at " + url_.text + " stalled", false);
        const ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (n <= 0)
            return fail(n == 0 ? "the stream at " + url_.text + " ended" : std::string("recv: ") + strerror(errno),
                        !connected_);
        const size_t dropped = parser.dropped();
        const bool ok = parser.feed(buf.data(), size_t(n));
        for (size_t i = dropped; i < parser.dropped(); i++)
            oversized_.add();
        if (!ok)
            return fail(url_.text + ": " + parser.error(), false);
        if (!connected_ && parser.connected()) {
            connected_ = true;
            LOG.info("%s: streaming %s", name_.c_str(), url_.text.c_str());
        }
    }
}

void MjpegUrlSource::tick(uint64_t now) {
    // Pause only if SteamVR reports that the headset is not worn.
    const bool absent = pause_when_absent_ && steamvr_ && steamvr_->worn() == Worn::No;
    const bool hold = !tracking_ || absent;
    const char *why = !tracking_ ? "tracking turned off" : "headset not worn";
    switch (state_) {
    case State::Disabled: return;
    case State::Paused:
        if (!hold) {
            LOG.info("%s: resuming", name_.c_str());
            state_ = State::Waiting;
            retry_at_ns_ = 0;
        }
        return;
    case State::Streaming:
        if (hold) {
            stop(why);
            state_ = State::Paused;
            return;
        }
        if (got_frame_ && backoff_ns_) {   // The stream works again, so the next failure starts at the minimum backoff.
            backoff_ns_ = 0;
            last_error_.clear();
            logged_waiting_ = false;
        }
        if (failed_) {
            last_error_ = thread_error_;
            stop(last_error_.c_str());
            restarts_.add();
            backoff_ns_ = backoff_ns_ ? std::min(backoff_ns_ * 2, backoff_max_ns_) : backoff_min_ns_;
            retry_at_ns_ = now + backoff_ns_;
            state_ = State::Backoff;
        }
        return;
    case State::Waiting:
    case State::Backoff:
        if (hold) {
            state_ = State::Paused;
            return;
        }
        if (!url_error_.empty()) {
            if (!logged_waiting_)
                LOG.warn("%s: %s", name_.c_str(), url_error_.c_str());
            logged_waiting_ = true;
            last_error_ = url_error_;
            return;
        }
        if (now < retry_at_ns_)
            return;
        if (!logged_waiting_)
            LOG.info("%s: connecting to %s", name_.c_str(), url_.text.c_str());
        logged_waiting_ = true;
        start();
        state_ = State::Streaming;
        return;
    }
}

void MjpegUrlSource::shutdown() {
    if (state_ == State::Streaming) {
        stop("shutdown");
        state_ = State::Waiting;
    }
}

}
