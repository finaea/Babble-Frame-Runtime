#include "camera/face_camera_source.h"

#include "camera/url/mjpeg_url_source.h"
#include "camera/usb/babble_usb_source.h"
#include "core/log.h"

namespace {
const babble::log::Logger LOG("source");

bool valid_mode(const nlohmann::json &mode) { return mode == "usb" || mode == "url" || mode == "pc"; }

// The PC app mode: an app on the PC (Baballonia) sends the mouth, so no camera opens and no frames come. Tracking only
// says whether the mouth is on, so the PC link still tells the VRCFT module to claim face tracking.
class PcAppSource : public babble::ITrackingSource {
public:
    explicit PcAppSource(bool tracking) : tracking_(tracking) {}
    void tick(uint64_t) override {}
    void shutdown() override {}
    void set_process_fps(double) override {}
    void set_tracking(bool on) override { tracking_ = on; }
    bool tracking() const override { return tracking_; }
    bool streaming() const override { return false; }
    babble::SourceActivity activity() const override { return babble::SourceActivity::Off; }
    const char *state_name() const override { return "PC app"; }
    const std::string &last_error() const override { return error_; }

private:
    bool tracking_;
    std::string error_;
};
}

namespace babble {

FaceCameraSource::FaceCameraSource(const nlohmann::json &cfg, SourceDeps deps) : cfg_(cfg), deps_(std::move(deps)) {
    if (!valid_mode(cfg_.value("mode", "usb")))
        cfg_["mode"] = "usb";
    make_inner();
}

FaceCameraSource::~FaceCameraSource() {
    if (inner_)
        inner_->set_frame_tap(nullptr);
}

void FaceCameraSource::make_inner() {
    if (inner_) {
        inner_->set_frame_tap(nullptr);
        inner_->shutdown();
        inner_.reset();   // This also removes its stats, so the new source can add them again.
    }
    if (cfg_["mode"] == "pc") {
        inner_ = std::make_unique<PcAppSource>(cfg_.value("tracking", true));
        LOG.info("%s: mouth from the PC app, no camera", deps_.name.c_str());
        return;
    }
    if (cfg_["mode"] == "url")
        inner_ = std::make_unique<MjpegUrlSource>(cfg_, deps_);
    else
        inner_ = std::make_unique<BabbleUsbSource>(cfg_, deps_);
    inner_->set_frame_tap(tap_);
    LOG.info("%s: mouth camera from %s", deps_.name.c_str(),
             cfg_["mode"] == "url" ? ("stream " + cfg_.value("url", std::string())).c_str() : "USB");
}

BabbleUsbSource *FaceCameraSource::usb() const { return dynamic_cast<BabbleUsbSource *>(inner_.get()); }

void FaceCameraSource::set_process_fps(double fps) {
    cfg_["process_fps"] = fps;
    inner_->set_process_fps(fps);
}

void FaceCameraSource::set_tracking(bool on) {
    cfg_["tracking"] = on;
    inner_->set_tracking(on);
}

void FaceCameraSource::set_frame_tap(IFrameTap *tap) {
    tap_ = tap;
    inner_->set_frame_tap(tap);
}

bool FaceCameraSource::set_option(const std::string &key, const nlohmann::json &value) {
    if (key == "mode") {
        if (!valid_mode(value))
            return false;
        if (value != cfg_["mode"]) {
            cfg_["mode"] = value;
            make_inner();
        }
        return true;
    }
    if (key == "url") {
        if (!value.is_string())
            return false;
        if (value.get<std::string>() != cfg_.value("url", std::string())) {
            cfg_["url"] = value;
            if (cfg_["mode"] == "url")
                make_inner();
        }
        return true;
    }
    // The USB options also apply after a later change back to the USB mode.
    if (key == "camera" && value.is_string()) {
        cfg_["camera"] = value;
        if (auto *u = usb())
            u->set_option(key, value);
        return true;
    }
    return inner_->set_option(key, value);
}

}
