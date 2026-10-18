#pragma once

// The state of the Eyes, Squint calibration and Mouth pages of the Babble tab, which the fork adds.
#include "sink/frame_preview_sink.h"
#include "ui/canvas.h"

#include <nlohmann/json.hpp>

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

// What a laser drag on these pages moves.
struct DragState {
    enum class Kind { None, Range, CropMove, CropResize } kind = Kind::None;
    // For Range: the setting, the track, the value domain and the handle (0 min, 1 max).
    std::string pointer;
    Rect track;
    float lo = 0, hi = 1;
    int handle = 0;
    float min = 0, max = 1;
    // For the crop box: the view rect, its scale in canvas pixels per source pixel, the image size, the box in
    // source pixels at the start of the drag, and the pointer at the start.
    Rect view;
    float scale = 1;
    float img_w = 0, img_h = 0;
    float bx = 0, by = 0, bs = 0;
    float px = 0, py = 0;
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
