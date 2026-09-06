// Functional test for the Scroller component.
#include <gtest/gtest.h>

#include <functional>  // for function
#include <memory>      // for make_shared
#include <string>      // for string
#include <vector>      // for vector

#include <ftxui/component/component.hpp>  // for Renderer
#include <ftxui/component/event.hpp>      // for Event
#include <ftxui/dom/elements.hpp>         // for operator|, text, vbox, Element
#include <ftxui/dom/node.hpp>             // for Render (into Screen)
#include <ftxui/screen/screen.hpp>        // for Screen
#include <ftxui/util/ref.hpp>             // for Ref

#include "scroller.hpp"
#include "config.hpp"
#include "markdown.hpp"

namespace {

constexpr int kWidth = 20;
constexpr int kHeight = 8;   // viewport height H

// Build a component that renders `n` single-row text lines.
ftxui::Component MakeLines(int n) {
  return ftxui::Renderer([n] {
    std::vector<ftxui::Element> elements;
    for (int i = 0; i < n; ++i) {
      elements.push_back(ftxui::text("line-" + std::to_string(i)));
    }
    return ftxui::vbox(std::move(elements));
  });
}

// Wrap a child in a Scroller exercising the wrap-mode path with a real
// viewport width (kWidth), matching how the app drives it.
ftxui::Component WrapScroller(ftxui::Component child, int* selected,
                              int* viewport,
                              std::function<void(int, int)> on_change = {}) {
  return ftxui::Scroller(std::move(child), selected, viewport,
                         std::move(on_change), 0, kWidth, false);
}

// Render one frame so the component learns its content height / viewport.
void Prime(ftxui::Component& scroller) {
  ftxui::Screen screen(kWidth, kHeight);
  ftxui::Render(screen, scroller->Render());
}// Render the scroller's current output into a fresh screen and return the
// content of the top visible row (source line text occupying row 0).
std::string TopRow(ftxui::Component& scroller) {
  ftxui::Screen screen(kWidth, kHeight);
  ftxui::Render(screen, scroller->Render());
  std::string out = screen.ToString();
  auto nl = out.find('\n');
  if (nl == std::string::npos) {
    nl = out.size();
  }
  std::string row = out.substr(0, nl);
  // Drop trailing carriage return, padding spaces, and the vscroll_indicator
  // glyph (a box-drawing character occupying the rightmost column).
  while (!row.empty() &&
         (row.back() == ' ' || row.back() == '\r' ||
          static_cast<unsigned char>(row.back()) >= 0x80)) {
    row.pop_back();
  }
  return row;
}

// Render the scroller's current output and count non-blank trimmed rows.
int VisibleRows(ftxui::Component& scroller) {
  ftxui::Screen screen(kWidth, kHeight);
  ftxui::Render(screen, scroller->Render());
  std::string out = screen.ToString();
  int rows = 0;
  std::string row;
  for (char c : out) {
    if (c == '\n' || c == '\r') {
      while (!row.empty() && row.back() == ' ') {
        row.pop_back();
      }
      if (!row.empty()) {
        ++rows;
      }
      row.clear();
    } else {
      row += c;
    }
  }
  while (!row.empty() && row.back() == ' ') {
    row.pop_back();
  }
  if (!row.empty()) {
    ++rows;
  }
  return rows;
}

// Extract the integer from a "line-N" row.
int LineNumber(const std::string& row) {
  auto pos = row.find("line-");
  return pos == std::string::npos ? -1 : std::stoi(row.substr(pos + 5));
}

}  // namespace

// Each `j` press advances the view offset by exactly one.
TEST(Scroller, DownAdvancesOnePerPress) {
  int n = 60;
  int selected = 0;
  int viewport = kHeight;
  auto scroller = WrapScroller(MakeLines(n), &selected, &viewport);
  Prime(scroller);

  for (int i = 1; i <= 40; ++i) {
    ASSERT_TRUE(scroller->OnEvent(ftxui::Event::j));
    EXPECT_EQ(selected, i);
  }
}

// `k` moves back by one and clamps at the top (returns false when unchanged).
TEST(Scroller, UpMovesOneAndClampsAtTop) {
  int selected = 5;
  int viewport = kHeight;
  auto scroller = WrapScroller(MakeLines(60), &selected, &viewport);
  Prime(scroller);

  ASSERT_TRUE(scroller->OnEvent(ftxui::Event::k));
  EXPECT_EQ(selected, 4);

  selected = 0;
  ASSERT_FALSE(scroller->OnEvent(ftxui::Event::k));
  EXPECT_EQ(selected, 0);
}

