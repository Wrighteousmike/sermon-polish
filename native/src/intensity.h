#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>

namespace sermon {

// How strongly a stage works, as one continuous amount from 0 to 1 (the UI shows 0-100).
//
//   0.00  Off (the stage is bypassed; callers use an empty std::optional for this)
//   0.25  "Less"    }  the three presets the app shipped with: they are fixed points on the
//   0.50  "Normal"  }  scale, so a recording that used to be set to Normal sounds the same now.
//   0.75  "More"    }
//   1.00  "Max"       past the old More; each stage's curve decides how far that goes.
//
// Everything between the points is interpolated by the stage's own macro curve (see amount.h), which
// moves all of a stage's underlying parameters together.
struct Intensity {
  double amount = 0.5;

  constexpr Intensity() = default;
  constexpr explicit Intensity(double a) : amount(a < 0 ? 0 : a > 1 ? 1 : a) {}

  static const Intensity Less, Normal, More, Max;

  friend constexpr bool operator==(Intensity a, Intensity b) { return a.amount == b.amount; }
  friend constexpr bool operator!=(Intensity a, Intensity b) { return a.amount != b.amount; }
};

inline constexpr Intensity Intensity::Less{0.25};
inline constexpr Intensity Intensity::Normal{0.5};
inline constexpr Intensity Intensity::More{0.75};
inline constexpr Intensity Intensity::Max{1.0};

// "off", "less", "normal", "more", "max", or a number 0-100 (0 is off). Returns false if the text is
// none of these; `out` is then left alone.
inline bool parseIntensity(const std::string& v, std::optional<Intensity>& out) {
  if (v == "off") { out = std::nullopt; return true; }
  if (v == "less") { out = Intensity::Less; return true; }
  if (v == "normal") { out = Intensity::Normal; return true; }
  if (v == "more") { out = Intensity::More; return true; }
  if (v == "max") { out = Intensity::Max; return true; }
  if (v.empty()) return false;
  char* end = nullptr;
  const double pct = std::strtod(v.c_str(), &end);
  if (end == v.c_str() || *end != '\0' || !std::isfinite(pct)) return false;
  if (pct <= 0) out = std::nullopt;
  else out = Intensity(pct / 100.0);
  return true;
}

}  // namespace sermon
