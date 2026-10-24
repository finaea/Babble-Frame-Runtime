#include "camera/usb/iso_reader.h"

#include <cerrno>
#include <cstring>

#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace babble::usb {

IsoReader::IsoReader(Device &dev, uint8_t endpoint, size_t packet_bytes, int packets, int count)
    : dev_(dev), endpoint_(endpoint), packet_bytes_(packet_bytes), packets_(packets) {
    const size_t urb_bytes = sizeof(usbdevfs_urb) + size_t(packets) * sizeof(usbdevfs_iso_packet_desc);
    for (int i = 0; i < count; i++) {
        urbs_.emplace_back(urb_bytes);
        buffers_.emplace_back(packet_bytes * size_t(packets));
    }
}

IsoReader::~IsoReader() { stop(); }

bool IsoReader::submit(size_t i) {
    std::memset(urbs_[i].data(), 0, urbs_[i].size());
    auto *urb = reinterpret_cast<usbdevfs_urb *>(urbs_[i].data());
    urb->type = USBDEVFS_URB_TYPE_ISO;
    urb->flags = USBDEVFS_URB_ISO_ASAP;
    urb->endpoint = endpoint_;
    urb->buffer = buffers_[i].data();
    urb->buffer_length = int(buffers_[i].size());
    urb->number_of_packets = packets_;
    urb->usercontext = reinterpret_cast<void *>(i);
    for (int p = 0; p < packets_; p++)
        urb->iso_frame_desc[p].length = unsigned(packet_bytes_);
    if (ioctl(dev_.fd(), USBDEVFS_SUBMITURB, urb) != 0)
        return false;
    in_flight_++;
    return true;
}

bool IsoReader::start(std::string *error) {
    for (size_t i = 0; i < urbs_.size(); i++) {
        if (!submit(i)) {
            if (error)
                *error = std::string("submit isochronous transfer: ") + strerror(errno);
            stop();
            return false;
        }
    }
    return true;
}

bool IsoReader::wait(int timeout_ms, const std::function<void(const uint8_t *, size_t, bool)> &on_packet,
                     std::string *error) {
    // usbfs shows completed transfers as POLLOUT.
    pollfd p{dev_.fd(), POLLOUT, 0};
    const int r = poll(&p, 1, timeout_ms);
    if (r < 0 && errno != EINTR) {
        if (error)
            *error = std::string("poll: ") + strerror(errno);
        return false;
    }
    if (p.revents & (POLLERR | POLLHUP)) {
        if (error)
            *error = "device disconnected";
        return false;
    }
    for (;;) {
        usbdevfs_urb *urb = nullptr;
        if (ioctl(dev_.fd(), USBDEVFS_REAPURBNDELAY, &urb) != 0) {
            if (errno == EAGAIN)
                return true;
            if (error)
                *error = errno == ENODEV ? std::string("device disconnected") : std::string("reap: ") + strerror(errno);
            return false;
        }
        in_flight_--;
        if (urb->status == -ENODEV || urb->status == -ESHUTDOWN) {
            if (error)
                *error = "device disconnected";
            return false;
        }
        // The packets stay at their own offsets in the buffer, and a packet with no data is normal.
        const auto *buf = static_cast<const uint8_t *>(urb->buffer);
        for (int i = 0; i < urb->number_of_packets; i++) {
            const auto &d = urb->iso_frame_desc[i];
            if (d.status != 0)
                on_packet(nullptr, 0, false);
            else if (d.actual_length > 0)
                on_packet(buf + size_t(i) * packet_bytes_, d.actual_length, true);
        }
        if (!submit(size_t(reinterpret_cast<uintptr_t>(urb->usercontext)))) {
            if (error)
                *error = errno == ENODEV ? std::string("device disconnected")
                                         : std::string("resubmit isochronous transfer: ") + strerror(errno);
            return false;
        }
    }
}

void IsoReader::stop() {
    if (!dev_.is_open()) {
        in_flight_ = 0;
        return;
    }
    for (auto &u : urbs_)
        ioctl(dev_.fd(), USBDEVFS_DISCARDURB, u.data());
    // Reap all submitted transfers, so that the kernel does not write into the buffers.
    for (int tries = 0; in_flight_ > 0 && tries < 50; tries++) {
        usbdevfs_urb *urb = nullptr;
        if (ioctl(dev_.fd(), USBDEVFS_REAPURBNDELAY, &urb) == 0) {
            in_flight_--;
            continue;
        }
        if (errno != EAGAIN)
            break;
        usleep(2000);
    }
    in_flight_ = 0;
}

}
