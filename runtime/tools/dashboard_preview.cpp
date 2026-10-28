// Renders the pages of the debug dashboard to PNG with synthetic readings and without SteamVR.
#include "ui/dashboard_view.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>
#pragma GCC diagnostic pop

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <tuple>

using namespace babble;
using namespace babble::ui;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: dashboard_preview OUT_DIR [FONTS_DIR]\n");
        return 2;
    }
    const std::filesystem::path out = argv[1];
    const std::filesystem::path fonts =
        argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path(BABBLE_RESOURCES_DIR) / "fonts";
    std::filesystem::create_directories(out);
    DashboardView view;
    view.load_icons(fonts.parent_path() / "icons");
    if (!view.load_fonts(fonts)) {
        fprintf(stderr, "fonts not found in %s\n", fonts.string().c_str());
        return 1;
    }
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    auto save = [&](const Canvas &img, const std::string &name) {
        std::string path = (out / (name + ".png")).string();
        stbi_write_png(path.c_str(), img.width(), img.height(), 4, img.data(), img.width() * 4);
        printf("%s\n", path.c_str());
    };

    std::vector<Choice> choices = {
        {"/sources/mouth/process_fps", "Mouth Tracking Rate", "Lower rates save battery.", "Hz",
         {30, 60, 90, 120}, 120, false, ChoicePage::Mouth},
        {"/sources/eye/rate_divisor", "Eye Tracking Rate",
         "A share of the eye camera rate. Lower rates save battery.", "", {1, 2, 3}, 1,
         false, ChoicePage::Eye, {"1:1", "1:2", "1:3"}},
        {"/pipelines/gaze/smoothing", "Gaze Smoothing",
         "Steadies the gaze. Stronger smoothing adds a little lag to fast eye movements.", "", {0, 1, 2, 3}, 0,
         false, ChoicePage::Eye, {"Off", "Low", "Medium", "High"}},
        {"/pipelines/eye/openness_source", "Openness Source",
         "Babble uses the runtime's eye model. Native uses the headset's own eye openness.", "", {0, 1}, 0, false,
         ChoicePage::EyeSources, {"Babble", "Native"}, {"model", "native"}},
        {"/pipelines/gaze/gaze_source", "Gaze Source",
         "Independent lets each eye move on its own. Combined uses the single gaze of SteamVR.", "", {0, 1}, 0, false,
         ChoicePage::EyeSources, {"Independent", "Combined"}, {"independent", "combined"}},
        {"/pipelines/eye/combine_widen", "Combine Eye Widen", "Both eyes get the average Widen of the two eyes.", "",
         {0, 1}, 1, true, ChoicePage::EyeModifiers},
        {"/pipelines/eye/combine_dilation", "Combine Pupil Dilation",
         "Both eyes get the average pupil dilation of the two eyes.", "", {0, 1}, 0, true, ChoicePage::EyeModifiers},
        {"/sinks/camera_stream/active", "Camera Stream",
         "Stream camera frames on TCP 9099 for diagnostics. Turns off after a restart.", "", {0, 1}, 1, true, ChoicePage::Diagnostics},
    };
    auto feed = [](Health h, double hz = 0, std::string detail = "", bool tracking = true) {
        FeedStatus f;
        f.health = h;
        f.hz = hz;
        f.detail = std::move(detail);
        f.available = true;
        f.tracking = tracking;
        return f;
    };
    Status tracking{true, Worn::Yes, feed(Health::Working, 119.8), feed(Health::Working, 90.1)};
    tracking.visualizer = AppState::Stopped;
    // This eye looks left and up, and is wide open.
    tracking.eyes[0] = {true, true, -18, 6, 1.0, 0.6, true, 0.843, true, 0.742, 0.941, true, 0.62};
    // This eye is half closed and the calibration is not complete.
    tracking.eyes[1] = {true, true, -21, 5, 0.35, 0, true, 0.251, false, 0, 0, false, 0};
    tracking.link = {true, true, "lan", "Demo PC", "192.0.2.10", 3.6};
    // The output modules as main.cpp fills them from the module blocks in defaults.json.
    const ModuleOption native_target{"/sinks/vrc_native/target", "VRChat On", false, false,
                                     {"\"auto\"", "\"pc\"", "\"frame\""}, {"Auto", "PC", "Frame"}, "\"auto\""};
    const ModuleOption osc_target{"/sinks/babble_osc/target", "Send To", false, false,
                                  {"\"auto\"", "\"pc\"", "\"frame\""}, {"Auto", "PC", "Frame"}, "\"auto\""};
    const ModuleOption osc_port{"/sinks/babble_osc/port", "Port", true, true, {}, {}, "8888"};
    tracking.outputs = {
        {"vrcft", "VRCFaceTracking", "The Babble module in VRCFaceTracking on your PC, found on the network by itself.",
         true, true, "Linked to Demo PC via LAN", {}},
        {"vrc_native", "VRChat Native", "Straight into VRChat's own face tracking, without VRCFaceTracking.", false,
         false, "", {native_target}},
        {"babble_osc", "Babble OSC",
         "Baballonia-format OSC to the PC address, the PC you stream from, or this headset. Allow its UDP port in "
         "Windows Firewall.",
         false, false, "", {osc_target, osc_port}}};
    Status paused{true, Worn::No, feed(Health::Paused), feed(Health::Paused)};
    Status fault{true, Worn::Yes, feed(Health::Working, 120),
                 feed(Health::Fault, 0, "cannot connect to dsp_service (is the eye tracking service running?)")};
    Status starting{true, Worn::Yes, feed(Health::Starting), feed(Health::Starting)};
    starting.visualizer = AppState::Stopped;
    starting.link = {true, false, "", "", "", -1};   // The link searches for the PC.
    Status no_vr{false, Worn::Unknown, feed(Health::Starting), feed(Health::Off)};
    Status eyes_off{true, Worn::Yes, feed(Health::Working, 120), feed(Health::Off, 0, "", false)};
    Status all_off{true, Worn::Yes, feed(Health::Off, 0, "", false), feed(Health::Off, 0, "", false)};

    view.render(c, tracking, choices, std::nullopt);
    save(c, "home_tracking");
    // Show VRChat Native on the PC that Steam Link streams from, and on this headset with no data yet.
    Status native_pc = tracking;
    native_pc.link = {true, true, "pc", "", "192.0.2.10", -1, true};
    view.render(c, native_pc, choices, std::nullopt);
    save(c, "home_native_pc");
    Status native_frame = tracking;
    native_frame.link = {true, false, "frame", "", "127.0.0.1", -1, true};
    view.render(c, native_frame, choices, std::nullopt);
    save(c, "home_native_frame");
    // Show Babble OSC as the first output that is on, and VRCFaceTracking with another output on.
    Status osc_home = tracking;
    osc_home.link = {true, true, "", "", "192.0.2.10:8888", -1};
    osc_home.link.osc = true;
    view.render(c, osc_home, choices, std::nullopt);
    save(c, "home_osc");
    Status more = tracking;
    more.link.more = 1;
    view.render(c, more, choices, std::nullopt);
    save(c, "home_more");
    view.render(c, tracking, choices, Point{84, DashboardView::kHeight - 84.f});
    save(c, "home_cog_hover");
    view.render(c, paused, choices, std::nullopt);
    save(c, "home_paused");
    view.render(c, fault, choices, std::nullopt);
    save(c, "home_fault");
    view.render(c, starting, choices, std::nullopt);
    save(c, "home_starting");
    view.render(c, no_vr, choices, std::nullopt);
    save(c, "home_no_steamvr");
    view.render(c, eyes_off, choices, Point{1080, 536});   // The pointer is on the eyes switch.
    save(c, "home_eyes_off");
    view.render(c, all_off, choices, std::nullopt);
    save(c, "home_all_off");
    {   // Show the runtime update pill next to the version.
        Status u = tracking;
        u.update = {UpdateNotice::State::Available, "V1.0 Alpha 5", "", 0};
        view.render(c, u, choices, std::nullopt);
        save(c, "home_update_available");
        u.update = {UpdateNotice::State::Downloading, "V1.0 Alpha 5", "", 0.42f};
        view.render(c, u, choices, std::nullopt);
        save(c, "home_update_downloading");
        u.update = {UpdateNotice::State::RestartNeeded, "V1.0 Alpha 5", "", 0};
        view.render(c, u, choices, std::nullopt);
        save(c, "home_update_restart");
    }

    view.set_page(DashboardView::Page::Settings);
    choices[0].value = 60;
    view.render(c, tracking, choices, Point{1000, 222});
    save(c, "settings");
    view.set_page(DashboardView::Page::Eye);
    view.render(c, tracking, choices, std::nullopt);
    save(c, "eye");
    view.set_page(DashboardView::Page::EyeSources);
    view.render(c, tracking, choices, std::nullopt);
    save(c, "eye_sources");
    view.set_page(DashboardView::Page::EyeModifiers);
    view.render(c, tracking, choices, std::nullopt);
    save(c, "eye_modifiers");
    view.set_page(DashboardView::Page::Mouth);
    view.render(c, tracking, choices, std::nullopt);
    save(c, "mouth");

    // Show the camera row for a USB mouth source, and then its page with two cameras and an active tracker.
    Status usb = tracking;
    usb.mouth_source = "usb";
    // A bulk MJPEG tracker, and an isochronous camera with MJPEG and YUY2, as the 1e45:0209 camera has them.
    UsbCamera tracker{"303a:8000:DEMO01", "Babble USB Tracker", "303a:8000 \u00b7 demo tracker"};
    tracker.formats = {{"mjpeg", "MJPEG", {{240, 240, {60}}}}};
    tracker.settings = {"mjpeg", 240, 240, 60, 0, true, false};
    UsbCamera webcam{"1e45:0209@1-1", "Generic HD Camera", "1e45:0209", true};
    const std::vector<int> mjpeg_fps{120, 60, 30, 25, 20};
    webcam.formats = {{"mjpeg", "MJPEG",
                       {{640, 480, mjpeg_fps}, {352, 288, mjpeg_fps}, {320, 240, mjpeg_fps}, {160, 120, mjpeg_fps}}},
                      {"yuy2", "YUY2", {{640, 480, {30}}, {352, 288, {30}}, {320, 240, {30}}, {160, 120, {30}}}}};
    webcam.settings = {"mjpeg", 640, 480, 60, 90, false, false};
    usb.cameras = {tracker, webcam};
    usb.camera = "303a:8000:DEMO01";
    usb.active_camera = usb.camera;
    view.render(c, usb, choices, std::nullopt);
    save(c, "mouth_usb");
    // In release builds, the USB tracker is the only mouth source, so there is no Frunk/USB row and no mouth rate.
    Status release = usb;
    release.mouth_source_selectable = false;
    view.render(c, release, {choices[1], choices[2]}, std::nullopt);
    save(c, "mouth_release");
    view.set_page(DashboardView::Page::Settings);
    view.render(c, release, {choices[1], choices[2]}, std::nullopt);
    save(c, "settings_release");
    view.set_page(DashboardView::Page::Cameras);
    view.render(c, usb, choices, Point{600, 340});   // The pointer is on the second camera.
    save(c, "cameras");
    view.set_options_camera(webcam.id);
    view.set_page(DashboardView::Page::CameraOptions);
    // A 90 degree turn makes the 640x480 image 225x300 in the preview box. The demo image is a soft gradient.
    CameraPreview pv{webcam.id, 225, 300, std::vector<uint8_t>(225 * 300), 1};
    for (int y = 0; y < 300; y++)
        for (int x = 0; x < 225; x++)
            pv.gray[size_t(y) * 225 + x] = uint8_t(40 + (x + y) * 160 / 525);
    Status live = usb;
    live.active_camera = webcam.id;
    live.preview = &pv;
    view.render(c, live, choices, std::nullopt);
    save(c, "camera_options");
    view.render(c, usb, choices, std::nullopt);
    save(c, "camera_options_inactive");
    view.set_page(DashboardView::Page::CameraResolution);
    view.render(c, usb, choices, std::nullopt);
    save(c, "camera_resolution");
    view.set_options_camera(tracker.id);
    view.set_page(DashboardView::Page::CameraOptions);
    view.render(c, usb, choices, std::nullopt);
    save(c, "camera_options_tracker");
    view.set_page(DashboardView::Page::Cameras);
    Status none = usb;
    none.cameras.clear();
    none.camera.clear();
    none.active_camera.clear();
    view.render(c, none, choices, std::nullopt);
    save(c, "cameras_none");
    Status missing = usb;
    missing.cameras.clear();
    missing.active_camera.clear();
    view.render(c, missing, choices, std::nullopt);
    save(c, "cameras_missing");
    Status diag = tracking;
    diag.verbose_minutes_left = 7;
    diag.last_diagnostics = "diagnostics-demo.txt";
    diag.log_file = "/home/steamos/.local/state/babble-frame-runtime/runtime.log";
    diag.stats_file = "/home/steamos/.local/state/babble-frame-runtime/stats.jsonl";
    view.set_page(DashboardView::Page::Diagnostics);
    view.render(c, diag, choices, std::nullopt);
    save(c, "diagnostics");
    // Show the dataset capture row in a few states.
    for (const auto &[name, st] : {std::pair<const char *, CaptureRow::State>{"not_installed", CaptureRow::State::NotInstalled},
                                   {"downloading", CaptureRow::State::Downloading},
                                   {"installed", CaptureRow::State::Installed},
                                   {"update", CaptureRow::State::UpdateAvailable},
                                   {"running", CaptureRow::State::Running}}) {
        Status d = diag;
        d.capture.state = st;
        d.capture.version = "0.1.0";
        d.capture.latest = "0.2.0";
        d.capture.progress = 0.42f;
        d.capture.recording = st == CaptureRow::State::Running;
        d.capture.uploads_pending = st == CaptureRow::State::Running ? 2 : 0;
        d.capture.upload_progress = 0.4f;
        d.capture.upload_stage = "uploading";
        view.render(c, d, choices, std::nullopt);
        save(c, std::string("diagnostics_capture_") + name);
    }
    diag.verbose_minutes_left = 0;
    diag.last_diagnostics.clear();
    for (Choice &ch : choices)   // Turn the camera stream off.
        if (ch.id == "/sinks/camera_stream/active")
            ch.value = 0;
    view.render(c, diag, choices, Point{1100, 222});
    save(c, "diagnostics_off");
    view.set_page(DashboardView::Page::Settings);
    view.render(c, tracking, choices, std::nullopt);
    view.click({1100, 666});
    view.render(c, tracking, choices, std::nullopt);
    save(c, "settings_restart_confirmation");

    // Show the Eyes, Squint calibration and Mouth pages with a demo config, demo signals and a demo face frame.
    {
        auto cfg = std::make_shared<nlohmann::json>(nlohmann::json::parse(R"({
            "sources": {"face": {"mode": "url", "url": "http://127.0.0.1:8082/"}},
            "pipelines": {
                "eye": {"ranges": {"LeftEyeLid": [0.1, 0.9], "RightEyeLid": [0, 0.85], "LeftEyeWiden": [0.2, 1],
                                   "RightEyeWiden": [0, 1], "LeftEyeDilation": [0, 1], "RightEyeDilation": [0.3, 0.7]}},
                "gaze": {"ranges": {"LeftEyeX": [-0.8, 0.8], "RightEyeX": [-1, 1], "LeftEyeY": [-0.6, 1], "RightEyeY": [-1, 1]}},
                "face": {"roi": [40, 30, 160, 160], "rotate": 0, "flip": "none",
                         "calibration": {"mode": "manual", "ranges": {"jawOpen": [0.05, 0.8], "mouthSmileLeft": [0.1, 0.7]}}}},
            "sinks": {"squint": {"ranges": {"LeftEyeSquint": [0.15, 0.9], "RightEyeSquint": [0, 1]}}}
        })"));
        std::map<std::string, LiveSignal> live;
        for (const auto &[name, v] : std::initializer_list<std::pair<const char *, float>>{
                 {"_LeftEyeLidPre", 0.62f}, {"_RightEyeLidPre", 0.58f}, {"_LeftEyeWidenPre", 0.12f},
                 {"_RightEyeWidenPre", 0.05f}, {"_LeftEyeSquintPre", 0.4f}, {"_RightEyeSquintPre", 0.35f},
                 {"_LeftEyeDilationPre", 0.5f}, {"_RightEyeDilationPre", 0.55f}, {"_LeftEyeXPre", -0.3f},
                 {"_RightEyeXPre", -0.25f}, {"_LeftEyeYPre", 0.1f}, {"_RightEyeYPre", 0.12f},
                 {"_jawOpenPre", 0.3f}, {"_jawLeftPre", 0.02f}, {"_mouthSmileLeftPre", 0.45f}})
            live[name] = {v, true};
        auto frame = std::make_shared<PreviewImage>();
        frame->width = frame->height = 240;
        frame->seq = 1;
        frame->pixels.resize(240 * 240);
        for (int y = 0; y < 240; y++)   // A soft gradient with a dark mouth shape.
            for (int x = 0; x < 240; x++) {
                const float dx = (x - 120) / 50.f, dy = (y - 140) / 22.f;
                frame->pixels[size_t(y) * 240 + x] = uint8_t(dx * dx + dy * dy < 1 ? 40 : 90 + x / 3 + y / 4);
            }
        auto data = [&](std::map<std::string, LiveSignal> extra, bool preview = false) {
            PageData d{cfg, live, preview ? frame : nullptr};
            for (auto &[k, v] : extra)
                d.signals[k] = v;
            return d;
        };
        std::vector<Choice> fork_choices = choices;
        fork_choices.push_back({"/pipelines/eye/blink_hold_ms", "Blink Hold",
                                "Keeps a blink closed long enough for other players to see it. Your mirror doesn't need it.",
                                "ms", {0, 50, 100, 150}, 0, false, ChoicePage::Eyelids, {"Off", "50", "100", "150"}});
        fork_choices.push_back({"/sinks/squint/active", "Squint",
                                "Squint from eyecam's engine on the eye cameras. Needs the squint calibration once.", "",
                                {0, 1}, 1, true, ChoicePage::Squint});

        view.set_page(DashboardView::Page::Eye);
        view.render(c, release, fork_choices, std::nullopt);
        save(c, "eye_fork_pages");

        view.set_page(DashboardView::Page::Eyes);
        view.set_page_data(data({}));
        view.render(c, tracking, fork_choices, Point{500, 260});   // The pointer is on the left openness range.
        save(c, "eyes_eyelids");
        view.click({1060, 76});   // The Gaze tab.
        view.render(c, tracking, fork_choices, std::nullopt);
        save(c, "eyes_gaze");

        view.set_page(DashboardView::Page::Squint);
        using LS = std::map<std::string, LiveSignal>;
        const std::pair<const char *, LS> squint_states[] = {
            {"squint_no_engine", {}},
            {"squint_learning", {{"_SquintCalState", {0, true}}}},
            {"squint_imported", {{"_SquintCalState", {0, true}}, {"_SquintReady", {1, true}},
                                 {"_SquintCalibrated", {1, true}}, {"_SquintCalImported", {1, true}}}},
            {"squint_running", {{"_SquintCalState", {1, true}}, {"_SquintCalStep", {4, true}},
                                {"_SquintCalSecondsLeft", {3.4f, true}}}},
            {"squint_fitting", {{"_SquintCalState", {2, true}}}},
            {"squint_done", {{"_SquintCalState", {3, true}}, {"_SquintReady", {1, true}}, {"_SquintCalibrated", {1, true}},
                             {"_LeftEyeSquintFsq", {-0.31f, true}}, {"_RightEyeSquintFsq", {-0.27f, true}}}},
            {"squint_failed", {{"_SquintCalState", {4, true}}, {"_SquintReady", {1, true}}, {"_SquintCalFail", {2, true}}}},
        };
        for (const auto &[name, sig] : squint_states) {
            view.set_page_data(data(sig));
            view.render(c, tracking, fork_choices, std::nullopt);
            save(c, name);
        }

        view.set_page(DashboardView::Page::Mouth);
        view.set_page_data(data({}, true));
        view.render(c, tracking, fork_choices, std::nullopt);
        save(c, "mouth_camera");
        // Drag the crop box 30 px right and its corner 40 px out, and show the box before the release.
        view.click({200, 280});
        view.drag({230, 280});
        view.render(c, tracking, fork_choices, std::nullopt);
        save(c, "mouth_camera_dragging");
        const auto moved = view.release({230, 280});
        for (const auto &[ptr, v] : moved.changes)
            printf("drag sends %s = %s\n", ptr.c_str(), v.dump().c_str());
        view.set_page_data(data({}));
        view.render(c, tracking, fork_choices, std::nullopt);
        save(c, "mouth_camera_waiting");
        view.click({1060, 76});   // The Shapes tab.
        view.render(c, tracking, fork_choices, std::nullopt);
        save(c, "mouth_shapes_jaw");
        view.click({340 + 160 + 80, 288});   // The Mouth group.
        view.render(c, tracking, fork_choices, std::nullopt);
        save(c, "mouth_shapes_mouth");
        view.click({1100, 682});   // Next.
        view.render(c, tracking, fork_choices, std::nullopt);
        save(c, "mouth_shapes_mouth_page2");
        (*cfg)["pipelines"]["face"]["calibration"]["mode"] = "auto";
        view.click({340 + 4 * 160 + 80, 288});   // The Tongue group.
        view.set_page_data(data({}));
        view.render(c, tracking, fork_choices, std::nullopt);
        save(c, "mouth_shapes_auto");

        // The PC app mode: Baballonia on the PC sends the mouth, so there is no preview.
        (*cfg)["sources"]["face"]["mode"] = "pc";
        view.click({920, 76});   // The Camera tab.
        view.set_page_data(data({}, true));
        view.render(c, tracking, fork_choices, std::nullopt);
        save(c, "mouth_camera_pc_app");
        view.set_page(DashboardView::Page::Home);
        view.render(c, release, choices, std::nullopt);
        save(c, "home_pc_app");
    }

    // The USB mouth camera: a GC0308-like camera at 640x480, its states, the free crop and the Resolution page.
    {
        auto cfg = std::make_shared<nlohmann::json>(nlohmann::json::parse(R"({
            "sources": {"face": {"mode": "usb", "camera": "", "width": 240, "height": 240, "fps": 30,
                                 "cameras": {"0c45:6366:1-1": {"format": "mjpeg", "width": 640, "height": 480, "fps": 30}}}},
            "pipelines": {"face": {"roi": [150, 120, 345, 301], "roi_square": false, "rotate": 0, "flip": "none"}}
        })"));
        auto frame = std::make_shared<PreviewImage>();
        frame->width = 640;
        frame->height = 480;
        frame->seq = 1;
        frame->pixels.resize(640 * 480);
        for (int y = 0; y < 480; y++)   // A soft gradient with a dark mouth shape.
            for (int x = 0; x < 640; x++) {
                const float dx = (x - 320) / 110.f, dy = (y - 290) / 45.f;
                frame->pixels[size_t(y) * 640 + x] = uint8_t(dx * dx + dy * dy < 1 ? 40 : 80 + x / 8 + y / 6);
            }
        UsbCamera gc0308{"0c45:6366:1-1", "USB 2.0 Camera", "0c45:6366", true};
        gc0308.formats = {{"mjpeg", "MJPEG", {{640, 480, {30}}, {320, 240, {120}}, {160, 120, {30, 15, 10, 5}}}},
                          {"yuy2", "YUY2", {{640, 480, {30}}, {320, 240, {30}}, {160, 120, {30}}}}};
        gc0308.settings = {"mjpeg", 640, 480, 30};
        Status u = tracking;
        u.mouth_source = "usb";
        u.cameras = {gc0308};
        u.active_camera = gc0308.id;
        u.usb_stream = {UsbStream::State::Streaming, gc0308.id, "mjpeg", 640, 480, 30, 25, ""};
        // A new view for each shot that needs the config as it is, without the values that a view sent.
        std::unique_ptr<DashboardView> v;
        auto fresh = [&] {
            v = std::make_unique<DashboardView>();
            v->load_icons(fonts.parent_path() / "icons");
            v->load_fonts(fonts);
            v->set_page(DashboardView::Page::Mouth);
        };
        auto set_data = [&](bool preview) { v->set_page_data({cfg, {}, preview ? frame : nullptr}); };
        fresh();
        set_data(true);
        v->render(c, u, choices, std::nullopt);
        save(c, "mouth_usb_wide");
        // Drag the top left corner up and left; the bottom right corner stays.
        v->click({150, 266});
        v->drag({110, 236});
        v->render(c, u, choices, std::nullopt);
        save(c, "mouth_usb_corner_drag");
        for (const auto &[ptr, v] : v->release({110, 236}).changes)
            printf("corner drag sends %s = %s\n", ptr.c_str(), v.dump().c_str());
        // Turn on the square lock, which squares the box around its centre.
        set_data(true);
        v->render(c, u, choices, std::nullopt);
        for (float x = 600; x < 1000; x += 8)
            if (const auto r = v->click({x, 682}); !r.changes.empty()) {
                for (const auto &[ptr, v] : r.changes)
                    printf("square lock sends %s = %s\n", ptr.c_str(), v.dump().c_str());
                break;
            }
        v->render(c, u, choices, std::nullopt);
        save(c, "mouth_usb_square");
        // The wide box turned 90 degrees is taller than wide for the model.
        (*cfg)["pipelines"]["face"]["roi_square"] = false;
        (*cfg)["pipelines"]["face"]["rotate"] = 90;
        fresh();
        set_data(true);
        v->render(c, u, choices, std::nullopt);
        save(c, "mouth_usb_rotated");
        (*cfg)["pipelines"]["face"]["rotate"] = 0;
        for (const auto &[name, st, err] : {std::tuple<const char *, UsbStream::State, const char *>{
                                                "mouth_usb_busy", UsbStream::State::Busy, "Device or resource busy"},
                                            {"mouth_usb_error", UsbStream::State::Error,
                                             "not enough USB bandwidth for this mode, pick a smaller one"},
                                            {"mouth_usb_waiting", UsbStream::State::Waiting, ""},
                                            {"mouth_usb_opening", UsbStream::State::Opening, ""}}) {
            Status b = u;
            b.usb_stream = {st, "", "", 0, 0, 0, 0, err};
            v->render(c, b, choices, std::nullopt);
            save(c, name);
        }
        // The USB source reports Waiting while it is stopped; the feed status says whether tracking is off or paused.
        for (const auto &[name, tracking, health] : {std::tuple<const char *, bool, Health>{"mouth_usb_tracking_off", false,
                                                                                             Health::Off},
                                                     {"mouth_usb_paused", true, Health::Paused}}) {
            Status b = u;
            b.usb_stream = {UsbStream::State::Waiting, "", "", 0, 0, 0, 0, ""};
            b.mouth.available = true;
            b.mouth.tracking = tracking;
            b.mouth.health = health;
            v->render(c, b, choices, std::nullopt);
            save(c, name);
        }
        // The camera page from the Mouth page, and Back.
        v->render(c, u, choices, std::nullopt);
        v->click({800, 316});
        v->render(c, u, choices, std::nullopt);
        save(c, "mouth_usb_cameras");
        v->click({60, 70});
        printf("back from the camera page: %s\n", v->page() == DashboardView::Page::Mouth ? "Mouth" : "other");
        // The Resolution page, and a pick of 320x240 that scales the crop box.
        v->render(c, u, choices, std::nullopt);
        v->click({800, 378});
        set_data(false);
        v->render(c, u, choices, Point{1050, 286});
        save(c, "resolution_gc0308");
        for (const auto &[ptr, v] : v->click({1090, 286}).changes)
            printf("320x240 pick sends %s = %s\n", ptr.c_str(), v.dump().c_str());
        v->render(c, u, choices, std::nullopt);
        save(c, "resolution_gc0308_picked");
        // A webcam with HD modes, on two pages.
        Status hd = u;
        UsbCamera webcam{"046d:0825:DEMO", "HD Webcam", "046d:0825"};
        webcam.formats = {{"mjpeg", "MJPEG", {{1920, 1080, {30}}, {1280, 720, {30}}, {640, 480, {30, 25, 20, 15, 10, 5}},
                                              {320, 240, {60, 50, 30, 25, 24, 20, 15, 12, 10, 5, 1}}}},
                          {"yuy2", "YUY2", {{1280, 720, {10}}, {640, 480, {30}}, {320, 240, {30}}, {176, 144, {30}},
                                            {160, 120, {30}}}}};
        webcam.settings = {"mjpeg", 640, 480, 30};
        hd.cameras = {webcam};
        hd.usb_stream.camera = "046d:0825:DEMO";
        v->render(c, hd, choices, std::nullopt);
        save(c, "resolution_webcam");
        v->click({1100, 682});   // Next.
        v->render(c, hd, choices, std::nullopt);
        save(c, "resolution_webcam_page2");
        Status nocam = u;
        nocam.cameras.clear();
        v->render(c, nocam, choices, std::nullopt);
        save(c, "resolution_no_camera");
        v->click({60, 70});
        printf("back from the Resolution page: %s\n", v->page() == DashboardView::Page::Mouth ? "Mouth" : "other");
    }
    // Show the Outputs page with the PC address found from Steam Link, typed in, and rejected.
    view.set_page(DashboardView::Page::Outputs);
    Status outs = tracking;
    view.render(c, outs, choices, std::nullopt);
    save(c, "outputs");
    outs.pc_address = "192.0.2.10";
    outs.outputs[1].on = true;
    outs.outputs[1].ok = true;
    outs.outputs[1].status = "Sending to VRChat at 192.0.2.10";
    outs.outputs[2].on = true;
    outs.outputs[2].ok = true;
    outs.outputs[2].status = "Sending to 192.0.2.10:8888";
    view.render(c, outs, choices, std::nullopt);
    save(c, "outputs_address");
    outs.output_error = "Port: abc is not a number";
    view.render(c, outs, choices, std::nullopt);
    save(c, "outputs_error");
    // Without an address or a Steam Link session, the outputs that are on wait.
    Status waiting = tracking;
    waiting.outputs[0].ok = false;
    waiting.outputs[0].status = "Searching for VRCFaceTracking\u2026";
    waiting.outputs[1].on = true;
    waiting.outputs[1].status = "Waiting for a Steam Link session";
    waiting.outputs[1].options[0].value = "\"pc\"";
    waiting.outputs[2].on = true;
    waiting.outputs[2].status = "Set a PC address, or stream with Steam Link";
    view.render(c, waiting, choices, std::nullopt);
    save(c, "outputs_waiting");
    // VRChat Native on this headset, with the other outputs off.
    Status frame_only = tracking;
    frame_only.outputs[0].on = false;
    frame_only.outputs[0].ok = false;
    frame_only.outputs[0].status.clear();
    frame_only.outputs[1].on = true;
    frame_only.outputs[1].ok = true;
    frame_only.outputs[1].status = "Sending to VRChat on this headset";
    frame_only.outputs[1].options[0].value = "\"frame\"";
    view.render(c, frame_only, choices, std::nullopt);
    save(c, "outputs_frame");

    // Measure the render cost for each redraw on the dashboard thread.
    {
        view.set_page(DashboardView::Page::Home);
        const int n = 50;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++)
            view.render(c, tracking, choices, Point{540, 536});
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        printf("home page render: %.2f ms\n", ms);
        view.set_page(DashboardView::Page::Settings);
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++)
            view.render(c, tracking, choices, Point{1000, 222});
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        printf("settings page render: %.2f ms\n", ms);
    }

    Canvas thumb(DashboardView::kThumb, DashboardView::kThumb);
    view.render_thumbnail(thumb);
    save(thumb, "thumbnail");
    return 0;
}
