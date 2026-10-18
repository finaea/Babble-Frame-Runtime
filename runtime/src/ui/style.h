#pragma once

// The palette and sizes of dashboard_view.cpp, for the pages that the fork adds.
#include "ui/canvas.h"

namespace babble::ui::style {

inline constexpr Color kBg = Color::hex(0x181818);
inline constexpr Color kPanel = Color::hex(0x202020);
inline constexpr Color kCard = Color::hex(0x292929);
inline constexpr Color kCardHover = Color::hex(0x343434);
inline constexpr Color kLine = Color::hex(0x343434);
inline constexpr Color kText = Color::hex(0xffffff);
inline constexpr Color kMuted = Color::hex(0x9d9d9d);
inline constexpr Color kAccent = Color::hex(0x0073cf);
inline constexpr Color kAccentHover = Color::hex(0x1684db);
inline constexpr Color kGreen = Color::hex(0x3fcf73);
inline constexpr Color kAmber = Color::hex(0xf2b33d);
inline constexpr Color kRed = Color::hex(0xf0544f);
inline constexpr Color kGrey = Color::hex(0x6b6b6b);

inline constexpr float kMargin = 56;
// The bottom row, beside the cog, where the pages put their buttons.
inline constexpr float kBottomY = 650;

}
