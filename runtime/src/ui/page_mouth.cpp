// The Mouth page: the camera, a live preview with the crop box, and the manual ranges of the 45 shapes.
#include "ui/dashboard_view.h"

#include "model/babble_mono_face.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace babble::ui {

using namespace style;
using nlohmann::json;

namespace {

// The shape groups of Baballonia, which are also the name prefixes.
constexpr const char *kGroups[] = {"jaw", "mouth", "cheek", "nose", "tongue"};
constexpr const char *kGroupLabels[] = {"Jaw", "Mouth", "Cheek", "Nose", "Tongue"};
constexpr int kPerPage = 8;

std::vector<std::string> shapes_in(int group) {
    std::vector<std::string> out;
    for (const char *s : face::kShapes)
        if (std::string_view(s).starts_with(kGroups[group]))
            out.emplace_back(s);
    return out;
}

// Changes "mouthSmileLeft" to "Smile Left".
std::string shape_label(const std::string &name, int group) {
    std::string out;
    for (char ch : name.substr(std::string_view(kGroups[group]).size())) {
        if (std::isupper(static_cast<unsigned char>(ch)) && !out.empty())
            out += ' ';
        out += ch;
    }
    return out;
}

std::string upper(std::string s) {
    for (char &ch : s)
        ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

int number(const json &v, int fallback) { return v.is_number() ? int(std::lround(v.get<double>())) : fallback; }

// Returns the camera in use: the streaming one, else the selected one, else the active one, else the first.
const UsbCamera *camera_in_use(const Status &s) {
    auto find = [&](const std::string &id) -> const UsbCamera * {
        for (const UsbCamera &u : s.cameras)
            if (!id.empty() && u.id == id)
                return &u;
        return nullptr;
    };
    const UsbCamera *cam = s.usb_stream.state == UsbStream::State::Streaming ? find(s.usb_stream.camera) : nullptr;
    for (const std::string *id : {&s.camera, &s.active_camera})
        if (!cam)
            cam = find(*id);
    if (!cam && !s.cameras.empty())
        cam = &s.cameras.front();
    return cam;
}

// Returns the mode as the Resolution row shows it, such as "Auto" or "640×480 · 30 fps · MJPEG".
std::string mode_label(const UsbCamera *cam, bool automatic) {
    if (!cam || automatic || !cam->settings.width)
        return "Auto";
    char buf[64];
    snprintf(buf, sizeof(buf), "%d×%d  ·  %d fps  ·  %s", cam->settings.width, cam->settings.height,
             cam->settings.fps, upper(cam->settings.format).c_str());
    return buf;
}

// Returns the selected camera as the Settings page shows it.
std::string camera_label(const Status &s) {
    std::string current = "Automatic";
    for (const auto &cam : s.cameras)
        if (cam.id == s.camera)
            current = cam.label;
    if (!s.camera.empty() && current == "Automatic")
        current = "Not connected";
    return current;
}

// Breaks the text at spaces into at most max_lines lines that fit in max_w, and shortens the last line.
std::vector<std::string> wrap(const Font &f, const std::string &text, float px, float max_w, size_t max_lines) {
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        const size_t sp = text.find(' ', i);
        const std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        i = sp == std::string::npos ? text.size() : sp + 1;
        const std::string joined = line.empty() ? word : line + " " + word;
        if (line.empty() || f.measure(joined, px) <= max_w || lines.size() + 1 == max_lines)
            line = joined;
        else {
            lines.push_back(line);
            line = word;
        }
    }
    lines.push_back(line);
    std::string &last = lines.back();
    while (f.measure(last, px) > max_w && last.size() > 1) {   // Cut whole UTF-8 sequences, as fit does.
        do
            last.pop_back();
        while (!last.empty() && (static_cast<unsigned char>(last.back()) & 0xc0) == 0x80);
        if (f.measure(last + "…", px) <= max_w) {
            last += "…";
            break;
        }
    }
    return lines;
}

constexpr int kModeRows = 8;   // Rows on one page of the Resolution list, with Auto.

}

