#include <doctest/doctest.h>

#include "camera/tracking_source.h"
#include "camera/url/mjpeg_url_source.h"
#include "core/clock.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace babble;
using json = nlohmann::json;

namespace {

std::string read_jpeg() {
    std::ifstream f(std::string(BABBLE_TEST_DATA_DIR) + "/babble_tracker_frame.jpg", std::ios::binary);
    REQUIRE(f.good());
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// The JPEG that the marker scan emits: everything up to the first FF D9.
std::string first_frame(const std::string &jpeg) {
    return jpeg.substr(0, jpeg.find("\xFF\xD9") + 2);
}

const std::string kHead = "HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace;boundary=frame\r\n\r\n";

std::string part(const std::string &jpeg) {
    return "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpeg.size()) + "\r\n\r\n" + jpeg +
           "\r\n";
}

// Feeds s in chunks of the given size and returns the parts that came out.
std::vector<std::string> run_parser(const std::string &s, size_t chunk, bool *ok = nullptr) {
    std::vector<std::string> parts;
    mjpeg::StreamParser p([&](const uint8_t *d, size_t n) { parts.emplace_back(reinterpret_cast<const char *>(d), n); });
    bool good = true;
    for (size_t i = 0; i < s.size() && good; i += chunk)
        good = p.feed(reinterpret_cast<const uint8_t *>(s.data()) + i, std::min(chunk, s.size() - i));
    if (ok)
        *ok = good;
    return parts;
}

// A local MJPEG server on a free port. Mode Stall accepts clients and sends nothing.
class TestServer {
public:
    explicit TestServer(const std::string &jpeg, bool stall = false) : jpeg_(jpeg), stall_(stall) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0);
        socklen_t len = sizeof(a);
        getsockname(fd_, reinterpret_cast<sockaddr *>(&a), &len);
        port_ = ntohs(a.sin_port);
        REQUIRE(listen(fd_, 4) == 0);
        thread_ = std::thread([this] { run(); });
    }
    ~TestServer() {
        stop_ = true;
        thread_.join();
        close(fd_);
    }
    int port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }
    void kick() { kicks_++; }   // Closes the current client; the server then accepts the next one.

private:
    void run() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0)
                continue;
            const int c = accept(fd_, nullptr, nullptr);
            if (c < 0)
                continue;
            serve(c);
            close(c);
        }
    }
    void serve(int c) {
        const int handled = kicks_;
        std::string req;
        char b[256];
        while (!stop_ && req.find("\r\n\r\n") == std::string::npos) {
            pollfd p{c, POLLIN, 0};
            if (poll(&p, 1, 50) > 0) {
                const ssize_t n = recv(c, b, sizeof(b), 0);
                if (n <= 0)
                    return;
                req.append(b, size_t(n));
            }
        }
        if (stall_) {
            while (!stop_)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return;
        }
        const std::string head = kHead, body = part(jpeg_);
        if (send(c, head.data(), head.size(), MSG_NOSIGNAL) < 0)
            return;
        while (!stop_ && kicks_ == handled) {
            if (send(c, body.data(), body.size(), MSG_NOSIGNAL) < 0)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }

    std::string jpeg_;
    bool stall_;
    int fd_ = -1, port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> kicks_{0};
    std::thread thread_;
};

// Ticks the source until pred() holds or the timeout passes.
template <class Pred>
bool tick_until(ITrackingSource &src, Pred pred, int timeout_ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        src.tick(mono_ns());
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// Ticks until a frame lands in the slot and returns it.
std::optional<StereoFrame> next_frame(ITrackingSource &src, LatestSlot<StereoFrame> &slot, int timeout_ms = 3000) {
    std::optional<StereoFrame> got;
    tick_until(src, [&] { return (got = slot.take()).has_value(); }, timeout_ms);
    return got;
}

json stream_cfg(const std::string &url) {
    return json{{"url", url},
                {"pause_when_absent", false},
                {"process_fps", 1000},
                {"reopen_backoff_ms", {{"min", 50}, {"max", 200}}}};
}

}

