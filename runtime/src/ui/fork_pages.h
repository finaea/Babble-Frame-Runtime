#pragma once

// The state of the Eyes, Squint calibration and Mouth pages of the Babble tab, which the fork adds.
#include "sink/frame_preview_sink.h"
#include "ui/canvas.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace babble::ui {

// One ExpressionState value, where recent is false if it is older than 1 s.
struct LiveSignal {
    float value = 0;
    bool recent = false;
};

// The live data that the dashboard thread gives the view before it draws one of these pages.
struct PageData {
    std::shared_ptr<const nlohmann::json> config;   // The current config, or null if the runtime sent none.
    std::map<std::string, LiveSignal> signals;      // Every signal that has a value, by name.
    std::shared_ptr<const PreviewImage> preview;    // The newest face frame while the Mouth page is open.
};

// The Mouth preview shows the face frame flipped and rotated as the model sees it (BabbleMonoFace::prepare: flip h,
// flip v, then counter-clockwise quarter turns), while the crop box is stored in source pixels. This maps points and
// boxes between the two. A box maps to a box, so cropping first and turning after gives the same pixels.
struct PreviewTransform {
    float w = 0, h = 0;   // The source frame size.
    int q = 0;            // Counter-clockwise quarter turns, 0-3.
    bool fh = false, fv = false;

    float view_w() const { return q % 2 ? h : w; }
    float view_h() const { return q % 2 ? w : h; }
    std::pair<float, float> to_source(float u, float v) const {
        float fx = u, fy = v;
        switch (q) {
        case 1: fx = w - v; fy = u; break;
        case 2: fx = w - u; fy = h - v; break;
        case 3: fx = v; fy = h - u; break;
        default: break;
        }
        return {fh ? w - fx : fx, fv ? h - fy : fy};
    }
    std::pair<float, float> to_view(float x, float y) const {
        const float fx = fh ? w - x : x, fy = fv ? h - y : y;
        switch (q) {
        case 1: return {fy, w - fx};
        case 2: return {w - fx, h - fy};
        case 3: return {h - fy, fx};
        default: return {fx, fy};
        }
    }
    Rect box_to_view(Rect r) const { return span(to_view(r.x, r.y), to_view(r.x + r.w, r.y + r.h)); }
    Rect box_to_source(Rect r) const { return span(to_source(r.x, r.y), to_source(r.x + r.w, r.y + r.h)); }

private:
    static Rect span(std::pair<float, float> a, std::pair<float, float> b) {
        const float x0 = std::min(a.first, b.first), y0 = std::min(a.second, b.second);
        return {x0, y0, std::max(a.first, b.first) - x0, std::max(a.second, b.second) - y0};
    }
};

// What a laser drag on these pages moves.
struct DragState {
    enum class Kind { None, Range, CropMove, CropResize } kind = Kind::None;
    // For Range: the setting, the track, the value domain and the handle (0 min, 1 max).
    std::string pointer;
    Rect track;
    float lo = 0, hi = 1;
    int handle = 0;
    float min = 0, max = 1;
    // For the crop box, all in the preview's (flipped and rotated) pixels: the box, the scale in canvas pixels per
    // pixel, the image size, the box at the start of the drag, and the pointer at the start. tf maps them back to
    // source pixels.
    Rect view;
    float scale = 1;
    float img_w = 0, img_h = 0;
    float bx = 0, by = 0, bs = 0;
    float px = 0, py = 0;
    PreviewTransform tf;
};

struct ForkPages {
    PageData data;
    DragState drag;
    // Values that this view sent and that the config does not show yet, with the time of the change.
    std::map<std::string, std::pair<nlohmann::json, uint64_t>> pending;
    int eyes_tab = 0;      // 0 eyelids, 1 gaze.
    int mouth_tab = 0;     // 0 camera, 1 shapes.
    int shape_group = 0;   // jaw, mouth, cheek, nose, tongue.
    int shape_page = 0;
};

}
