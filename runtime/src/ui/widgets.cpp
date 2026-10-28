// The widgets and the input handling of the Eyes, Squint calibration and Mouth pages.
#include "ui/dashboard_view.h"

#include "core/clock.h"
#include "model/postprocess/manual_range.h"
#include "ui/style.h"

#include <cmath>
#include <cstdio>

namespace babble::ui {

using namespace style;
using nlohmann::json;

namespace {

// A value that the view sent counts until the config shows it, or for this long if the runtime drops it.
constexpr uint64_t kPendingNs = 3'000'000'000ull;
// The smallest distance between the two handles of a range.
constexpr float kMinGap = 0.02f;

float round2(float v) { return std::round(v * 100) / 100; }

}

json DashboardView::setting(const std::string &pointer, const json &fallback) const {
    if (auto it = fork_.pending.find(pointer); it != fork_.pending.end() && mono_ns() - it->second.second < kPendingNs)
        return it->second.first;
    const json::json_pointer ptr(pointer);
    if (fork_.data.config && fork_.data.config->contains(ptr))
        return (*fork_.data.config)[ptr];
    return fallback;
}

std::optional<float> DashboardView::signal(const std::string &name, bool any_age) const {
    const auto it = fork_.data.signals.find(name);
    if (it == fork_.data.signals.end() || !(it->second.recent || any_age))
        return std::nullopt;
    return it->second.value;
}

void DashboardView::send(ClickResult &out, const std::string &pointer, json value) {
    fork_.pending[pointer] = {value, mono_ns()};
    out.changes.emplace_back(pointer, std::move(value));
    out.redraw = true;
}

Rect DashboardView::render_small_button(Canvas &c, float right, float y, const std::string &text,
                                        const std::string &action, bool primary) {
    const Rect r = render_button(c, right, y, text, primary);
    Hit h{Hit::Fork, r};
    h.text = action;
    hits_.push_back(h);
    return r;
}

void DashboardView::render_tabs(Canvas &c, float right, const std::vector<std::string> &labels, int selected,
                                const std::string &action) {
    render_segments(c, right, 40, labels, selected, [&](size_t k, const Rect &r) {
        Hit h{Hit::Fork, r};
        h.text = action;
        h.value = double(k);
        hits_.push_back(h);
    });
}

float DashboardView::render_value_segments(Canvas &c, float right, float y, const std::string &pointer,
                                           const std::vector<std::string> &labels, const std::vector<json> &values,
                                           float seg_w, const json *current) {
    const json cur = current ? *current : setting(pointer, values.front());
    int selected = -1;
    for (size_t k = 0; k < values.size(); k++)
        if (cur == values[k])
            selected = int(k);
    // This copies render_segments with a width for each segment.
    const float seg_h = 64, pad = 4, px = 24;
    const float ctl_w = seg_w * float(labels.size()) + 2 * pad;
    const Rect ctl{right - ctl_w, y, ctl_w, seg_h + 2 * pad};
    c.fill_rect(ctl, kPanel, 4);
    for (size_t k = 0; k < labels.size(); k++) {
        const Rect seg{ctl.x + pad + float(k) * seg_w, ctl.y + pad, seg_w, seg_h};
        const bool on = int(k) == selected;
        if (on || hovered(seg))
            c.fill_rect(seg, on ? kAccent : kCardHover, 3);
        medium_.draw(c, seg.x + (seg.w - medium_.measure(labels[k], px)) / 2,
                     seg.y + (seg.h + medium_.cap_height(px)) / 2, labels[k], px, on ? kText : kMuted);
        Hit h{Hit::Fork, seg};
        h.text = "set:" + pointer;
        h.data = values[k];
        hits_.push_back(h);
    }
    return ctl.x;
}

// Draws a two-handle range slider with the live value as a dot, inside r.
void DashboardView::render_range(Canvas &c, Rect r, const std::string &pointer, const std::string &sig,
                                 bool signed_axis, bool dim) {
    const float lo = signed_axis ? -1 : 0, hi = 1;
    ManualRange range{lo, hi};
    ManualRange::parse(setting(pointer, json::array({lo, hi})), &range);
    if (fork_.drag.kind == DragState::Kind::Range && fork_.drag.pointer == pointer)
        range = {fork_.drag.min, fork_.drag.max};

    const float knob = 13, x0 = r.x + knob + 2, x1 = r.x + r.w - knob - 2, ty = r.y + r.h - 18;
    auto at = [&](float v) { return x0 + (std::clamp(v, lo, hi) - lo) / (hi - lo) * (x1 - x0); };
    char buf[48];
    snprintf(buf, sizeof(buf), signed_axis ? "%+.2f  to  %+.2f" : "%.2f  to  %.2f", double(range.min),
             double(range.max));
    regular_.draw(c, x1 - regular_.measure(buf, 19), r.y + 20, buf, 19, kMuted);
    c.fill_rect({x0, ty - 4, x1 - x0, 8}, kLine, 4);
    if (signed_axis)
        c.fill_rect({at(0) - 1, ty - 9, 2, 18}, kGrey);
    const Color fill = dim ? kGrey : kAccent;
    c.fill_rect({at(range.min), ty - 4, at(range.max) - at(range.min), 8}, fill, 4);
    const bool hover = hovered(r);
    for (float v : {range.min, range.max})
        c.fill_circle(at(v), ty, hover ? knob + 1 : knob, dim ? kMuted : kText);
    if (const auto v = signal(sig)) {
        c.fill_circle(at(*v), ty, 7, kBg);
        c.fill_circle(at(*v), ty, 5, kAmber);
    }
    Hit h{Hit::Fork, r};
    h.text = "range:" + pointer;
    h.data = {{"x0", x0}, {"x1", x1}, {"lo", lo}, {"hi", hi}};
    hits_.push_back(h);
}

void DashboardView::render_text_field(Canvas &c, Rect r, const std::string &id, const std::string &text,
                                      const std::string &placeholder) {
    c.fill_rect(r, hovered(r) ? kCardHover : kBg, 4);
    c.stroke_rect(r, hovered(r) ? kText : kMuted, 4, 2);
    const std::string edit = "Edit";
    const float ex = r.x + r.w - 18 - medium_.measure(edit, 22);
    medium_.draw(c, ex, r.y + (r.h + medium_.cap_height(22)) / 2, edit, 22, kAccentHover);
    const std::string &shown = text.empty() ? placeholder : text;
    regular_.draw(c, r.x + 18, r.y + (r.h + regular_.cap_height(24)) / 2, fit(regular_, shown, 24, ex - r.x - 40), 24,
                  text.empty() ? kMuted : kText);
    Hit h{Hit::EditText, r};   // Upstream's keyboard flow, which sends the typed text to the setting.
    h.text = id;
    hits_.push_back(h);
}

// Draws a dashboard setting row on one of these pages, as a switch or as segments.
void DashboardView::render_choice_row(Canvas &c, float y, float row_h, size_t index, const Choice &ch) {
    if (ch.boolean) {
        render_switch_row(c, y, index, ch, row_h);
        return;
    }
    const float W = float(c.width());
    const float d = (124 - row_h) / 2;
    const float tx = render_block(c, y, Icon::Rate, row_h);
    const float seg_w = 128, seg_h = 64, pad = 4;
    const float ctl_w = seg_w * float(ch.options.size()) + 2 * pad;
    const Rect ctl{W - kMargin - 16 - ctl_w, y + 26 - d, ctl_w, seg_h + 2 * pad};
    const float text_w = ctl.x - tx - 24;
    if (regular_.measure(ch.hint, 21) <= text_w) {
        medium_.draw(c, tx, y + 48 - d, fit(medium_, ch.label, 26, text_w), 26, kText);
        regular_.draw(c, tx, y + 83 - d, ch.hint, 21, kMuted);
    } else {   // Put a long hint on two lines, broken at the last space that fits.
        size_t cut = ch.hint.size();
        while (cut != std::string::npos && regular_.measure(ch.hint.substr(0, cut), 19) > text_w)
            cut = cut == 0 ? std::string::npos : ch.hint.rfind(' ', cut - 1);
        const std::string first = cut == std::string::npos ? "" : ch.hint.substr(0, cut);
        const std::string rest = cut == std::string::npos ? ch.hint : ch.hint.substr(cut + 1);
        medium_.draw(c, tx, y + 38 - d, fit(medium_, ch.label, 26, text_w), 26, kText);
        regular_.draw(c, tx, y + 67 - d, first, 19, kMuted);
        regular_.draw(c, tx, y + 90 - d, fit(regular_, rest, 19, text_w), 19, kMuted);
    }
    c.fill_rect(ctl, kPanel, 4);
    for (size_t k = 0; k < ch.options.size(); k++) {
        const double v = ch.options[k];
        const Rect seg{ctl.x + pad + float(k) * seg_w, ctl.y + pad, seg_w, seg_h};
        const bool selected = std::fabs(v - ch.value) < 1e-6;
        if (selected || hovered(seg))
            c.fill_rect(seg, selected ? kAccent : kCardHover, 3);
        char num[32];
        snprintf(num, sizeof(num), "%g", v);
        const std::string label = k < ch.option_labels.size() ? ch.option_labels[k] : num;
        medium_.draw(c, seg.x + (seg.w - medium_.measure(label, 24)) / 2, seg.y + (seg.h + medium_.cap_height(24)) / 2,
                     label, 24, selected ? kText : kMuted);
        hits_.push_back({Hit::Option, seg, index, v});
    }
}

void DashboardView::click_fork(const Hit &h, ClickResult &out, Point p) {
    const std::string &a = h.text;
    out.redraw = true;
    if (a == "page:eyes" || a == "page:squint" || a == "page:mouth") {
        restart_armed_until_ns_ = 0;
        page_ = a == "page:eyes" ? Page::Eyes : a == "page:squint" ? Page::Squint : Page::Mouth;
    } else if (a == "tab:eyes") {
        fork_.eyes_tab = int(h.value);
    } else if (a == "tab:mouth") {
        fork_.mouth_tab = int(h.value);
    } else if (a == "group") {
        fork_.shape_group = int(h.value);
        fork_.shape_page = 0;
    } else if (a == "shapes:prev" || a == "shapes:next") {
        fork_.shape_page = std::max(0, fork_.shape_page + (a == "shapes:next" ? 1 : -1));
    } else if (a.starts_with("set:")) {
        send(out, a.substr(4), h.data);
    } else if (a == "reset:min" || a == "reset:max") {
        // Each item is [pointer, lo, hi], and the other end of the range stays.
        for (const json &item : h.data) {
            const std::string ptr = item[0];
            const float lo = item[1], hi = item[2];
            ManualRange r{lo, hi};
            ManualRange::parse(setting(ptr, json::array({lo, hi})), &r);
            const json v = a == "reset:min" ? json::array({lo, r.max}) : json::array({r.min, hi});
            if (v != json::array({r.min, r.max}))
                send(out, ptr, v);
        }
    } else if (a == "squint:start") {
        // A new positive token starts the routine; the wall-clock milliseconds never repeat, even after a restart.
        send(out, "/sinks/squint/calibrate", int64_t(wall_ns() / 1'000'000ull));
    } else if (a == "squint:cancel") {
        send(out, "/sinks/squint/calibrate", 0);
    } else if (a.starts_with("range:")) {
        DragState &d = fork_.drag;
        d = {};
        d.kind = DragState::Kind::Range;
        d.pointer = a.substr(6);
        d.track = {h.data["x0"].get<float>(), 0, h.data["x1"].get<float>() - h.data["x0"].get<float>(), 0};
        d.lo = h.data["lo"];
        d.hi = h.data["hi"];
        ManualRange r{d.lo, d.hi};
        ManualRange::parse(setting(d.pointer, json::array({d.lo, d.hi})), &r);
        d.min = r.min;
        d.max = r.max;
        const float v = d.lo + std::clamp((p.x - d.track.x) / d.track.w, 0.f, 1.f) * (d.hi - d.lo);
        // Take the nearer handle, or the one on the side of the click if both are at one point.
        const float dm = std::fabs(v - d.min), dx = std::fabs(v - d.max);
        d.handle = dm < dx ? 0 : dx < dm ? 1 : v > d.max ? 1 : 0;
        drag(p);
    } else if (a.starts_with("slider:")) {
        DragState &d = fork_.drag;
        d = {};
        d.kind = DragState::Kind::Slider;
        d.pointer = a.substr(7);
        d.track = {h.data["x0"].get<float>(), 0, h.data["x1"].get<float>() - h.data["x0"].get<float>(), 0};
        d.lo = h.data["lo"];
        d.hi = h.data["hi"];
        d.step = h.data["step"];
        drag(p);
    } else if (a == "image:reset") {
        for (const json &item : h.data)
            if (setting(item[0].get<std::string>(), item[1]) != item[1])
                send(out, item[0].get<std::string>(), item[1]);
    } else if (a == "crop:move" || a == "crop:corner") {
        DragState &d = fork_.drag;
        d = {};
        d.kind = a == "crop:move" ? DragState::Kind::CropMove : DragState::Kind::CropCorner;
        d.corner = int(h.value);
        d.scale = h.data["scale"];
        d.img_w = h.data["w"];
        d.img_h = h.data["h"];
        d.bx = h.data["bx"];
        d.by = h.data["by"];
        d.bw = h.data["bw"];
        d.bh = h.data["bh"];
        d.square = h.data["sq"];
        d.px = p.x;
        d.py = p.y;
        d.tf.w = h.data["sw"];
        d.tf.h = h.data["sh"];
        d.tf.q = h.data["q"];
        d.tf.fh = h.data["fh"];
        d.tf.fv = h.data["fv"];
    } else if (a == "crop:whole") {
        send(out, "/pipelines/face/roi", json::array());
    } else if (a == "crop:square") {
        // Turning the lock on makes the box a square of its shorter side around its centre.
        const bool on = h.value > 0.5;
        send(out, "/pipelines/face/roi_square", on);
        const json roi = setting("/pipelines/face/roi", json::array());
        if (on && roi.is_array() && roi.size() == 4 && roi[2] != roi[3]) {
            const double x = roi[0], y = roi[1], w = roi[2], hh = roi[3], side = std::floor(std::min(w, hh));
            if (side > 0)
                send(out, "/pipelines/face/roi",
                     json::array({std::lround(x + (w - side) / 2), std::lround(y + (hh - side) / 2), long(side), long(side)}));
        }
    } else if (a == "open:cameras") {
        page_ = Page::Cameras;
        out.refresh_cameras = true;   // Show the cameras that are connected now.
    } else if (a == "open:modes") {
        page_ = Page::Resolution;
        fork_.mode_page = 0;
    } else if (a == "modes:prev" || a == "modes:next") {
        fork_.mode_page = std::max(0, fork_.mode_page + (a == "modes:next" ? 1 : -1));
    } else if (a == "mode" || a == "turn") {
        // The items are [pointer, value] and go out in one click, so the camera restarts once.
        for (const json &item : h.data)
            send(out, item[0].get<std::string>(), item[1]);
    }
}

bool DashboardView::drag(Point p) {
    DragState &d = fork_.drag;
    if (d.kind == DragState::Kind::Range) {
        const float v = d.lo + std::clamp((p.x - d.track.x) / d.track.w, 0.f, 1.f) * (d.hi - d.lo);
        if (d.handle == 0)
            d.min = std::min(v, d.max - kMinGap);
        else
            d.max = std::max(v, d.min + kMinGap);
        return true;
    }
    if (d.kind == DragState::Kind::Slider) {
        const float v = d.lo + std::clamp((p.x - d.track.x) / d.track.w, 0.f, 1.f) * (d.hi - d.lo);
        d.min = std::clamp(std::round(v / d.step) * d.step, d.lo, d.hi);
        return true;
    }
    if (d.kind == DragState::Kind::CropMove) {
        d.view = drag_move({d.bx, d.by, d.bw, d.bh}, (p.x - d.px) / d.scale, (p.y - d.py) / d.scale, d.img_w, d.img_h);
        return true;
    }
    if (d.kind == DragState::Kind::CropCorner) {
        d.view = drag_corner({d.bx, d.by, d.bw, d.bh}, d.corner, (p.x - d.px) / d.scale, (p.y - d.py) / d.scale, d.img_w,
                             d.img_h, d.square);
        return true;
    }
    return false;
}

DashboardView::ClickResult DashboardView::release(Point p) {
    ClickResult out;
    DragState &d = fork_.drag;
    if (d.kind == DragState::Kind::None)
        return out;
    drag(p);
    if (d.kind == DragState::Kind::Range) {
        ManualRange r{d.lo, d.hi};
        ManualRange::parse(setting(d.pointer, json::array({d.lo, d.hi})), &r);
        const json v = json::array({round2(d.min), round2(d.max)});
        if (v[0].get<float>() < v[1].get<float>() && v != json::array({round2(r.min), round2(r.max)}))
            send(out, d.pointer, v);
    } else if (d.kind == DragState::Kind::Slider) {
        const json old = setting(d.pointer, json());
        const json v = round2(d.min);
        if (!old.is_number() || std::fabs(old.get<float>() - d.min) > 1e-4f)
            send(out, d.pointer, v);
    } else if (d.view.w > 0) {   // The crop box moved; view holds it in the preview's pixels, so map it back.
        const Rect r = d.tf.box_to_source(d.view);
        const long x = std::clamp(std::lround(r.x), 0L, std::lround(d.tf.w) - 1);
        const long y = std::clamp(std::lround(r.y), 0L, std::lround(d.tf.h) - 1);
        long w = std::min(std::lround(r.w), std::lround(d.tf.w) - x), h = std::min(std::lround(r.h), std::lround(d.tf.h) - y);
        if (d.square)
            w = h = std::min(w, h);
        send(out, "/pipelines/face/roi", json::array({x, y, w, h}));
    }
    d = {};
    out.redraw = true;
    return out;
}

}