void DashboardView::render_mouth(Canvas &c, const Status &s) {
    const float W = float(c.width());
    render_back(c, "Mouth");
    render_tabs(c, W - kMargin, {"Camera", "Shapes"}, fork_.mouth_tab, "tab:mouth");

    if (fork_.mouth_tab == 0) {
        // The preview, flipped and rotated as the model sees it, with the crop box (stored in source pixels).
        const Rect view{kMargin, 140, 400, 400};
        c.fill_rect(view, kBg, 4);
        c.stroke_rect(view, kLine, 4, 1);
        const auto &img = fork_.data.preview;
        const json mode = setting("/sources/face/mode", "usb");
        const bool url_mode = mode == "url", pc_mode = mode == "pc", usb_mode = !url_mode && !pc_mode;
        const std::string url = setting("/sources/face/url", "").get<std::string>();
        const json sq = setting("/pipelines/face/roi_square", true);
        const bool square = !sq.is_boolean() || sq.get<bool>();
        using State = UsbStream::State;
        const State st = usb_mode ? s.usb_stream.state : State::None;
        // The USB source reports Waiting while it is stopped, so the feed status tells why: tracking off or not worn.
        const bool waiting = st == State::Waiting && s.mouth.available;
        const bool tracking_off = waiting && !s.mouth.tracking;
        const bool paused = waiting && s.mouth.tracking && s.mouth.health == Health::Paused;
        // A USB camera that is not streaming shows its state instead of the last frame.
        const bool live = st == State::None || st == State::Streaming;
        bool wide = false;
        if (!pc_mode && live && img && img->width && img->height) {
            PreviewTransform tf;
            tf.w = float(img->width);
            tf.h = float(img->height);
            // The model-side rotate and flip are for Stream URL; in USB mode the frame comes turned by the camera.
            const json rot = usb_mode ? json(0) : setting("/pipelines/face/rotate", 0);
            tf.q = rot.is_number() ? (int(rot.get<double>()) / 90 % 4 + 4) % 4 : 0;
            const json flip = usb_mode ? json("none") : setting("/pipelines/face/flip", "none");
            const std::string fl = flip.is_string() ? flip.get<std::string>() : "none";
            tf.fh = fl.find('h') != std::string::npos;
            tf.fv = fl.find('v') != std::string::npos;
            const float w = tf.view_w(), h = tf.view_h();
            const float s = std::min(view.w / w, view.h / h);
            const float ox = view.x + (view.w - w * s) / 2, oy = view.y + (view.h - h * s) / 2;
            for (int y = int(oy); y < int(oy + h * s); y++)
                for (int x = int(ox); x < int(ox + w * s); x++) {
                    const auto [fx, fy] = tf.to_source((x - ox + 0.5f) / s, (y - oy + 0.5f) / s);
                    const uint32_t sx = std::min(img->width - 1, uint32_t(std::max(0.f, fx)));
                    const uint32_t sy = std::min(img->height - 1, uint32_t(std::max(0.f, fy)));
                    const uint8_t g = img->pixels[size_t(sy) * img->width + sx];
                    c.blend(x, y, {g, g, g, 255}, 1);
                }
            // An ROI that is empty or does not fit means the whole frame. src is in source pixels, vb in the preview's.
            Rect src{0, 0, tf.w, tf.h};
            const json roi = setting("/pipelines/face/roi", json::array());
            const bool has_roi = roi.is_array() && roi.size() == 4 && roi[2].get<float>() > 0 && roi[3].get<float>() > 0 &&
                                 roi[0].get<float>() + roi[2].get<float>() <= tf.w &&
                                 roi[1].get<float>() + roi[3].get<float>() <= tf.h;
            if (has_roi)
                src = {roi[0].get<float>(), roi[1].get<float>(), roi[2].get<float>(), roi[3].get<float>()};
            Rect vb = tf.box_to_view(src);
            const DragState &d = fork_.drag;
            const bool dragging = (d.kind == DragState::Kind::CropMove || d.kind == DragState::Kind::CropCorner) &&
                                  d.view.w > 0;
            if (dragging)
                vb = d.view;
            const Rect box{ox + vb.x * s, oy + vb.y * s, vb.w * s, vb.h * s};
            const Color shade = Color::hex(0x000000, 150);
            c.fill_rect({ox, oy, w * s, box.y - oy}, shade);
            c.fill_rect({ox, box.y + box.h, w * s, oy + h * s - box.y - box.h}, shade);
            c.fill_rect({ox, box.y, box.x - ox, box.h}, shade);
            c.fill_rect({box.x + box.w, box.y, ox + w * s - box.x - box.w, box.h}, shade);
            c.stroke_rect(box, kAccentHover, 0, 3);
            json geo = {{"scale", s}, {"w", w},     {"h", h},     {"bx", vb.x}, {"by", vb.y},  {"bw", vb.w}, {"bh", vb.h},
                        {"sw", tf.w}, {"sh", tf.h}, {"q", tf.q},  {"fh", tf.fh}, {"fv", tf.fv}, {"sq", square}};
            // The corner hits come first because the first hit wins, and shrink with the box so that it still moves.
            const float hs = std::min(60.f, std::min(box.w, box.h) / 3);
            for (int k = 0; k < 4; k++) {
                const float cx = box.x + (k & 1 ? box.w : 0), cy = box.y + (k & 2 ? box.h : 0);
                c.fill_circle(cx, cy, std::min(13.f, hs / 2), kText);
                Hit corner{Hit::Fork, {cx - hs / 2, cy - hs / 2, hs, hs}};
                corner.text = "crop:corner";
                corner.value = k;
                corner.data = geo;
                hits_.push_back(corner);
            }
            Hit move{Hit::Fork, box};
            move.text = "crop:move";
            move.data = geo;
            hits_.push_back(move);
            // The size and the ratio as the model sees them, after rotate and flip; it stretches any box to a square.
            const long vw = std::lround(vb.w), vh = std::lround(vb.h);
            wide = vw > vh;
            char buf[96];
            snprintf(buf, sizeof(buf), "Crop: %s%ld × %ld  ·  %.2f : 1", has_roi || dragging ? "" : "whole frame, ", vw,
                     vh, vh > 0 ? double(vw) / double(vh) : 0.0);
            regular_.draw(c, kMargin, 572, buf, 20, wide ? kAmber : kText);
            if (wide)
                regular_.draw(c, kMargin, 598, "Babble recommends square or taller.", 19, kAmber);
        } else {
            const std::string msg = pc_mode                   ? "The PC app sends the mouth"
                                    : url_mode && url.empty() ? "Set a stream URL"
                                    : url_mode                ? "Waiting for the stream"
                                    : st == State::Opening    ? "Opening the camera…"
                                    : st == State::Busy       ? "Camera busy: another app has it. Is mouthcam running?"
                                    : st == State::Error      ? (s.usb_stream.error.empty() ? "Camera error" : s.usb_stream.error)
                                    : st == State::Streaming  ? "Waiting for the camera"
                                    : tracking_off ? "Mouth tracking is off. Turn on Mouth Tracking on the front page."
                                    : paused       ? "Paused while the headset is off."
                                                   : "Waiting for a USB camera";
            const auto lines = wrap(regular_, msg, 24, view.w - 48, 3);
            float ly = view.y + view.h / 2 - float(lines.size() - 1) * 17;
            for (const std::string &line : lines) {
                regular_.draw(c, view.x + (view.w - regular_.measure(line, 24)) / 2, ly, line, 24,
                              st == State::Busy || st == State::Error ? kAmber : kMuted);
                ly += 34;
            }
        }
        if (!pc_mode)
            regular_.draw(c, kMargin, wide ? 624 : 600, "Drag the box to move it. Drag a corner to resize.", 18, kMuted);

        const float x = 488, cw = W - kMargin - x, right = W - kMargin - 12;
        float y = 140;
        // A card with a title, and a hint under it or space for a field.
        auto card = [&](float h, const char *title, const std::string &hint, float hint_w) {
            c.fill_rect({x, y + 4, cw, h - 8}, kCard, 6);
            const bool top = !hint.empty() || h > 110;
            medium_.draw(c, x + 20, y + (top ? 44 : h / 2 + medium_.cap_height(26) / 2), title, 26, kText);
            if (!hint.empty())
                regular_.draw(c, x + 20, y + 76, fit(regular_, hint, 20, hint_w), 20, kMuted);
        };
        // Three modes need the card's width, so the hint goes on a line under them.
        const char *camera_hint = pc_mode    ? "Baballonia on the PC sends it. No camera runs here."
                                  : url_mode ? "An MJPEG stream (mouthcam)."
                                             : "A Babble tracker or a USB webcam.";
        card(136, "Camera", "", 0);
        render_value_segments(c, right, y + 16, "/sources/face/mode", {"USB", "Stream URL", "PC App"}, {"usb", "url", "pc"},
                              150);
        regular_.draw(c, x + 20, y + 114, fit(regular_, camera_hint, 20, cw - 40), 20, kMuted);
        y += 136;
        if (usb_mode) {
            // Two rows that open the camera list and the mode list, and the stream as it runs.
            c.fill_rect({x, y + 4, cw, 160}, kCard, 6);
            auto link_row = [&](float ry, const char *title, const std::string &value, const char *action) {
                const Rect r{x + 8, ry, cw - 16, 60};
                if (hovered(r))
                    c.fill_rect(r, kCardHover, 4);
                const float base = ry + 30 + medium_.cap_height(24) / 2, chev = r.x + r.w - 16;
                medium_.draw(c, x + 20, base, title, 24, kText);
                const std::string v = fit(regular_, value, 22, cw - 240);
                regular_.draw(c, chev - 24 - regular_.measure(v, 22), base, v, 22, kMuted);
                c.line(chev - 8, ry + 22, chev, ry + 30, 3, kMuted);
                c.line(chev, ry + 30, chev - 8, ry + 38, 3, kMuted);
                Hit h{Hit::Fork, r};
                h.text = action;
                hits_.push_back(h);
            };
            link_row(y + 10, "USB camera", camera_label(s), "open:cameras");
            const UsbCamera *cam = camera_in_use(s);
            link_row(y + 72, "Resolution", mode_label(cam, cam && automatic_mode(*cam)), "open:modes");
            const UsbStream &us = s.usb_stream;
            std::string line;
            Color col = kMuted;
            char buf[96];
            switch (st) {
            case State::Streaming: {
                const double fps = us.fps_real > 0 ? us.fps_real : us.fps_asked;
                snprintf(buf, sizeof(buf), "%u×%u %s  ·  %.0f fps", unsigned(us.width), unsigned(us.height),
                         upper(us.format).c_str(), fps);
                line = buf;
                if (us.fps_real > 0 && us.fps_asked > 0 && std::fabs(us.fps_real - us.fps_asked) >= 1) {
                    snprintf(buf, sizeof(buf), " (asked %.0f)", us.fps_asked);
                    line += buf;
                }
                col = kGreen;
                break;
            }
            case State::Waiting: line = tracking_off ? "Mouth tracking is off" : paused ? "Paused" : "No camera"; break;
            case State::Opening: line = "Opening…"; break;
            case State::Busy: line = "Busy: another app has the camera"; col = kAmber; break;
            case State::Error: line = us.error.empty() ? "Error" : us.error; col = kAmber; break;
            case State::None: break;
            }
            if (!line.empty())
                regular_.draw(c, x + 20, y + 152, fit(regular_, line, 20, cw - 40), 20, col);
            y += 168;
        } else {
            card(132, "Stream URL", "", 0);
            if (!url_mode)
                regular_.draw(c, x + 200, y + 44, "Used when Camera is Stream URL.", 20, kMuted);
            render_text_field(c, {x + 20, y + 62, cw - 40, 56}, "/sources/face/url", url, "http://127.0.0.1:8082/");
            y += 132;
        }
        // The labels turn clockwise in every mode. USB mode saves the turn for the camera, whose source turns
        // clockwise before the crop; the other modes save it for the model, which turns counter-clockwise after it.
        const std::vector<std::string> rot_labels{"0°", "90°", "180°", "270°"}, flip_labels{"None", "Mirror", "Vertical", "Both"};
        const UsbCamera *turn_cam = usb_mode ? camera_in_use(s) : nullptr;
        if (usb_mode && !turn_cam) {
            card(88, "Rotate", "Connect a camera first.", cw - 200);
            y += 88;
            card(88, "Flip", "", 0);
        } else if (usb_mode) {
            const CameraTurn now = camera_turn(*turn_cam);
            // The frame size from the camera, before its turn, which the crop box follows.
            const bool streams = s.usb_stream.state == State::Streaming && s.usb_stream.camera == turn_cam->id;
            const float fw = streams ? s.usb_stream.width : float(turn_cam->settings.width);
            const float fh = streams ? s.usb_stream.height : float(turn_cam->settings.height);
            const std::string cam_ptr = camera_setting_id(turn_cam->id);
            // One click sends the changed camera fields and the crop box turned with them. The model-side turn stays
            // for Stream URL, and USB mode skips it.
            auto turn_to = [&](CameraTurn next) {
                json b = json::array();
                if (next == now)
                    return b;
                if (next.rotate != now.rotate)
                    b.push_back(json::array({cam_ptr + "/rotate", next.rotate}));
                if (next.flip_h != now.flip_h)
                    b.push_back(json::array({cam_ptr + "/flip_h", next.flip_h}));
                if (next.flip_v != now.flip_v)
                    b.push_back(json::array({cam_ptr + "/flip_v", next.flip_v}));
                const json roi = setting("/pipelines/face/roi", json::array());
                if (roi.is_array() && roi.size() == 4 && roi[2].is_number() && roi[3].is_number()) {
                    const bool side = now.rotate % 180 == 90;   // The box is in the turned frame.
                    const float tw = side ? fh : fw, th = side ? fw : fh;
                    const Rect r{roi[0].get<float>(), roi[1].get<float>(), roi[2].get<float>(), roi[3].get<float>()};
                    if (fw <= 0 || fh <= 0)
                        b.push_back(json::array({"/pipelines/face/roi", json::array()}));
                    else if (r.w > 0 && r.h > 0 && r.x + r.w <= tw && r.y + r.h <= th) {
                        const Rect t = turn_crop(r, fw, fh, now, next);
                        b.push_back(json::array({"/pipelines/face/roi", json::array({std::lround(t.x), std::lround(t.y),
                                                                                     std::lround(t.w), std::lround(t.h)})}));
                    }
                }
                return b;
            };
            card(88, "Rotate", "For this camera", cw - 460);
            const json rot_now = now.rotate;
            render_value_segments(c, right, y + 8, "", rot_labels, {0, 90, 180, 270}, 100, &rot_now);
            for (size_t k = 0; k < 4; k++) {
                Hit &h = hits_[hits_.size() - 4 + k];
                h.text = "turn";
                h.data = turn_to({int(k) * 90, now.flip_h, now.flip_v});
            }
            y += 88;
            card(88, "Flip", "", 0);
            const json flip_now = int(now.flip_h) + 2 * int(now.flip_v);
            render_value_segments(c, right, y + 8, "", flip_labels, {0, 1, 2, 3}, 112, &flip_now);
            for (size_t k = 0; k < 4; k++) {
                Hit &h = hits_[hits_.size() - 4 + k];
                h.text = "turn";
                h.data = turn_to({now.rotate, bool(k & 1), bool(k & 2)});
            }
        } else {
            card(88, "Rotate", "", 0);
            render_value_segments(c, right, y + 8, "/pipelines/face/rotate", rot_labels, {0, 270, 180, 90}, 100);
            y += 88;
            card(88, "Flip", "", 0);
            render_value_segments(c, right, y + 8, "/pipelines/face/flip", flip_labels, {"none", "h", "v", "hv"}, 112);
        }
        if (!pc_mode) {
            const Rect whole = render_small_button(c, W - kMargin, kBottomY, "Whole Frame", "crop:whole");
            // The square lock, with its label, left of Whole Frame.
            const Rect sw{whole.x - 24 - 92, kBottomY + 6, 92, 52};
            const char *label = "Keep square";
            const float lx = sw.x - 8 - medium_.measure(label, 24);
            const Rect hit{lx - 12, kBottomY, whole.x - 12 - (lx - 12), 64};
            render_switch(c, sw, square, hovered(hit));
            medium_.draw(c, lx, kBottomY + 32 + medium_.cap_height(24) / 2, label, 24, kText);
            Hit h{Hit::Fork, hit};
            h.text = "crop:square";
            h.value = square ? 0 : 1;
            hits_.push_back(h);
        }
        return;
    }

    // The Shapes tab: the calibration mode, the group, and a page of range sliders.
    float y = 140;
    c.fill_rect({kMargin, y + 4, W - 2 * kMargin, 96}, kCard, 6);
    const float seg_x = render_value_segments(c, W - kMargin - 12, y + 16, "/pipelines/face/calibration/mode",
                                              {"Manual", "Auto"}, {"manual", "auto"}, 150);
    medium_.draw(c, kMargin + 20, y + 44, "Calibration", 26, kText);
    const bool manual = setting("/pipelines/face/calibration/mode", "manual") == "manual";
    const std::string hint = manual ? "Uses the ranges below." : "Learns the top of each shape again each time the headset goes on.";
    regular_.draw(c, kMargin + 20, y + 78, fit(regular_, hint, 20, seg_x - kMargin - 44), 20, kMuted);
    y += 112;

    medium_.draw(c, kMargin, y + 46, "Shapes", 26, kText);
    render_segments(c, W - kMargin, y, {std::begin(kGroupLabels), std::end(kGroupLabels)}, fork_.shape_group,
                    [&](size_t k, const Rect &r) {
                        Hit h{Hit::Fork, r};
                        h.text = "group";
                        h.value = double(k);
                        hits_.push_back(h);
                    });
    y += 88;

    const auto shapes = shapes_in(fork_.shape_group);
    const int pages = int((shapes.size() + kPerPage - 1) / kPerPage);
    fork_.shape_page = std::min(fork_.shape_page, pages - 1);
    const float cell_w = (W - 2 * kMargin - 16) / 2, row_h = 70;
    json reset = json::array();
    for (size_t i = 0; i < shapes.size(); i++) {
        const std::string ptr = "/pipelines/face/calibration/ranges/" + shapes[i];
        reset.push_back({ptr, 0, 1});
        const int k = int(i) - fork_.shape_page * kPerPage;
        if (k < 0 || k >= kPerPage)
            continue;
        const float cx = kMargin + float(k % 2) * (cell_w + 16), cy = y + float(k / 2) * row_h;
        c.fill_rect({cx, cy + 3, cell_w, row_h - 6}, kCard, 6);
        medium_.draw(c, cx + 16, cy + row_h / 2 + medium_.cap_height(22) / 2,
                     fit(medium_, shape_label(shapes[i], fork_.shape_group), 22, 160), 22, manual ? kText : kMuted);
        render_range(c, {cx + 184, cy + 6, cell_w - 196, row_h - 12}, ptr, "_" + shapes[i] + "Pre", false, !manual);
    }

    float right = W - kMargin;
    if (pages > 1) {
        right = render_small_button(c, right, kBottomY, "Next", "shapes:next").x - 12;
        char buf[32];
        snprintf(buf, sizeof(buf), "%d / %d", fork_.shape_page + 1, pages);
        const float tw = regular_.measure(buf, 22);
        regular_.draw(c, right - tw, kBottomY + 40, buf, 22, kMuted);
        right = render_small_button(c, right - tw - 12, kBottomY, "Previous", "shapes:prev").x - 28;
    }
    right = render_small_button(c, right, kBottomY, "Reset Maximums", "reset:max").x - 12;
    hits_.back().data = reset;
    render_small_button(c, right, kBottomY, "Reset Minimums", "reset:min");
    hits_.back().data = reset;
}

