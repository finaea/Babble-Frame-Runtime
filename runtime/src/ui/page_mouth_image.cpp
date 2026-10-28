// The Image tab of the Mouth page: the brightness and the tilt of the model input, and that input as the model sees it.
#include "ui/dashboard_view.h"

#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace babble::ui {

using namespace style;
using nlohmann::json;

void DashboardView::render_mouth_image(Canvas &c) {
    const float W = float(c.width());
    const bool pc_mode = setting("/sources/face/mode", "usb") == "pc";

    // The 224x224 model input after the crop, flips, turns, tilt and brightness, scaled up to the view.
    const Rect view{kMargin, 140, 400, 400};
    c.fill_rect(view, kBg, 4);
    c.stroke_rect(view, kLine, 4, 1);
    const auto &img = fork_.data.model_view;
    if (!pc_mode && img && img->width && img->height) {
        const float s = std::min(view.w / float(img->width), view.h / float(img->height));
        const float ox = view.x + (view.w - float(img->width) * s) / 2, oy = view.y + (view.h - float(img->height) * s) / 2;
        for (int y = int(oy); y < int(oy + float(img->height) * s); y++)
            for (int x = int(ox); x < int(ox + float(img->width) * s); x++) {
                const uint32_t sx = std::min(img->width - 1, uint32_t((x - ox) / s));
                const uint32_t sy = std::min(img->height - 1, uint32_t((y - oy) / s));
                const uint8_t g = img->pixels[size_t(sy) * img->width + sx];
                c.blend(x, y, {g, g, g, 255}, 1);
            }
        regular_.draw(c, kMargin, 572, "What the mouth model sees, 224 × 224.", 20, kText);
    } else {
        const char *msg = pc_mode ? "The PC app sends the mouth" : "No model input yet";
        regular_.draw(c, view.x + (view.w - regular_.measure(msg, 24)) / 2, view.y + view.h / 2, msg, 24, kMuted);
        if (!pc_mode)
            regular_.draw(c, kMargin, 572, "It shows while mouth tracking runs.", 20, kMuted);
    }

    const float x = 488, cw = W - kMargin - x;
    float y = 140;
    // A card with a title, a hint and a slider.
    auto card = [&](const char *title, const char *hint, const char *pointer, float lo, float hi, float step,
                    float neutral, const char *format) {
        c.fill_rect({x, y + 4, cw, 172}, kCard, 6);
        medium_.draw(c, x + 20, y + 44, title, 26, kText);
        regular_.draw(c, x + 20, y + 76, fit(regular_, hint, 20, cw - 40), 20, kMuted);
        render_slider(c, {x + 20, y + 92, cw - 40, 72}, pointer, lo, hi, step, neutral, format);
        y += 184;
    };
    card("Brightness", "Multiplies each pixel, as Baballonia's gamma does.", "/pipelines/face/gain", 0.5f, 3.0f, 0.05f,
         1.0f, "× %.2f");
    card("Tilt", "For a camera at an angle. + turns the picture counter-clockwise.", "/pipelines/face/tilt_deg",
         -45.0f, 45.0f, 1.0f, 0.0f, "%+.0f°");

    render_small_button(c, W - kMargin, kBottomY, "Reset", "image:reset");
    hits_.back().data = json::array({json::array({"/pipelines/face/gain", 1.0}),
                                     json::array({"/pipelines/face/tilt_deg", 0})});
}

// Draws a one-handle slider that fills from the neutral value, with the value above its right end.
void DashboardView::render_slider(Canvas &c, Rect r, const std::string &pointer, float lo, float hi, float step,
                                  float neutral, const char *format) {
    const json cur = setting(pointer, neutral);
    float v = cur.is_number() ? cur.get<float>() : neutral;
    if (fork_.drag.kind == DragState::Kind::Slider && fork_.drag.pointer == pointer)
        v = fork_.drag.min;

    const float knob = 13, x0 = r.x + knob + 2, x1 = r.x + r.w - knob - 2, ty = r.y + r.h - 18;
    auto at = [&](float t) { return x0 + (std::clamp(t, lo, hi) - lo) / (hi - lo) * (x1 - x0); };
    char buf[32];
    snprintf(buf, sizeof(buf), format, double(v));
    medium_.draw(c, x1 - medium_.measure(buf, 22), r.y + 20, buf, 22, kText);
    c.fill_rect({x0, ty - 4, x1 - x0, 8}, kLine, 4);
    c.fill_rect({std::min(at(neutral), at(v)), ty - 4, std::fabs(at(v) - at(neutral)), 8}, kAccent, 4);
    c.fill_rect({at(neutral) - 1, ty - 9, 2, 18}, kGrey);
    c.fill_circle(at(v), ty, hovered(r) ? knob + 1 : knob, kText);
    Hit h{Hit::Fork, r};
    h.text = "slider:" + pointer;
    h.data = {{"x0", x0}, {"x1", x1}, {"lo", lo}, {"hi", hi}, {"step", step}};
    hits_.push_back(h);
}

}
