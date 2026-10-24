#include "camera/usb/uvc_camera.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace {
const babble::log::Logger LOG("usb");
}

namespace babble::uvc {

namespace {

// The class-specific requests to an interface.
constexpr uint8_t kSetCur = 0x01, kGetCur = 0x81;
constexpr uint8_t kToInterface = 0x21, kFromInterface = 0xa1;
constexpr uint16_t kProbeControl = 0x0100, kCommitControl = 0x0200;
constexpr unsigned kTimeoutMs = 1000;

}

Camera::~Camera() { close(); }

bool Camera::open(const usb::Found &found, const std::string &format, uint16_t width, uint16_t height, double fps,
                  std::string *error) {
    close();
    busy_ = false;
    if (!dev_.open(found, error))
        return false;
    const auto config = dev_.config_descriptor();
    auto info = parse_config(config.data(), config.size(), error);
    if (!info) {
        close();
        return false;
    }
    info_ = *info;
    const auto choice = pick_mode(info_, format, width, height, fps, error);
    if (!choice) {
        close();
        return false;
    }
    frame_ = choice->frame;
    yuyv_ = choice->yuyv;
    if (LOG.debug_enabled()) {
        std::string sizes, alts;
        for (const auto &m : list_modes(info_))
            sizes += (sizes.empty() ? "" : ", ") + std::string(m.yuyv ? "yuyv " : "mjpeg ") + std::to_string(m.width) +
                     "x" + std::to_string(m.height) + "@" + std::to_string(m.fps.empty() ? 0 : int(m.fps.front()));
        for (const auto &a : info_.alts)
            alts += (alts.empty() ? "" : ", ") + std::to_string(a.alt) + (a.bulk ? ":bulk " : ":iso ") +
                    std::to_string(a.packet_bytes);
        LOG.debug("%s: UVC %x.%02x, control if %u, stream if %u, alts [%s], modes [%s]; picked %s %u (%ux%u)",
                  found.node.c_str(), info_.bcd_uvc >> 8, info_.bcd_uvc & 0xff, info_.control_iface,
                  info_.stream_iface, alts.c_str(), sizes.c_str(), yuyv_ ? "yuyv" : "mjpeg", frame_.index,
                  frame_.width, frame_.height);
    }
    if (!dev_.claim(info_.control_iface, error) || !dev_.claim(info_.stream_iface, error)) {
        busy_ = dev_.claim_errno() == EBUSY;
        close();
        return false;
    }
    // An isochronous camera negotiates on alternate setting 0, which has no bandwidth.
    dev_.set_interface(info_.stream_iface, info_.bulk ? info_.stream_alt : 0);

    // Send the probe, read the values that the device accepts, and then commit them.
    Probe want;
    want.format_index = choice->format_index;
    want.frame_index = frame_.index;
    want.frame_interval = choice->interval ? choice->interval : frame_.default_interval;
    auto block = encode_probe(want, info_.bcd_uvc);
    const uint16_t len = uint16_t(block.size());
    int r = dev_.control(kToInterface, kSetCur, kProbeControl, info_.stream_iface, block.data(), len, kTimeoutMs);
    if (r < 0 && want.frame_interval != frame_.default_interval) {
        // If the camera refuses the rate, use its default rate.
        LOG.info("%s: the camera refused %.1f fps (%s); using its default %.1f fps", found.node.c_str(),
                 1e7 / want.frame_interval, strerror(-r), 1e7 / frame_.default_interval);
        want.frame_interval = frame_.default_interval;
        block = encode_probe(want, info_.bcd_uvc);
        r = dev_.control(kToInterface, kSetCur, kProbeControl, info_.stream_iface, block.data(), len, kTimeoutMs);
    }
    if (r < 0) {
        if (error)
            *error = std::string("UVC probe: ") + strerror(-r);
        close();
        return false;
    }
    std::vector<uint8_t> got(len);
    r = dev_.control(kFromInterface, kGetCur, kProbeControl, info_.stream_iface, got.data(), len, kTimeoutMs);
    auto accepted = r > 0 ? decode_probe(got.data(), size_t(r)) : std::nullopt;
    probe_ = accepted ? *accepted : want;
    LOG.debug("%s: probe asked format %u frame %u interval %u; camera answered %s: interval %u, max frame %u B, "
              "max payload %u B",
              found.node.c_str(), want.format_index, want.frame_index, want.frame_interval,
              accepted ? "" : "nothing (using ours)", probe_.frame_interval, probe_.max_video_frame_size,
              probe_.max_payload_transfer_size);
    if (!probe_.max_video_frame_size)
        probe_.max_video_frame_size = frame_.max_frame_bytes;
    block = encode_probe(probe_, info_.bcd_uvc);
    r = dev_.control(kToInterface, kSetCur, kCommitControl, info_.stream_iface, block.data(), len, kTimeoutMs);
    if (r < 0) {
        if (error)
            *error = std::string("UVC commit: ") + strerror(-r);
        close();
        return false;
    }

    const size_t frame_bytes = yuyv_ ? size_t(frame_.width) * frame_.height * 2 : 0;
    assembler_ = FrameAssembler(std::max<size_t>({probe_.max_video_frame_size, frame_bytes, 64 * 1024}) * 2);
    if (!info_.bulk) {
        // Use the smallest setting that holds one payload, like the Linux driver, and keep the bandwidth of the bus
        // for other devices. A camera that gives no payload size gets the largest setting.
        const AltSetting *alt = probe_.max_payload_transfer_size ? pick_alt(info_, probe_.max_payload_transfer_size)
                                                                 : pick_alt(info_, info_.endpoint_max_packet);
        if (!alt || !dev_.set_interface(info_.stream_iface, alt->alt)) {
            LOG.info("%s: no isochronous setting for a %u B payload (%s)", found.node.c_str(),
                     probe_.max_payload_transfer_size, alt ? strerror(errno) : "none is large enough");
            if (error)
                *error = "not enough USB bandwidth for this mode, pick a smaller one";
            close();
            return false;
        }
        iso_alt_ = alt->alt;
        LOG.debug("%s: isochronous alt %u, endpoint 0x%02x, %u B packets", found.node.c_str(), alt->alt, alt->endpoint,
                  alt->packet_bytes);
        iso_reader_ = std::make_unique<usb::IsoReader>(dev_, alt->endpoint, alt->packet_bytes);
        if (!iso_reader_->start(error)) {
            close();
            return false;
        }
        return true;
    }

    // Read large transfers for fewer wakeups, because each transfer holds only complete payloads.
    payload_ = probe_.max_payload_transfer_size ? probe_.max_payload_transfer_size : 16384;
    size_t transfer = std::max<size_t>(payload_, 16384 / payload_ * payload_);
    reader_ = std::make_unique<usb::BulkReader>(dev_, info_.endpoint, transfer, 4);
    if (!reader_->start(error)) {
        close();
        return false;
    }
    return true;
}

bool Camera::read(int timeout_ms, const std::function<void(const uint8_t *, size_t)> &on_frame, std::string *error) {
    if (iso_reader_) {
        // Each isochronous packet is one payload, and a lost packet makes its frame bad.
        return iso_reader_->wait(
            timeout_ms,
            [&](const uint8_t *data, size_t size, bool ok) {
                if (!ok) {
                    assembler_.mark_bad();
                } else if (assembler_.push(data, size)) {
                    auto &frame = assembler_.frame();
                    on_frame(frame.data(), frame.size());
                }
            },
            error);
    }
    if (!reader_)
        return false;
    return reader_->wait(
        timeout_ms,
        [&](const uint8_t *data, size_t size) {
            for (size_t at = 0; at < size; at += payload_) {
                if (assembler_.push(data + at, std::min(payload_, size - at))) {
                    auto &frame = assembler_.frame();
                    on_frame(frame.data(), frame.size());
                }
            }
        },
        error);
}

void Camera::close() {
    if (reader_) {
        reader_->stop();
        reader_.reset();
    }
    if (iso_reader_) {
        iso_reader_->stop();
        iso_reader_.reset();
    }
    if (dev_.is_open()) {
        if (iso_alt_) {
            // Alternate setting 0 stops an isochronous stream and frees its bandwidth.
            dev_.set_interface(info_.stream_iface, 0);
        } else if (info_.endpoint && info_.bulk) {
            // A halt on a bulk streaming endpoint stops the stream (UVC 1.1, 2.4.3.2.2).
            dev_.clear_halt(info_.endpoint);
        }
        dev_.close();
    }
    iso_alt_ = 0;
    assembler_.reset();
}

}