TEST_CASE("mjpeg parse_url accepts http, bare hosts and host:port, and rejects the rest") {
    mjpeg::Url u;
    std::string err;
    REQUIRE(mjpeg::parse_url("http://127.0.0.1:8082/", &u, &err));
    CHECK(u.host == "127.0.0.1");
    CHECK(u.port == 8082);
    CHECK(u.path == "/");

    REQUIRE(mjpeg::parse_url("192.168.1.5", &u, &err));
    CHECK(u.text == "http://192.168.1.5/");
    CHECK(u.port == 80);

    REQUIRE(mjpeg::parse_url("host:9000", &u, &err));
    CHECK(u.host == "host");
    CHECK(u.port == 9000);

    err.clear();
    CHECK_FALSE(mjpeg::parse_url("https://x", &u, &err));
    CHECK_FALSE(err.empty());
    err.clear();
    CHECK_FALSE(mjpeg::parse_url("rtsp://x", &u, &err));
    CHECK_FALSE(err.empty());
    err.clear();
    CHECK_FALSE(mjpeg::parse_url("", &u, &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("mjpeg stream parser returns each part byte-identical for any chunking") {
    const std::string jpeg = read_jpeg();
    const std::string stream = kHead + part(jpeg) + part(jpeg) + part(jpeg);

    for (size_t chunk : {stream.size(), size_t(1), size_t(7), size_t(1000)}) {
        CAPTURE(chunk);
        bool ok = false;
        const auto parts = run_parser(stream, chunk, &ok);
        CHECK(ok);
        REQUIRE(parts.size() == 3);   // The last part is complete by its Content-Length, so it needs no trailing boundary.
        for (const auto &p : parts)
            CHECK(p == first_frame(jpeg));
    }

    // The split points land inside the header, the part header and across the boundary.
    const size_t cuts[] = {10, kHead.size() - 1, kHead.size() + 3, kHead.size() + 40};
    mjpeg::StreamParser p([&](const uint8_t *, size_t) {});
    size_t prev = 0;
    for (size_t cut : cuts) {
        CHECK(p.feed(reinterpret_cast<const uint8_t *>(stream.data()) + prev, cut - prev));
        prev = cut;
    }
}

TEST_CASE("mjpeg stream parser handles a part without Content-Length") {
    const std::string jpeg = read_jpeg();
    const std::string stream = kHead + "--frame\r\nContent-Type: image/jpeg\r\n\r\n" + jpeg + "\r\n--frame\r\n";
    for (size_t chunk : {stream.size(), size_t(1)}) {
        CAPTURE(chunk);
        const auto parts = run_parser(stream, chunk);
        REQUIRE(parts.size() == 1);
        CHECK(parts[0] == first_frame(jpeg));
    }
}

TEST_CASE("mjpeg stream parser rejects a non-200 response") {
    bool ok = true;
    run_parser("HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n\r\nnope", 1000, &ok);
    CHECK_FALSE(ok);
}

TEST_CASE("mjpeg stream parser ignores the Content-Type and reads raw concatenated JPEGs") {
    const std::string jpeg = read_jpeg();
    const std::string stream = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\njunk" + jpeg + jpeg + "x" + jpeg;
    for (size_t chunk : {stream.size(), size_t(1), size_t(3)}) {
        CAPTURE(chunk);
        bool ok = false;
        const auto parts = run_parser(stream, chunk, &ok);
        CHECK(ok);
        REQUIRE(parts.size() == 3);
        for (const auto &p : parts)
            CHECK(p == first_frame(jpeg));
    }
}

TEST_CASE("mjpeg stream parser decodes a chunked body") {
    const std::string jpeg = read_jpeg();
    std::string body = part(jpeg) + part(jpeg), stream = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    for (size_t i = 0; i < body.size(); i += 100) {
        const std::string c = body.substr(i, 100);
        char len[16];
        snprintf(len, sizeof(len), "%zx\r\n", c.size());
        stream += len + c + "\r\n";
    }
    for (size_t chunk : {stream.size(), size_t(1), size_t(7)}) {
        CAPTURE(chunk);
        const auto parts = run_parser(stream, chunk);
        REQUIRE(parts.size() == 2);
        CHECK(parts[0] == first_frame(jpeg));
        CHECK(parts[1] == first_frame(jpeg));
    }
}

TEST_CASE("mjpeg stream parser drops a frame past the size limit and recovers") {
    const std::string jpeg = read_jpeg();
    std::vector<std::string> parts;
    mjpeg::StreamParser p([&](const uint8_t *d, size_t n) { parts.emplace_back(reinterpret_cast<const char *>(d), n); },
                          jpeg.size() + 16);
    const std::string stream = kHead + "\xFF\xD8" + std::string(jpeg.size() * 2, 'a') + jpeg;
    CHECK(p.feed(reinterpret_cast<const uint8_t *>(stream.data()), stream.size()));
    CHECK(p.dropped() == 1);
    REQUIRE(parts.size() == 1);
    CHECK(parts[0] == first_frame(jpeg));
}

TEST_CASE("mjpeg basic_auth encodes username:password") {
    CHECK(mjpeg::basic_auth("Aladdin", "open sesame") == "QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
    CHECK(mjpeg::basic_auth("a", "") == "YTo=");
}

TEST_CASE("mjpeg source streams from a local server and reconnects after the server drops it") {
    const std::string jpeg = read_jpeg();
    TestServer server(jpeg);
    json threads = json::object();
    LatestSlot<StereoFrame> slot;
    StatsRegistry stats;
    MjpegUrlSource src(stream_cfg(server.url()), SourceDeps{"mouth", threads, slot, stats, nullptr, nullptr});

    auto first = next_frame(src, slot);
    REQUIRE(first);
    REQUIRE(first->left);
    CHECK(first->left->width > 0);
    CHECK(first->left->height > 0);
    CHECK(first->left->fmt == PixelFormat::Gray8);
    CHECK(first->left->origin == FrameOrigin::Face);
    CHECK(src.activity() == SourceActivity::Streaming);

    const uint32_t seq = first->left->seq;
    server.kick();
    std::optional<StereoFrame> again;
    REQUIRE(tick_until(src, [&] {
        auto f = slot.take();
        if (f && f->left->seq > seq)
            again = std::move(f);
        return again.has_value();
    }));
    CHECK(again->left->seq > seq);
}

TEST_CASE("mjpeg source waits while nothing is listening") {
    int port = 0;
    {
        TestServer server("");   // Takes a free port, then closes it again.
        port = server.port();
    }
    json threads = json::object();
    LatestSlot<StereoFrame> slot;
    StatsRegistry stats;
    MjpegUrlSource src(stream_cfg("http://127.0.0.1:" + std::to_string(port) + "/"),
                       SourceDeps{"mouth", threads, slot, stats, nullptr, nullptr});
    CHECK(tick_until(src, [&] { return src.last_error().rfind("waiting for stream at", 0) == 0; }));
    CHECK(src.activity() == SourceActivity::Starting);
}

TEST_CASE("mjpeg source shutdown does not hang on a stalled server") {
    TestServer server("", true);
    json threads = json::object();
    LatestSlot<StereoFrame> slot;
    StatsRegistry stats;
    MjpegUrlSource src(stream_cfg(server.url()), SourceDeps{"mouth", threads, slot, stats, nullptr, nullptr});
    REQUIRE(tick_until(src, [&] { return src.streaming(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // The thread has connected and waits for data.
    const auto t0 = std::chrono::steady_clock::now();
    src.shutdown();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1000);
}

TEST_CASE("mjpeg face_camera in url mode streams through the wrapper") {
    const std::string jpeg = read_jpeg();
    TestServer server(jpeg);
    json threads = json::object();
    LatestSlot<StereoFrame> slot;
    StatsRegistry stats;
    json cfg = stream_cfg(server.url());
    cfg["type"] = "face_camera";
    cfg["mode"] = "url";
    auto src = make_tracking_source(cfg, SourceDeps{"mouth", threads, slot, stats, nullptr, nullptr});
    REQUIRE(src);
    auto f = next_frame(*src, slot);
    REQUIRE(f);
    CHECK(f->left->origin == FrameOrigin::Face);
    CHECK(f->left->width > 0);
    src->shutdown();
}

TEST_CASE("face_camera in pc mode opens no camera and keeps its tracking switch") {
    const std::string jpeg = read_jpeg();
    TestServer server(jpeg);
    json threads = json::object();
    LatestSlot<StereoFrame> slot;
    StatsRegistry stats;
    json cfg = stream_cfg(server.url());
    cfg["type"] = "face_camera";
    cfg["mode"] = "pc";
    auto src = make_tracking_source(cfg, SourceDeps{"mouth", threads, slot, stats, nullptr, nullptr});
    REQUIRE(src);
    // Tracking stays on, so the PC link still claims face tracking, but nothing streams.
    CHECK(src->tracking());
    for (int i = 0; i < 20; i++) {
        src->tick(mono_ns());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_FALSE(src->streaming());
    CHECK(src->activity() == SourceActivity::Off);
    CHECK_FALSE(slot.take());
    src->set_tracking(false);
    CHECK_FALSE(src->tracking());
    src->set_tracking(true);

    // A live change to the URL mode streams, and a change back stops it.
    REQUIRE(src->set_option("mode", "url"));
    auto f = next_frame(*src, slot);
    REQUIRE(f);
    REQUIRE(src->set_option("mode", "pc"));
    CHECK_FALSE(src->streaming());
    CHECK(src->tracking());
    CHECK_FALSE(src->set_option("mode", "wifi"));
    src->shutdown();
}