// The view is top-anchored: from the very first press, each `j` moves the
// topmost visible line down by exactly one. This is the regression the former
// cursor-anchored (centered) scroller failed: it needed ~half a viewport of
// presses before anything visibly moved at the top of the file.
TEST(Scroller, ViewScrollsOneLinePerPress) {
  int n = 60;
  int selected = 0;
  int viewport = kHeight;
  auto scroller = WrapScroller(MakeLines(n), &selected, &viewport);
  Prime(scroller);

  // First press already scrolls: line-0 becomes line-1.
  ASSERT_TRUE(scroller->OnEvent(ftxui::Event::j));
  EXPECT_EQ(TopRow(scroller), "line-1");

  // Every subsequent press scrolls exactly one more line, immediately.
  for (int s = 1; s <= 20; ++s) {
    EXPECT_EQ(LineNumber(TopRow(scroller)), s)
        << "top row after " << s << " presses";
    scroller->OnEvent(ftxui::Event::j);
    EXPECT_EQ(selected, s + 1);
  }
}

// PageDown / PageUp move by the viewport height.
TEST(Scroller, PageScrollsByViewportHeight) {
  int n = 60;
  int selected = 0;
  int viewport = kHeight;
  auto scroller = WrapScroller(MakeLines(n), &selected, &viewport);
  Prime(scroller);

  ASSERT_TRUE(scroller->OnEvent(ftxui::Event::PageDown));
  EXPECT_EQ(selected, kHeight - 1);  // page steps by viewport height - 1

  selected = 30;
  ASSERT_TRUE(scroller->OnEvent(ftxui::Event::PageUp));
  EXPECT_EQ(selected, 30 - (kHeight - 1));
}

// Home jumps to the first line; End to the last possible scroll offset (the
// line placed at the top when the viewport is filled to the bottom).
TEST(Scroller, HomeEnd) {
  int n = 60;
  int selected = 30;
  int viewport = kHeight;
  auto scroller = WrapScroller(MakeLines(n), &selected, &viewport);
  Prime(scroller);

  ASSERT_TRUE(scroller->OnEvent(ftxui::Event::Home));
  EXPECT_EQ(selected, 0);

  ASSERT_TRUE(scroller->OnEvent(ftxui::Event::End));
  EXPECT_EQ(selected, n - kHeight);  // max offset = content - viewport

  // No change at the bounds.
  ASSERT_FALSE(scroller->OnEvent(ftxui::Event::End));
  EXPECT_EQ(selected, n - kHeight);
}

// The on_change callback fires with before/after on every transition.
TEST(Scroller, OnChangeCallback) {
  int selected = 0;
  int viewport = kHeight;
  std::vector<std::pair<int, int>> calls;
  auto scroller = WrapScroller(MakeLines(60), &selected, &viewport,
                               [&](int before, int after) {
                                 calls.emplace_back(before, after);
                               });
  Prime(scroller);

  scroller->OnEvent(ftxui::Event::j);
  scroller->OnEvent(ftxui::Event::j);

  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0], std::make_pair(0, 1));
  EXPECT_EQ(calls[1], std::make_pair(1, 2));
}

// When the viewport is at least as tall as the content, nothing can scroll:
// every navigation is a no-op and offsets clamp to 0.
TEST(Scroller, NoScrollWhenContentFitsViewport) {
  int n = 4;  // fewer lines than the 8-row viewport
  int selected = 0;
  int viewport = kHeight;
  auto scroller = WrapScroller(MakeLines(n), &selected, &viewport);
  Prime(scroller);

  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::j));
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::k));
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::PageDown));
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::PageUp));
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::End));
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::Home));
  EXPECT_EQ(selected, 0);
}

// An event arriving before the first render must be safely ignored rather
// than computing a bogus max_offset from unitialized content height.
TEST(Scroller, EventBeforeFirstRenderIsIgnored) {
  int selected = 0;
  int viewport = kHeight;
  auto scroller = WrapScroller(MakeLines(60), &selected, &viewport);

  // No Prime() call: the component has not measured content yet.
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::j));
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::PageDown));
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::End));
  EXPECT_EQ(selected, 0);
}

namespace {

// A component that renders lines wider than the viewport (60 columns vs a
// 20-column screen), tall enough that vertical scrolling is active.
ftxui::Component MakeWideContent() {
  return ftxui::Renderer([] {
    std::vector<ftxui::Element> elements;
    for (int i = 0; i < 12; ++i) {
      elements.push_back(ftxui::text(std::string(60, 'x')));
    }
    elements.push_back(ftxui::text("short"));
    return ftxui::vbox(std::move(elements));
  });
}

}  // namespace

