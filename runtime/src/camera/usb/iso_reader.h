#pragma once

// Isochronous streaming through usbfs, for UVC cameras that have no bulk endpoint.

#include "camera/usb/usb_device.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace babble::usb {

// Keeps several isochronous IN transfers in the queue of one endpoint, where each packet is one UVC payload.
class IsoReader {
public:
    IsoReader(Device &dev, uint8_t endpoint, size_t packet_bytes, int packets = 32, int count = 5);
    ~IsoReader();
    bool start(std::string *error);
    // Sends each received packet to on_packet, with ok false for a packet that was lost, and returns false if the
    // device is gone or failed.
    bool wait(int timeout_ms, const std::function<void(const uint8_t *, size_t, bool ok)> &on_packet,
              std::string *error);
    void stop();

private:
    bool submit(size_t i);
    Device &dev_;
    uint8_t endpoint_;
    size_t packet_bytes_;
    int packets_;
    // Each URB is a usbdevfs_urb and its packet descriptors in one block.
    std::vector<std::vector<uint8_t>> urbs_;
    std::vector<std::vector<uint8_t>> buffers_;
    int in_flight_ = 0;
};

}
