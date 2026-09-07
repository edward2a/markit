// Screen chrome for markit: status bar, action bar, navigation bar.
//
// The builders are pure functions over plain values (no component state),
// so the static milestone is unit-testable without a terminal.
#ifndef MARKIT_CHROME_HPP
#define MARKIT_CHROME_HPP

#include <string>  // for string
#include <vector>  // for vector

#include <ftxui/dom/elements.hpp>  // for Element

#include "config.hpp"  // for WrapMode

namespace markit {

// Fixed nav-bar width in columns (the separator beside it adds one more).
inline constexpr int kNavWidth = 30;

// A document heading: 1-based level + plain text (formatting stripped).
struct Heading {
  int level = 1;
  std::string text;
};

// Collect the document headings in order via a lightweight md4c parse.
std::vector<Heading> ExtractHeadings(const std::string& markdown);

// Pure formatter for the status bar right side, e.g. "12/120  10%  wrap".
// Counts are scroll positions: current = selected+1, total = max_offset+1.
std::string FormatPosition(int selected, int max_offset, WrapMode mode);

// Bottom row of the content column: filename (left) + position/mode (right).
ftxui::Element StatusBar(const std::string& filename, int selected,
                         int max_offset, WrapMode mode);

// Row above the status bar: static key-binding hints.
ftxui::Element ActionBar();

// Right-side full-height panel: "Outline" title + one row per heading.
ftxui::Element NavBar(const std::vector<Heading>& headings);

}  // namespace markit

#endif  // MARKIT_CHROME_HPP
