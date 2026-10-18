#pragma once

#include "camera/tracking_source.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace babble {

class BabbleUsbSource;

// The mouth camera, which is either the USB camera (mode "usb") or an MJPEG stream over HTTP (mode "url").
// It owns one of the two sources and changes between them live, and the URL mode never opens USB.
class FaceCameraSource : public ITrackingSource {
public:
    FaceCameraSource(const nlohmann::json &cfg, SourceDeps deps);
    ~FaceCameraSource() override;

    void tick(uint64_t now_ns) override { inner_->tick(now_ns); }
    void shutdown() override { inner_->shutdown(); }
    void set_process_fps(double fps) override;
    void set_tracking(bool on) override;
    bool tracking() const override { return inner_->tracking(); }
    bool set_option(const std::string &key, const nlohmann::json &value) override;
    void set_frame_tap(IFrameTap *tap) override;
    double camera_fps() const override { return inner_->camera_fps(); }
    bool streaming() const override { return inner_->streaming(); }
    SourceActivity activity() const override { return inner_->activity(); }
    const char *state_name() const override { return inner_->state_name(); }
    const std::string &last_error() const override { return inner_->last_error(); }

    // Returns the USB source in the USB mode, or null in the URL mode.
    BabbleUsbSource *usb() const;

private:
    void make_inner();

    nlohmann::json cfg_;   // This holds the current mode, URL, rate and tracking for the next inner source.
    SourceDeps deps_;
    IFrameTap *tap_ = nullptr;
    std::unique_ptr<ITrackingSource> inner_;
};

}
