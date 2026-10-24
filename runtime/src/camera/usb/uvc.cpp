#include "camera/usb/uvc.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace babble::uvc {

namespace {

constexpr uint8_t kInterface = 0x04, kEndpoint = 0x05, kClassInterface = 0x24, kSsEndpointCompanion = 0x30;
constexpr uint8_t kVideoClass = 0x0e, kControlSub = 0x01, kStreamingSub = 0x02;
constexpr uint8_t kVcHeader = 0x01, kVsFormatMjpeg = 0x06, kVsFrameMjpeg = 0x07;
constexpr uint8_t kVsFormatUncompressed = 0x04, kVsFrameUncompressed = 0x05;
// The GUID of the YUY2 uncompressed format.
constexpr uint8_t kYuy2Guid[16] = {'Y', 'U', 'Y', '2', 0x00, 0x00, 0x10, 0x00,
                                   0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
void put16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = uint8_t(v >> (8 * i));
}

// Reads a VS_FRAME_MJPEG or VS_FRAME_UNCOMPRESSED descriptor, which have the same layout.
FrameSize parse_frame(const uint8_t *d, uint8_t len) {
    FrameSize f;
    f.index = d[3];
    f.width = u16(d + 5);
    f.height = u16(d + 7);
    f.max_frame_bytes = u32(d + 17);
    f.default_interval = u32(d + 21);
    const uint8_t type = d[25];
    if (type == 0 && len >= 38) {
        f.continuous = true;
        f.intervals = {u32(d + 26), u32(d + 30), u32(d + 34)};
    } else {
        for (size_t i = 0; i < type && 26 + 4 * i + 4 <= len; i++)
            f.intervals.push_back(u32(d + 26 + 4 * i));
    }
    return f;
}

// Returns the advertised rates of a frame size, highest first.
std::vector<double> frame_rates(const FrameSize &f) {
    std::vector<uint32_t> iv;
    if (f.continuous)
        iv = {f.intervals[0], f.default_interval, f.intervals[1]};
    else
        iv = f.intervals;
    if (iv.empty() && f.default_interval)
        iv.push_back(f.default_interval);
    std::vector<double> fps;
    for (uint32_t i : iv)
        if (i)
            fps.push_back(1e7 / double(i));
    std::sort(fps.begin(), fps.end(), std::greater<>());
    fps.erase(std::unique(fps.begin(), fps.end(), [](double a, double b) { return std::abs(a - b) < 0.01; }), fps.end());
    return fps;
}

// Returns the interval of the offered rate closest to fps, or the default interval if fps is 0.
uint32_t pick_interval(const FrameSize &f, double fps) {
    if (fps <= 0)
        return f.default_interval;
    const double want = 1e7 / fps;
    if (f.continuous) {
        // Outside the range, ask for the rate itself like before, and the camera's refusal gives the default rate.
        const uint32_t lo = f.intervals[0], hi = f.intervals[1], step = f.intervals[2];
        if (want < lo || want > hi)
            return uint32_t(want);
        uint32_t i = uint32_t(want);
        if (step)
            i = lo + uint32_t(std::lround(double(i - lo) / step)) * step;
        return std::min(i, hi);
    }
    uint32_t best = f.default_interval;
    double best_diff = 1e18;
    for (uint32_t i : f.intervals) {
        if (!i)
            continue;
        if (const double diff = std::abs(1e7 / double(i) - fps); diff < best_diff) {
            best = i;
            best_diff = diff;
        }
    }
    return best;
}

bool fits(const FrameSize &f) { return uint32_t(f.width) * f.height <= kMaxPixels; }

}

