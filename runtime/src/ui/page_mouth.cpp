// The Mouth page: the camera, a live preview with the crop box, and the manual ranges of the 45 shapes.
#include "ui/dashboard_view.h"

#include "model/babble_mono_face.h"
#include "ui/style.h"

#include <cctype>
#include <cstdio>

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

}

void DashboardView::render_mouth(Canvas &c) {
    const float W = float(c.width());
    render_back(c, "Mouth");
    render_tabs(c, W - kMargin, {"Camera", "Shapes"}, fork_.mouth_tab, "tab:mouth");

    if (fork_.mouth_tab == 0) {
        // The preview, flipped and rotated as the model sees it, with the crop box (stored in source pixels).
        const Rect view{kMargin, 140, 400, 400};
        c.fill_rect(view, kBg, 4);
        c.stroke_rect(view, kLine, 4, 1);
        const auto &img = fork_.data.preview;
        const bool url_mode = setting("/sources/face/mode", "usb") == "url";
        const std::string url = setting("/sources/face/url", "").get<std::string>();
        if (img && img->width && img->height) {
            PreviewTransform tf;
            tf.w = float(img->width);
            tf.h = float(img->height);
            const json rot = setting("/pipelines/face/rotate", 0);
            tf.q = rot.is_number() ? (int(rot.get<double>()) / 90 % 4 + 4) % 4 : 0;
            const json flip = setting("/pipelines/face/flip", "none");
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
            if (roi.is_array() && roi.size() == 4 && roi[2].get<float>() > 0 && roi[3].get<float>() > 0 &&
                roi[0].get<float>() + roi[2].get<float>() <= tf.w && roi[1].get<float>() + roi[3].get<float>() <= tf.h)
                src = {roi[0].get<float>(), roi[1].get<float>(), roi[2].get<float>(), roi[3].get<float>()};
            Rect vb = tf.box_to_view(src);
            const DragState &d = fork_.drag;
            if ((d.kind == DragState::Kind::CropMove || d.kind == DragState::Kind::CropResize) && d.view.w > 0) {
                vb = d.view;
                src = tf.box_to_source(vb);
            }
            const float bx = vb.x, by = vb.y, bw = vb.w, bh = vb.h;
            const Rect box{ox + bx * s, oy + by * s, bw * s, bh * s};
            const Color shade = Color::hex(0x000000, 150);
            c.fill_rect({ox, oy, w * s, box.y - oy}, shade);
            c.fill_rect({ox, box.y + box.h, w * s, oy + h * s - box.y - box.h}, shade);
            c.fill_rect({ox, box.y, box.x - ox, box.h}, shade);
            c.fill_rect({box.x + box.w, box.y, ox + w * s - box.x - box.w, box.h}, shade);
            c.stroke_rect(box, kAccentHover, 0, 3);
            c.fill_circle(box.x + box.w, box.y + box.h, 13, kText);
            json geo = {{"scale", s}, {"w", w},    {"h", h},    {"bx", bx},   {"by", by},   {"bs", std::min(bw, bh)},
                        {"sw", tf.w}, {"sh", tf.h}, {"q", tf.q}, {"fh", tf.fh}, {"fv", tf.fv}};
            Hit resize{Hit::Fork, {box.x + box.w - 30, box.y + box.h - 30, 60, 60}};
            resize.text = "crop:resize";
            resize.json = geo;
            hits_.push_back(resize);
            Hit move{Hit::Fork, box};
            move.text = "crop:move";
            move.json = geo;
            hits_.push_back(move);
            char buf[64];
            if (roi.empty())
                snprintf(buf, sizeof(buf), "Crop: whole frame (%.0f × %.0f)", double(tf.w), double(tf.h));
            else   // In source pixels, as saved.
                snprintf(buf, sizeof(buf), "Crop: %.0f, %.0f  ·  %.0f × %.0f", double(src.x), double(src.y),
                         double(src.w), double(src.h));
            regular_.draw(c, kMargin, 572, buf, 20, kText);
        } else {
            const std::string msg = url_mode && url.empty() ? "Set a stream URL" : url_mode ? "Waiting for the stream"
                                                                                            : "Waiting for the camera";
            regular_.draw(c, view.x + (view.w - regular_.measure(msg, 24)) / 2, view.y + view.h / 2, msg, 24, kMuted);
        }
        regular_.draw(c, kMargin, 600, "Drag the box to crop. Drag its corner to resize.", 19, kMuted);

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
        card(104, "Camera", url_mode ? "An MJPEG stream (mouthcam)." : "Finds a Babble tracker.", 300);
        render_value_segments(c, right, y + 16, "/sources/face/mode", {"USB", "Stream URL"}, {"usb", "url"}, 150);
        y += 104;
        card(132, "Stream URL", "", 0);
        if (!url_mode)
            regular_.draw(c, x + 200, y + 44, "Used when Camera is Stream URL.", 20, kMuted);
        render_text_field(c, {x + 20, y + 62, cw - 40, 56}, "keyboard", url, "http://127.0.0.1:8082/");
        y += 132;
        card(88, "Rotate", "", 0);
        render_value_segments(c, right, y + 8, "/pipelines/face/rotate", {"0°", "90°", "180°", "270°"},
                              {0, 90, 180, 270}, 100);
        y += 88;
        card(88, "Flip", "", 0);
        render_value_segments(c, right, y + 8, "/pipelines/face/flip", {"None", "Mirror", "Vertical", "Both"},
                              {"none", "h", "v", "hv"}, 112);
        render_small_button(c, W - kMargin, kBottomY, "Whole Frame", "crop:whole");
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
    hits_.back().json = reset;
    render_small_button(c, right, kBottomY, "Reset Minimums", "reset:min");
    hits_.back().json = reset;
}

}