// With horizontal panning enabled, ArrowRight/ArrowLeft (and l/h) move the
// horizontal offset within [0, content_width - viewport_width].
TEST(Scroller, HorizontalPanClamps) {
  int selected = 0;
  int viewport_height = kHeight;
  int selected_x = 0;
  int viewport_width = kWidth;
  bool hscroll = true;
  auto scroller = ftxui::Scroller(MakeWideContent(), &selected, &viewport_height,
                                  {}, &selected_x, &viewport_width, &hscroll);
  Prime(scroller);

  ASSERT_TRUE(scroller->OnEvent(ftxui::Event::ArrowRight));
  EXPECT_EQ(selected_x, 1);
  ASSERT_TRUE(scroller->OnEvent(ftxui::Event::Character('l')));
  EXPECT_EQ(selected_x, 2);

  // max_x_offset = 60 - 20 = 40; further presses clamp instead of moving.
  selected_x = 40;
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::ArrowRight));
  EXPECT_EQ(selected_x, 40);

  ASSERT_TRUE(scroller->OnEvent(ftxui::Event::ArrowLeft));
  EXPECT_EQ(selected_x, 39);
  ASSERT_TRUE(scroller->OnEvent(ftxui::Event::Character('h')));
  EXPECT_EQ(selected_x, 38);

  // Vertical navigation is unaffected by horizontal panning.
  ASSERT_TRUE(scroller->OnEvent(ftxui::Event::j));
  EXPECT_EQ(selected, 1);
}

// When horizontal panning is disabled (wrap mode), the horizontal keys are
// not consumed and the offset never moves.
TEST(Scroller, HorizontalKeysInactiveInWrapMode) {
  int selected = 0;
  int viewport_height = kHeight;
  int selected_x = 0;
  int viewport_width = kWidth;
  bool hscroll = false;
  auto scroller = ftxui::Scroller(MakeWideContent(), &selected, &viewport_height,
                                  {}, &selected_x, &viewport_width, &hscroll);
  Prime(scroller);

  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::ArrowRight));
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::ArrowLeft));
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::Character('l')));
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::Character('h')));
  EXPECT_EQ(selected_x, 0);
  EXPECT_EQ(selected, 0);
}

// In wrap mode with a real viewport width, long code/HTML lines must reflow
// (the content keeps width-constrained layout instead of being laid out at
// its natural width and clipped). A single long line inside a bordered box
// renders as several rows.
TEST(Scroller, WrapModeReflowsLongCodeLines) {
  const char* md =
      "```\nsome very long code line with many words beyond the narrow "
      "viewport width that must wrap\n```\n";
  int selected = 0;
  int viewport_height = kHeight;
  int selected_x = 0;
  int viewport_width = kWidth;
  bool hscroll = false;
  markit::Config cfg;
  auto scroller = ftxui::Scroller(
      ftxui::Renderer(
          [&md, &cfg] { return markit::RenderMarkdown(md, cfg); }),
      &selected, &viewport_height, {}, &selected_x, &viewport_width, &hscroll);
  Prime(scroller);
  EXPECT_GT(VisibleRows(scroller), 3);
}

// A wrap -> scroll -> wrap round-trip at the same viewport width (mirroring
// the app's `w` toggle, which also re-renders the content tree per mode)
// keeps valid scroll bounds: the wrapped height matches the wrap tree.
TEST(Scroller, WrapSurvivesModeToggleAtSameWidth) {
  const char* md =
      "```\nsome very long code line with many words beyond the narrow "
      "viewport width that must wrap\n"
      "a second long code line with many words beyond the narrow viewport\n"
      "a third long code line with many words beyond the narrow viewport\n"
      "```\n";
  int selected = 0;
  int viewport_height = kHeight;
  int selected_x = 0;
  int viewport_width = kWidth;
  bool hscroll = false;
  markit::Config cfg;
  auto scroller = ftxui::Scroller(
      ftxui::Renderer(
          [&md, &cfg] { return markit::RenderMarkdown(md, cfg); }),
      &selected, &viewport_height, {}, &selected_x, &viewport_width, &hscroll);
  Prime(scroller);
  const int wrap_rows = VisibleRows(scroller);
  ASSERT_GT(wrap_rows, 3);

  hscroll = true;  // scroll mode: single wide rows.
  cfg.horizontal_wrap = markit::WrapMode::Scroll;
  Prime(scroller);

  hscroll = false;  // back to wrap at the same width.
  cfg.horizontal_wrap = markit::WrapMode::Wrap;
  Prime(scroller);
  EXPECT_EQ(VisibleRows(scroller), wrap_rows);

  // Navigation still clamps to the re-measured content.
  EXPECT_TRUE(scroller->OnEvent(ftxui::Event::End));
  EXPECT_GE(selected, 0);
  EXPECT_TRUE(scroller->OnEvent(ftxui::Event::Home));
  EXPECT_EQ(selected, 0);
}
