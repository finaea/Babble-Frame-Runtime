#pragma once

// An MJPEG or YUYV UVC camera from user space, and one thread must call open() and then read() in a loop.

#include "camera/usb/iso_reader.h"
#include "camera/usb/usb_device.h"
#include "camera/usb/uvc.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace babble::uvc {

class Camera {
public:
    ~Camera();
    // Opens the device, negotiates the mode that pick_mode() gives for the settings, and starts the stream.
    bool open(const usb::Found &dev, const std::string &format, uint16_t width, uint16_t height, double fps,
              std::string *error);
    // Opens an MJPEG mode at the nearest frame size and rate, like before there were format settings.
    bool open(const usb::Found &dev, uint16_t width, uint16_t height, double fps, std::string *error) {
        return open(dev, "mjpeg", width, height, fps, error);
    }
    // Calls on_frame for each complete frame and returns false if the camera is gone or failed.
    bool read(int timeout_ms, const std::function<void(const uint8_t *, size_t)> &on_frame, std::string *error);
    void close();

    const Info &info() const { return info_; }
    const Probe &negotiated() const { return probe_; }
    const FrameSize &frame_size() const { return frame_; }
    bool yuyv() const { return yuyv_; }
    uint8_t iso_alt() const { return iso_alt_; }   // The isochronous alternate setting, or 0 for bulk.
    // True if the last open() failed because another program holds the camera.
    bool busy() const { return busy_; }
    uint64_t dropped() const { return assembler_.dropped(); }

private:
    usb::Device dev_;
    Info info_;
    Probe probe_;
    FrameSize frame_;
    bool yuyv_ = false, busy_ = false;
    uint8_t iso_alt_ = 0;      // The isochronous alternate setting that streams, or 0 for bulk.
    FrameAssembler assembler_;
    size_t payload_ = 16384;   // The dwMaxPayloadTransferSize, where a bulk transfer splits into payloads.
    std::unique_ptr<usb::BulkReader> reader_;
    std::unique_ptr<usb::IsoReader> iso_reader_;
};

}
