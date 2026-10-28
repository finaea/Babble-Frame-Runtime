#pragma once

// An ESP32 board that sends JPEGs over its USB serial port (Babble's babble_wrooms_s3_usb firmware), read from user
// space because the kernel of the Frame has no cdc_acm driver. It sends no control transfers: a change of DTR or RTS
// is how flashing tools reset an ESP32-S3.

#include "camera/jpeg_cutter.h"
#include "camera/usb/usb_device.h"
#include "camera/usb/uvc.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace babble::serial {

constexpr uint16_t kEspressif = 0x303a;
constexpr const char *kLabel = "ESP32 serial board";

// The CDC data interface and its bulk IN endpoint.
struct Port {
    uint8_t iface = 0xff, endpoint = 0;
    uint16_t max_packet = 0;
};

// Returns the first CDC data interface with a bulk IN endpoint, or nullopt if there is none or the device also has a
// UVC streaming interface (a UVC camera, such as the Babble tracker, is never a serial board).
std::optional<Port> parse_config(const uint8_t *data, size_t size);
// Returns true for an Espressif device whose configuration descriptor has a serial port and no UVC streaming.
bool is_board(const usb::Found &f, const std::vector<uint8_t> &config);
// Reads the descriptor from sysfs, so it does not open the device.
bool is_board(const usb::Found &f);

// Returns the connected UVC cameras and serial boards, in sysfs order.
std::vector<usb::Found> list_cameras();
// Returns the camera list name of a device: kLabel for a serial board, or else usb::device_label().
std::string label(const usb::Found &f, const std::vector<uint8_t> &config);

// The one mode that a board streams (240x240 MJPEG at about 30 fps), as UVC features for the camera list and the
// Resolution page. The board decides the size, and the decoder takes the size of each JPEG.
uvc::Info board_info();
// Returns the UVC features of a camera, or board_info() for a serial board.
std::optional<uvc::Info> camera_info(const usb::Found &f, const std::vector<uint8_t> &config);

// A serial board with the API of uvc::Camera, so that the USB source streams both in the same way.
class Board {
public:
    Board();
    ~Board();
    // Opens the device, claims the data interface and starts the reads. The mode is ignored.
    bool open(const usb::Found &dev, const uvc::Mode &want, std::string *error);
    // Calls on_frame for each complete JPEG and returns false if the board is gone or failed.
    bool read(int timeout_ms, const std::function<void(const uint8_t *, size_t)> &on_frame, std::string *error);
    void close();

    const uvc::Probe &negotiated() const { return probe_; }
    const uvc::FrameSize &frame_size() const { return frame_; }
    uvc::FormatKind format() const { return uvc::FormatKind::Mjpeg; }
    bool iso() const { return false; }
    uint8_t iso_alt() const { return 0; }
    uint64_t dropped() const { return cutter_.dropped(); }
    bool busy() const { return busy_; }

private:
    usb::Device dev_;
    uvc::Probe probe_;
    uvc::FrameSize frame_;
    bool busy_ = false;
    JpegCutter cutter_;
    const std::function<void(const uint8_t *, size_t)> *on_frame_ = nullptr;   // Set during read().
    std::unique_ptr<usb::StreamReader> reader_;
};

}
