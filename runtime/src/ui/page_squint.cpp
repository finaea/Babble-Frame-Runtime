// The Squint calibration page: the status of eyecam's engine, the guided routine and its result.
#include "ui/dashboard_view.h"

#include "ui/style.h"

#include <cmath>
#include <cstdio>

namespace babble::ui {

using namespace style;

namespace {

// The prompts for _SquintCalStep, with a short instruction under each.
constexpr const char *kStepPrompt[] = {"Get ready", "Close your eyes", "Look normally", "Open wide",
                                       "Squint hard", "Look up", "Look down"};
constexpr const char *kStepHint[] = {"Keep your head still and look straight ahead.",
                                     "Close both eyes gently and keep them closed.",
                                     "Look straight ahead with relaxed eyes.",
                                     "Open your eyes as wide as you can.",
                                     "Squint as hard as you can.",
                                     "Look up with your eyes. Keep your head still.",
                                     "Look down with your eyes. Keep your head still."};
// The reasons for _SquintCalFail.
constexpr const char *kFail[] = {"", "Not enough usable frames. Keep the headset steady and try again.",
                                 "The squint was not deep enough. Squint harder next time.",
                                 "The look-down check failed. Look down with the eyes, not the head.",
                                 "Something went wrong. Try again."};

}

void DashboardView::render_squint(Canvas &c, const std::vector<Choice> &choices) {
    const float W = float(c.width());
    render_back(c, "Squint Calibration");
    const auto state = signal("_SquintCalState", true);
    const int st = state ? int(std::lround(*state)) : -1;

    if (st == 1 || st == 2) {   // The routine is running: one big prompt.
        const Rect card{kMargin, 150, W - 2 * kMargin, 450};
        c.fill_rect(card, kPanel, 6);
        const int step = std::clamp(int(std::lround(signal("_SquintCalStep", true).value_or(0))), 0, 6);
        const std::string prompt = st == 2 ? "Fitting" : kStepPrompt[step];
        const std::string hint = st == 2 ? "Working out the squint depth. This takes a moment." : kStepHint[step];
        medium_.draw(c, (W - medium_.measure(prompt, 72)) / 2, card.y + 190, prompt, 72, kText);
        regular_.draw(c, (W - regular_.measure(hint, 28)) / 2, card.y + 250, hint, 28, kMuted);
        if (st == 1) {
            const float left = std::max(0.f, signal("_SquintCalSecondsLeft", true).value_or(0));
            char buf[16];
            snprintf(buf, sizeof(buf), "%.0f", std::ceil(left));
            medium_.draw(c, (W - medium_.measure(buf, 96)) / 2, card.y + 390, buf, 96, kAccentHover);
        }
        render_small_button(c, W - kMargin, kBottomY, "Cancel", "squint:cancel");
        return;
    }

    const bool running = st >= 0;
    const bool ready = signal("_SquintReady", true).value_or(0) > 0.5f;
    const bool calibrated = signal("_SquintCalibrated", true).value_or(0) > 0.5f;
    const bool imported = signal("_SquintCalImported", true).value_or(0) > 0.5f;
    constexpr float kRow = 112, d = (124 - kRow) / 2;
    float y = 160;
    auto row = [&](const char *title, const std::string &detail, Color dot) {
        c.fill_rect({kMargin, y + 4, W - 2 * kMargin, kRow - 12}, kCard, 6);
        c.fill_circle(kMargin + 40, y + kRow / 2 - 2, 10, dot);
        medium_.draw(c, kMargin + 80, y + 48 - d, title, 26, kText);
        return [&, detail](float right) {
            regular_.draw(c, kMargin + 80, y + 83 - d, fit(regular_, detail, 21, right - kMargin - 104), 21, kMuted);
            y += kRow;
        };
    };

    // Calibration: the Start button is on this row.
    {
        const std::string detail = !running   ? "The squint engine isn't running. Turn Squint on below."
                                   : imported ? "Imported from eyecam. Redo it once here for the best result."
                                   : calibrated ? "Calibrated. Redo it if squint looks too weak or too strong."
                                                : "Not calibrated yet. Squint needs this once.";
        auto finish = row("Calibration", detail, !running ? kGrey : calibrated && !imported ? kGreen : kAmber);
        float right = W - kMargin - 16;
        if (running)
            right = render_small_button(c, right, y + 28 - d, calibrated ? "Redo" : "Start", "squint:start", true).x;
        finish(right);
    }
    if (running) {
        const std::string detail = ready ? "Ready. The relaxed levels of this wear are learned."
                                         : "Learning the relaxed levels of this wear. Wear the headset for about 30 s.";
        row("Engine", detail, ready ? kGreen : kAmber)(W - kMargin);
    }
    if (st == 3 || st == 4) {   // The result of the last run in this session.
        std::string detail;
        if (st == 3) {
            char buf[96];
            snprintf(buf, sizeof(buf), "Done. Squint depth: left %.2f, right %.2f.",
                     double(signal("_LeftEyeSquintFsq", true).value_or(0)),
                     double(signal("_RightEyeSquintFsq", true).value_or(0)));
            detail = buf;
        } else {
            detail = kFail[std::clamp(int(std::lround(signal("_SquintCalFail", true).value_or(4))), 1, 4)];
        }
        row("Last Run", detail, st == 3 ? kGreen : kRed)(W - kMargin);
    }
    for (size_t i = 0; i < choices.size(); i++)
        if (choices[i].page == "squint") {
            render_choice_row(c, y, kRow, i, choices[i]);
            y += kRow;
        }
    regular_.draw(c, 150, kBottomY + 39,
                  fit(regular_, "The routine takes about 30 s: close, look normally, squint, then look up and down. "
                                "Keep the head still.", 20, W - kMargin - 150),
                  20, kMuted);
}

}
