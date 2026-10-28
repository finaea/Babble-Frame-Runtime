#include "camera/usb/serial_board.h"

#include <algorithm>
#include <cerrno>

namespace babble::serial {

namespace {

constexpr uint8_t kInterface = 4, kEndpoint = 5;
constexpr uint8_t kCdcDataClass = 0x0a, kVideoClass = 0x0e, kStreamingSub = 0x02;
// The reads are 4096 bytes, as in acutevr's bridge, which works on a real board.
constexpr size_t kTransferBytes = 4096;
constexpr uint32_t kInterval = 333333;   // 30 fps, in 100 ns units.
constexpr uint16_t kSide = 240;

}

std::optional<Port> parse_config(const uint8_t *data, size_t size) {
    std::optional<Port> port;
    uint8_t cur_iface = 0xff, cur_class = 0, cur_sub = 0;
    for (size_t i = 0; i + 2 <= size;) {
        const uint8_t len = data[i], type = data[i + 1];
        if (len < 2 || i + len > size)
            break;
        const uint8_t *d = data + i;
        if (type == kInterface && len >= 9) {
            cur_iface = d[2];
            cur_class = d[5];
            cur_sub = d[6];
            if (cur_class == kVideoClass && cur_sub == kStreamingSub)
                return std::nullopt;
        } else if (type == kEndpoint && len >= 7 && cur_class == kCdcDataClass && !port && (d[2] & 0x80) &&
                   (d[3] & 0x03) == 0x02) {
            port = Port{cur_iface, d[2], uint16_t(d[4] | d[5] << 8)};
        }
        i += len;
    }
    return port;
}

bool is_board(const usb::Found &f, const std::vector<uint8_t> &config) {
    return f.vendor == kEspressif && parse_config(config.data(), config.size()).has_value();
}

bool is_board(const usb::Found &f) { return f.vendor == kEspressif && is_board(f, usb::config_descriptor(f)); }

std::vector<usb::Found> list_cameras() {
    auto out = usb::list_uvc();
    for (const auto &f : usb::find(kEspressif, 0))
        if (is_board(f))
            out.push_back(f);
    std::sort(out.begin(), out.end(), [](const usb::Found &a, const usb::Found &b) { return a.sysfs < b.sysfs; });
    return out;
}

std::string label(const usb::Found &f, const std::vector<uint8_t> &config) {
    return is_board(f, config) ? kLabel : usb::device_label(f);
}

uvc::Info board_info() {
    uvc::FrameSize fs;
    fs.index = 1;
    fs.width = fs.height = kSide;
    fs.default_interval = kInterval;
    fs.max_frame_bytes = 0xffff;   // The ETVR header has a 2-byte size.
    fs.intervals = {kInterval};
    uvc::Format fm;
    fm.index = 1;
    fm.kind = uvc::FormatKind::Mjpeg;
    fm.frames = {fs};
    uvc::Info info;
    info.bulk = true;
    info.formats = {fm};
    return info;
}

std::optional<uvc::Info> camera_info(const usb::Found &f, const std::vector<uint8_t> &config) {
    if (is_board(f, config))
        return board_info();
    return uvc::parse_config(config.data(), config.size());
}

Board::Board()
    : cutter_([this](const uint8_t *d, size_t n) {
          if (on_frame_)
              (*on_frame_)(d, n);
      }, JpegCutter::kMaxFrame, true) {
    frame_ = board_info().formats[0].frames[0];
    probe_.frame_interval = kInterval;
}

Board::~Board() { close(); }

bool Board::open(const usb::Found &found, const uvc::Mode &, std::string *error) {
    close();
    busy_ = false;
    if (!dev_.open(found, error))
        return false;
    const auto config = dev_.config_descriptor();
    const auto port = parse_config(config.data(), config.size());
    if (!port) {
        if (error)
            *error = "no serial data interface";
        close();
        return false;
    }
    if (!dev_.claim(port->iface, error)) {
        busy_ = dev_.claim_errno() == EBUSY;
        close();
        return false;
    }
    reader_ = std::make_unique<usb::BulkReader>(dev_, port->endpoint, kTransferBytes, 4);
    if (!reader_->start(error)) {
        close();
        return false;
    }
    return true;
}

bool Board::read(int timeout_ms, const std::function<void(const uint8_t *, size_t)> &on_frame, std::string *error) {
    if (!reader_)
        return false;
    on_frame_ = &on_frame;
    const bool ok = reader_->wait(timeout_ms, [&](const uint8_t *data, size_t size) { cutter_.feed(data, size); },
                                  error);
    on_frame_ = nullptr;
    return ok;
}

void Board::close() {
    if (reader_) {
        reader_->stop();
        reader_.reset();
    }
    dev_.close();   // This releases the interface. Unlike a UVC camera, the endpoint gets no clear-halt request.
}

}
