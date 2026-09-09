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

// Row above the status bar: key-binding hints, switching to the nav set
// while the nav bar has focus.
ftxui::Element ActionBar(bool nav_focused = false);

// Right-side full-height panel: "Outline" title + one row per heading.
// `current` is the index into `headings` of the section in view (-1 for
// none, e.g. preamble or documents without headings); that row renders
// bold in the accent color. Out-of-range values highlight nothing.
// `cursor` is the keyboard-selected index, rendered inverted only while
// `focused` is true (independent of `current`); `focused` also inverts
// the title so the focus is visible.
ftxui::Element NavBar(const std::vector<Heading>& headings, int current = -1,
                      int cursor = -1, bool focused = false);

// Clamp a nav list offset (first visible heading) to its valid range for
// `count` headings with `visible` rows on screen.
int ClampNavOffset(int offset, int count, int visible);

// Shift `offset` minimally so `cursor` is visible in a `visible`-row
// window, then clamp. Pure follow-scroll for the nav keyboard cursor.
int FollowNavOffset(int offset, int cursor, int count, int visible);

}  // namespace markit

#endif  // MARKIT_CHROME_HPP