std::optional<Info> parse_config(const uint8_t *data, size_t size, std::string *error) {
    Info info;
    // The interface of the descriptors that follow.
    uint8_t cur_iface = 0xff, cur_alt = 0, cur_class = 0, cur_sub = 0;
    // The format that the frame descriptors that follow belong to, as a frame subtype and a format index.
    uint8_t cur_frame_sub = 0, cur_format = 0;
    bool last_was_iso = false;   // True right after an isochronous endpoint, for its SuperSpeed companion.
    for (size_t i = 0; i + 2 <= size;) {
        const uint8_t len = data[i], type = data[i + 1];
        if (len < 2 || i + len > size)
            break;
        const uint8_t *d = data + i;
        const bool was_iso = last_was_iso;
        last_was_iso = false;
        if (type == kInterface && len >= 9) {
            cur_iface = d[2];
            cur_alt = d[3];
            cur_class = d[5];
            cur_sub = d[6];
            cur_frame_sub = cur_format = 0;
            if (cur_class == kVideoClass && cur_sub == kControlSub && info.control_iface == 0xff)
                info.control_iface = cur_iface;
            if (cur_class == kVideoClass && cur_sub == kStreamingSub && info.stream_iface == 0xff)
                info.stream_iface = cur_iface;
        } else if (type == kEndpoint && len >= 7 && cur_class == kVideoClass && cur_sub == kStreamingSub &&
                   cur_iface == info.stream_iface && (d[2] & 0x80)) {
            AltSetting a;
            a.alt = cur_alt;
            a.endpoint = d[2];
            a.bulk = (d[3] & 0x03) == 0x02;
            // Bits 11-12 of wMaxPacketSize are the number of extra transactions in each microframe.
            const uint16_t mps = u16(d + 4);
            a.packet_bytes = a.bulk ? (mps & 0x7ffu) : (mps & 0x7ffu) * (1 + ((mps >> 11) & 0x3u));
            info.alts.push_back(a);
            last_was_iso = !a.bulk;
        } else if (type == kSsEndpointCompanion && len >= 6 && was_iso && !info.alts.empty()) {
            // A SuperSpeed endpoint gives its real bytes per interval here.
            if (const uint16_t bpi = u16(d + 4))
                info.alts.back().packet_bytes = bpi;
        } else if (type == kClassInterface && len >= 3 && cur_class == kVideoClass) {
            const uint8_t sub = d[2];
            if (cur_sub == kControlSub && sub == kVcHeader && len >= 5) {
                info.bcd_uvc = u16(d + 3);
            } else if (cur_sub == kStreamingSub && sub == kVsFormatMjpeg && len >= 5) {
                cur_frame_sub = kVsFrameMjpeg;
                cur_format = d[3];
                if (!info.mjpeg_format)
                    info.mjpeg_format = cur_format;
            } else if (cur_sub == kStreamingSub && sub == kVsFormatUncompressed && len >= 21 &&
                       std::equal(kYuy2Guid, kYuy2Guid + 16, d + 5)) {
                cur_frame_sub = kVsFrameUncompressed;
                cur_format = d[3];
                if (!info.yuyv_format)
                    info.yuyv_format = cur_format;
            } else if (cur_sub == kStreamingSub && (sub == kVsFrameMjpeg || sub == kVsFrameUncompressed)) {
                if (len >= 26 && sub == cur_frame_sub && sub == kVsFrameMjpeg && cur_format == info.mjpeg_format)
                    info.mjpeg_frames.push_back(parse_frame(d, len));
                else if (len >= 26 && sub == cur_frame_sub && sub == kVsFrameUncompressed &&
                         cur_format == info.yuyv_format)
                    info.yuyv_frames.push_back(parse_frame(d, len));
            } else if (cur_sub == kStreamingSub && sub != 0x0d && sub != 0x03) {
                cur_frame_sub = cur_format = 0;   // These descriptors are for a different format.
            }
        }
        i += len;
    }
    // Use bulk if possible, or else the isochronous endpoint with the largest packet.
    for (const auto &a : info.alts) {
        if (!info.endpoint || (a.bulk && !info.bulk) || (!a.bulk && !info.bulk && a.packet_bytes > info.endpoint_max_packet)) {
            info.endpoint = a.endpoint;
            info.endpoint_max_packet = uint16_t(std::min<uint32_t>(a.packet_bytes, 0xffff));
            info.bulk = a.bulk;
            info.stream_alt = a.alt;
        }
    }
    if (info.stream_iface == 0xff || !info.endpoint) {
        if (error)
            *error = "no video streaming interface";
        return std::nullopt;
    }
    if (info.mjpeg_frames.empty())
        info.mjpeg_format = 0;
    if (info.yuyv_frames.empty())
        info.yuyv_format = 0;
    if (!info.mjpeg_format && !info.yuyv_format) {
        if (error)
            *error = "no MJPEG or YUYV format";
        return std::nullopt;
    }
    return info;
}

