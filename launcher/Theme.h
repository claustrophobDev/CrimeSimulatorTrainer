#pragma once

#include <Windows.h>

#include "Canvas.h"

namespace theme {

// Нейтральный графит вместо сине-чёрного, который лепят во все тёмные темы.
// Янтарный - единственный акцент, он значит "включено". Зелёный и красный
// заняты состоянием подключения, чтобы смыслы не путались.
inline constexpr Color kVoid = MakeColor(0x07, 0x07, 0x08);
inline constexpr Color kPanel = MakeColor(0x10, 0x11, 0x14);
inline constexpr Color kPanelTop = MakeColor(0x15, 0x17, 0x1B);
inline constexpr Color kRaise = MakeColor(0x16, 0x18, 0x1D);
inline constexpr Color kRowHover = MakeColor(0x1B, 0x1E, 0x24);
inline constexpr Color kLine = MakeColor(0x25, 0x29, 0x30);
inline constexpr Color kLineSoft = MakeColor(0x1B, 0x1E, 0x23);
inline constexpr Color kTrack = MakeColor(0x2C, 0x31, 0x39);

inline constexpr Color kText = MakeColor(0xEC, 0xEE, 0xF1);
inline constexpr Color kTextDim = MakeColor(0x8C, 0x93, 0x9D);
inline constexpr Color kTextFaint = MakeColor(0x5A, 0x60, 0x6B);

inline constexpr Color kAccent = MakeColor(0xE2, 0xA3, 0x3A);
inline constexpr Color kAccentDeep = MakeColor(0x8A, 0x62, 0x1E);
inline constexpr Color kAccentWash = MakeColor(0x24, 0x1D, 0x11);
inline constexpr Color kLive = MakeColor(0x4F, 0xD0, 0x8A);
inline constexpr Color kWarn = MakeColor(0xE5, 0x8A, 0x3C);
inline constexpr Color kAlert = MakeColor(0xE0, 0x55, 0x61);
inline constexpr Color kKnob = MakeColor(0xFA, 0xFB, 0xFC);

// Размеры в логических пикселях при 96 dpi. В рантайме всё умножается на
// масштаб, так что экранных координат тут нет.
namespace metric {
inline constexpr float kShadow = 22.0f;
inline constexpr float kPanelWidth = 580.0f;
inline constexpr float kPanelRadius = 14.0f;
inline constexpr float kPad = 22.0f;

inline constexpr float kHeaderHeight = 54.0f;
inline constexpr float kStatusHeight = 88.0f;
inline constexpr float kGroupHeader = 26.0f;
inline constexpr float kRowHeight = 46.0f;
inline constexpr float kGroupGap = 6.0f;
inline constexpr float kFooterHeight = 60.0f;

inline constexpr float kTrackWidth = 44.0f;
inline constexpr float kTrackHeight = 24.0f;
inline constexpr float kKnobDiameter = 18.0f;
} // namespace metric

} // namespace theme