bool DashboardView::automatic_mode(const UsbCamera &cam) const {
    // Auto is a camera with no saved format, or the source defaults that Auto saves.
    const json entry = setting(camera_setting_id(cam.id), json());
    if (!entry.is_object() || !entry.contains("format"))
        return true;
    const json fps = setting("/sources/face/fps", 30);
    return entry["format"] == "mjpeg" && entry.value("width", 0) == number(setting("/sources/face/width", 240), 240) &&
           entry.value("height", 0) == number(setting("/sources/face/height", 240), 240) &&
           std::fabs(entry.value("fps", 0.0) - (fps.is_number() ? fps.get<double>() : 30.0)) < 0.5;
}

CameraTurn DashboardView::camera_turn(const UsbCamera &cam) const {
    // A field that this view just sent wins over the settings from the last status.
    const std::string ptr = camera_setting_id(cam.id);
    CameraTurn t{cam.settings.rotate, cam.settings.flip_h, cam.settings.flip_v};
    t.rotate = number(setting(ptr + "/rotate", t.rotate), t.rotate);
    const json fh = setting(ptr + "/flip_h", t.flip_h), fv = setting(ptr + "/flip_v", t.flip_v);
    t.flip_h = fh.is_boolean() ? fh.get<bool>() : t.flip_h;
    t.flip_v = fv.is_boolean() ? fv.get<bool>() : t.flip_v;
    return t;
}