const FrameSize *pick_frame(const Info &info, uint16_t width, uint16_t height) {
    for (const auto &f : info.mjpeg_frames)
        if (f.width == width && f.height == height)
            return &f;
    return info.mjpeg_frames.empty() ? nullptr : &info.mjpeg_frames.front();
}

std::vector<Mode> list_modes(const Info &info) {
    std::vector<Mode> out;
    for (const bool yuyv : {false, true}) {
        std::vector<Mode> group;
        for (const auto &f : yuyv ? info.yuyv_frames : info.mjpeg_frames) {
            // Some cameras list one size more than one time, so merge their rates.
            auto it = std::find_if(group.begin(), group.end(),
                                   [&](const Mode &m) { return m.width == f.width && m.height == f.height; });
            if (it == group.end()) {
                group.push_back({yuyv, f.width, f.height, {}, !fits(f)});
                it = group.end() - 1;
            }
            for (double r : frame_rates(f))
                if (std::none_of(it->fps.begin(), it->fps.end(), [&](double x) { return std::abs(x - r) < 0.01; }))
                    it->fps.push_back(r);
            std::sort(it->fps.begin(), it->fps.end(), std::greater<>());
        }
        std::stable_sort(group.begin(), group.end(), [](const Mode &a, const Mode &b) {
            return uint32_t(a.width) * a.height > uint32_t(b.width) * b.height;
        });
        out.insert(out.end(), group.begin(), group.end());
    }
    return out;
}

std::optional<Choice> pick_mode(const Info &info, const std::string &format, uint16_t width, uint16_t height,
                                double fps, std::string *error) {
    auto exact = [&](bool yuyv) -> const FrameSize * {
        for (const auto &f : yuyv ? info.yuyv_frames : info.mjpeg_frames)
            if (f.width == width && f.height == height && fits(f))
                return &f;
        return nullptr;
    };
    auto largest = [&](bool yuyv) -> const FrameSize * {
        const FrameSize *best = nullptr;
        for (const auto &f : yuyv ? info.yuyv_frames : info.mjpeg_frames)
            if (fits(f) && (!best || uint32_t(f.width) * f.height > uint32_t(best->width) * best->height))
                best = &f;
        return best;
    };
    bool yuyv = false;
    const FrameSize *f = nullptr;
    if (format == "mjpeg" || format == "yuyv") {
        yuyv = format == "yuyv";
        f = exact(yuyv);
    }
    // The auto rule: the exact size in MJPEG and then YUYV, or else the largest MJPEG and then YUYV size that fits.
    for (const bool y : {false, true})
        if (!f && (f = exact(y)))
            yuyv = y;
    for (const bool y : {false, true})
        if (!f && (f = largest(y)))
            yuyv = y;
    if (!f) {
        if (error)
            *error = "no mode at or under 640×480";
        return std::nullopt;
    }
    // Some cameras list one size more than one time with other rates, so take the one that offers the closest rate.
    if (fps > 0) {
        double best_diff = 1e18;
        for (const auto &g : yuyv ? info.yuyv_frames : info.mjpeg_frames) {
            if (g.width != f->width || g.height != f->height || !fits(g))
                continue;
            for (double r : frame_rates(g))
                if (const double diff = std::abs(r - fps); diff < best_diff) {
                    best_diff = diff;
                    f = &g;
                }
        }
    }
    Choice c;
    c.yuyv = yuyv;
    c.format_index = yuyv ? info.yuyv_format : info.mjpeg_format;
    c.frame = *f;
    c.interval = pick_interval(*f, fps);
    return c;
}

