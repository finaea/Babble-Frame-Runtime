// The Eyes page: manual ranges for the eyelids, squint, dilation and gaze, with the live value of each.
#include "ui/dashboard_view.h"

#include "ui/style.h"

namespace babble::ui {

using namespace style;
using nlohmann::json;

namespace {

struct EyeRow {
    const char *label;
    const char *base;     // The pointer without the signal name.
    const char *signal;   // The signal name without "Left" or "Right".
};
constexpr EyeRow kLidRows[] = {{"Openness", "/pipelines/eye/ranges/", "EyeLid"},
                               {"Widen", "/pipelines/eye/ranges/", "EyeWiden"},
                               {"Squint", "/sinks/squint/ranges/", "EyeSquint"},
                               {"Dilation", "/pipelines/eye/ranges/", "EyeDilation"}};
constexpr EyeRow kGazeRows[] = {{"Left / Right", "/pipelines/gaze/ranges/", "EyeX"},
                                {"Up / Down", "/pipelines/gaze/ranges/", "EyeY"}};

}

void DashboardView::render_eyes(Canvas &c, const std::vector<Choice> &choices) {
    const float W = float(c.width());
    render_back(c, "Eyes");
    const bool gaze = fork_.eyes_tab == 1;
    render_tabs(c, W - kMargin, {"Eyelids", "Gaze"}, fork_.eyes_tab, "tab:eyes");

    const char *hint = gaze ? "A narrower range reaches the edge with a smaller eye movement. The dot is the live value."
                            : "Raise the minimum to need a bigger movement. Lower the maximum to reach 100% sooner. "
                              "Dot: live value.";
    regular_.draw(c, kMargin, 150, fit(regular_, hint, 20, W - 2 * kMargin), 20, kMuted);

    const float label_w = 190, gap = 24;
    const float col_w = (W - 2 * kMargin - 24 - label_w - gap) / 2;
    const float col_x[2] = {kMargin + 20 + label_w, kMargin + 20 + label_w + col_w + gap};
    medium_.draw(c, col_x[0] + 15, 196, "Left Eye", 22, kText);
    medium_.draw(c, col_x[1] + 15, 196, "Right Eye", 22, kText);

    const bool signed_axis = gaze;
    const EyeRow *rows = gaze ? kGazeRows : kLidRows;
    const size_t n = gaze ? std::size(kGazeRows) : std::size(kLidRows);
    const float row_h = 84;
    float y = 206;
    json reset = json::array();
    for (size_t i = 0; i < n; i++) {
        const EyeRow &r = rows[i];
        c.fill_rect({kMargin, y + 4, W - 2 * kMargin, row_h - 8}, kCard, 6);
        medium_.draw(c, kMargin + 20, y + row_h / 2 + medium_.cap_height(24) / 2, r.label, 24, kText);
        for (int e = 0; e < 2; e++) {
            const std::string name = std::string(e == 0 ? "Left" : "Right") + r.signal;
            const std::string ptr = r.base + name;
            render_range(c, {col_x[e], y + 8, col_w, row_h - 16}, ptr, "_" + name + "Pre", signed_axis);
            reset.push_back({ptr, signed_axis ? -1 : 0, 1});
        }
        y += row_h;
    }
    // The dashboard rows of this tab, such as Blink Hold or Gaze Smoothing.
    for (size_t i = 0; i < choices.size(); i++)
        if (choices[i].page == (gaze ? "gaze" : "eyelids")) {
            render_choice_row(c, y, 96, i, choices[i]);
            y += 96;
        }
    if (gaze) {
        // Valve's own gaze for each eye, or SteamVR's combined gaze for both.
        c.fill_rect({kMargin, y + 4, W - 2 * kMargin, 80}, kCard, 6);
        medium_.draw(c, kMargin + 20, y + 44 + medium_.cap_height(26) / 2, "Gaze Source", 26, kText);
        render_value_segments(c, W - kMargin - 12, y + 8, "/pipelines/gaze/gaze_source", {"Per Eye", "Combined"},
                              {"independent", "combined"}, 150);
        y += 88;
    }

    const Rect max_btn = render_small_button(c, W - kMargin, kBottomY, "Reset Maximums", "reset:max");
    const Rect min_btn = render_small_button(c, max_btn.x - 16, kBottomY, "Reset Minimums", "reset:min");
    hits_[hits_.size() - 1].json = reset;
    hits_[hits_.size() - 2].json = reset;
    regular_.draw(c, 150, kBottomY + 39, fit(regular_, "Resets the ranges on this tab.", 20, min_btn.x - 32 - 150), 20,
                  kMuted);
}

}