// The Resolution page: Auto, then a row for each format and size of the camera in use, with its rates. A pick saves
// the whole settings entry of that camera, as upstream's Camera Options page does.
void DashboardView::render_resolution(Canvas &c, const Status &s) {
    const float W = float(c.width());
    render_back(c, "Resolution");
    const UsbCamera *cam = camera_in_use(s);
    if (cam) {
        const std::string label = fit(regular_, cam->label, 22, 520);
        regular_.draw(c, W - kMargin - regular_.measure(label, 22), 84, label, 22, kMuted);
    }
    const bool automatic = cam && automatic_mode(*cam);

    // A pick sends the camera settings and, if the frame size changes, the crop box scaled to the new frame.
    const json roi = setting("/pipelines/face/roi", json::array());
    const json sq = setting("/pipelines/face/roi_square", true);
    const bool square = !sq.is_boolean() || sq.get<bool>();
    const CameraTurn turn = cam ? camera_turn(*cam) : CameraTurn{};
    auto batch = [&](const UsbCamera::Settings &next, int nw, int nh) {
        json b = json::array({json::array({camera_setting_id(cam->id),
                                           {{"format", next.format}, {"width", next.width}, {"height", next.height},
                                            {"fps", next.fps}, {"rotate", turn.rotate}, {"flip_h", turn.flip_h},
                                            {"flip_v", turn.flip_v}}})});
        int sw = s.usb_stream.width, sh = s.usb_stream.height;
        if (turn.rotate % 180 == 90) {   // The crop box is in the turned frame.
            std::swap(sw, sh);
            std::swap(nw, nh);
        }
        if (!(roi.is_array() && roi.size() == 4 && roi[2].is_number() && roi[3].is_number()) || sw <= 0 || sh <= 0 ||
            nw <= 0 || nh <= 0 || (nw == sw && nh == sh))
            return b;
        double rw = std::min(roi[2].get<double>() * nw / sw, double(nw));
        double rh = std::min(roi[3].get<double>() * nh / sh, double(nh));
        double rx = std::clamp(roi[0].get<double>() * nw / sw, 0.0, nw - rw);
        double ry = std::clamp(roi[1].get<double>() * nh / sh, 0.0, nh - rh);
        if (square) {
            const double side = std::min(rw, rh);
            rx += (rw - side) / 2;
            ry += (rh - side) / 2;
            rw = rh = side;
        }
        const long x = std::lround(rx), y = std::lround(ry);
        const long bw = std::min(std::lround(rw), long(nw) - x), bh = std::min(std::lround(rh), long(nh) - y);
        const long side = std::min(bw, bh);
        if (bw > 0 && bh > 0)
            b.push_back(json::array({"/pipelines/face/roi", json::array({x, y, square ? side : bw, square ? side : bh})}));
        return b;
    };

    auto check = [&](const Rect &box) {
        c.line(box.x + 26, box.y + box.h / 2, box.x + 38, box.y + box.h / 2 + 12, 5, kAccent);
        c.line(box.x + 38, box.y + box.h / 2 + 12, box.x + 60, box.y + box.h / 2 - 14, 5, kAccent);
    };
    // One row for each format and size, in the order that the camera lists them.
    struct Row {
        const UsbCamera::Format *format;
        const UsbCamera::Size *size;
    };
    std::vector<Row> rows;
    if (cam)
        for (const auto &f : cam->formats)
            for (const auto &z : f.sizes)
                rows.push_back({&f, &z});
    const float row_h = 60, tx = kMargin + 84;
    float y = 140;
    const int pages = std::max(1, int((rows.size() + kModeRows) / kModeRows));   // Auto takes the first row.
    fork_.mode_page = std::min(fork_.mode_page, pages - 1);
    if (fork_.mode_page == 0) {
        const Rect box{kMargin, y, W - 2 * kMargin, row_h - 8};
        c.fill_rect(box, automatic || hovered(box) ? kCardHover : kCard, 6);
        if (automatic)
            check(box);
        medium_.draw(c, tx, box.y + box.h / 2 + medium_.cap_height(26) / 2, "Auto", 26, kText);
        const char *hint = "The runtime picks a mode.";
        regular_.draw(c, box.x + box.w - 24 - regular_.measure(hint, 21), box.y + box.h / 2 + regular_.cap_height(21) / 2,
                      hint, 21, kMuted);
        if (cam) {
            // Auto saves the source defaults, which the camera streams as its nearest MJPEG size (or of its first
            // format if it has no MJPEG), as uvc::pick_mode does.
            UsbCamera::Settings next = cam->settings;
            next.format = "mjpeg";
            next.width = number(setting("/sources/face/width", 240), 240);
            next.height = number(setting("/sources/face/height", 240), 240);
            next.fps = number(setting("/sources/face/fps", 30), 30);
            const UsbCamera::Format *f = cam->formats.empty() ? nullptr : &cam->formats.front();
            for (const auto &k : cam->formats)
                if (k.key == "mjpeg")
                    f = &k;
            int nw = 0, nh = 0;
            if (f)
                for (const auto &z : f->sizes)
                    if (!nw || std::abs(z.width - next.width) + std::abs(z.height - next.height) <
                                   std::abs(nw - next.width) + std::abs(nh - next.height)) {
                        nw = z.width;
                        nh = z.height;
                    }
            Hit h{Hit::Fork, box};
            h.text = "mode";
            h.data = batch(next, nw, nh);
            hits_.push_back(h);
        }
        y += row_h;
    }
    const size_t first = fork_.mode_page == 0 ? 0 : size_t(fork_.mode_page * kModeRows - 1);
    for (size_t i = first; i < rows.size() && y + row_h <= 140 + kModeRows * row_h; i++) {
        const UsbCamera::Format &f = *rows[i].format;
        const UsbCamera::Size &z = *rows[i].size;
        const Rect box{kMargin, y, W - 2 * kMargin, row_h - 8};
        const bool on = !automatic && cam->settings.format == f.key && cam->settings.width == z.width &&
                        cam->settings.height == z.height;
        c.fill_rect(box, on ? kCardHover : kCard, 6);
        if (on)
            check(box);
        const float base = box.y + box.h / 2 + medium_.cap_height(26) / 2;
        char buf[48];
        snprintf(buf, sizeof(buf), "%d × %d", z.width, z.height);
        medium_.draw(c, tx, base, buf, 26, kText);
        regular_.draw(c, tx + 170, base, f.label, 22, kMuted);
        // The rates, highest first from the left, as small segments. They fit between the format and the right edge
        // of the row, and the lowest rates are left out if even narrow segments do not fit.
        float right = box.x + box.w - 12;
        const float left = tx + 170 + regular_.measure(f.label, 22) + 24;
        const size_t n = std::min(z.fps.size(), std::max<size_t>(1, size_t((right - left + 8) / (72 + 8))));
        const float chip_w = std::min(104.f, (right - left + 8) / float(n) - 8), chip_h = 40;
        for (size_t k = n; k-- > 0;) {
            const Rect chip{right - chip_w, box.y + (box.h - chip_h) / 2, chip_w, chip_h};
            const bool sel = on && z.fps[k] == cam->settings.fps;
            c.fill_rect(chip, sel ? kAccent : hovered(chip) ? kCardHover : kPanel, 4);
            snprintf(buf, sizeof(buf), "%d fps", z.fps[k]);
            medium_.draw(c, chip.x + (chip.w - medium_.measure(buf, 21)) / 2, chip.y + (chip.h + medium_.cap_height(21)) / 2,
                         buf, 21, sel ? kText : kMuted);
            UsbCamera::Settings next = cam->settings;
            next.format = f.key;
            next.width = z.width;
            next.height = z.height;
            next.fps = z.fps[k];
            Hit h{Hit::Fork, chip};
            h.text = "mode";
            h.data = batch(next, z.width, z.height);
            hits_.push_back(h);
            right -= chip_w + 8;
        }
        y += row_h;
    }
    if (!cam)
        regular_.draw(c, kMargin, y + 44, "Connect a camera first.", 24, kMuted);
    else if (rows.empty())
        regular_.draw(c, kMargin, y + 44, "This camera lists no modes.", 24, kMuted);

    if (pages > 1) {
        float right = render_small_button(c, W - kMargin, kBottomY, "Next", "modes:next").x - 12;
        char buf[32];
        snprintf(buf, sizeof(buf), "%d / %d", fork_.mode_page + 1, pages);
        const float tw = regular_.measure(buf, 22);
        regular_.draw(c, right - tw, kBottomY + 40, buf, 22, kMuted);
        render_small_button(c, right - tw - 12, kBottomY, "Previous", "modes:prev");
    }
}

}