const AltSetting *pick_alt(const Info &info, uint32_t max_payload) {
    const AltSetting *best = nullptr;
    for (const auto &a : info.alts)
        if (!a.bulk && a.alt != 0 && a.packet_bytes >= max_payload && (!best || a.packet_bytes < best->packet_bytes))
            best = &a;
    return best;
}

bool yuyv_to_gray(const uint8_t *data, size_t size, uint32_t width, uint32_t height, uint8_t *gray) {
    const size_t pixels = size_t(width) * height;
    if (!pixels || size != pixels * 2)
        return false;
    for (size_t i = 0; i < pixels; i++)
        gray[i] = data[2 * i];
    return true;
}

size_t probe_length(uint16_t bcd_uvc) {
    if (bcd_uvc >= 0x0150)
        return 48;
    if (bcd_uvc >= 0x0110)
        return 34;
    return 26;
}

std::vector<uint8_t> encode_probe(const Probe &p, uint16_t bcd_uvc) {
    std::vector<uint8_t> b(probe_length(bcd_uvc), 0);
    put16(&b[0], p.hint);
    b[2] = p.format_index;
    b[3] = p.frame_index;
    put32(&b[4], p.frame_interval);
    put32(&b[18], p.max_video_frame_size);
    put32(&b[22], p.max_payload_transfer_size);
    return b;
}

std::optional<Probe> decode_probe(const uint8_t *data, size_t size) {
    if (size < 26)
        return std::nullopt;
    Probe p;
    p.hint = u16(data);
    p.format_index = data[2];
    p.frame_index = data[3];
    p.frame_interval = u32(data + 4);
    p.max_video_frame_size = u32(data + 18);
    p.max_payload_transfer_size = u32(data + 22);
    return p;
}

void FrameAssembler::reset() {
    building_.clear();
    fid_ = -1;
    bad_ = false;
}

bool FrameAssembler::finish() {
    const bool ok = !bad_ && !building_.empty();
    if (ok)
        done_.swap(building_);
    else if (!building_.empty() || bad_)
        dropped_++;
    building_.clear();
    bad_ = false;
    return ok;
}

bool FrameAssembler::push(const uint8_t *payload, size_t size) {
    if (size < 2)
        return false;
    const uint8_t header_len = payload[0], info = payload[1];
    if (header_len < 2 || header_len > size) {
        bad_ = true;   // A corrupt header makes the full frame bad.
        return false;
    }
    const int fid = info & 0x01;
    const bool eof = info & 0x02, err = info & 0x40;
    bool completed = false;
    // A changed frame ID starts a new frame, because some devices do not send EOF.
    if (fid_ >= 0 && fid != fid_ && (!building_.empty() || bad_))
        completed = finish();
    fid_ = fid;
    if (err)
        bad_ = true;
    const size_t data_len = size - header_len;
    if (building_.size() + data_len > max_bytes_) {
        bad_ = true;
        building_.clear();
    } else {
        building_.insert(building_.end(), payload + header_len, payload + size);
    }
    if (eof) {
        completed = finish() || completed;
        fid_ = -1;   // After EOF, the next payload starts a frame with any FID.
    }
    return completed;
}

bool looks_like_jpeg(const uint8_t *data, size_t size) {
    if (size < 4 || data[0] != 0xff || data[1] != 0xd8)
        return false;
    // Some cameras (the GC0308) pad the frame with bytes after EOI, so look for EOI in the last few KB.
    const size_t stop = size > 4096 ? size - 4096 : 2;
    for (size_t i = size - 1; i > stop; i--)
        if (data[i] == 0xd9 && data[i - 1] == 0xff)
            return true;
    return false;
}

}
