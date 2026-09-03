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
  auto scroller = ftxui::Scroller(MakeLines(n), &selected, &viewport);
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
  auto scroller = ftxui::Scroller(MakeLines(60), &selected, &viewport);
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
  auto scroller = ftxui::Scroller(MakeLines(n), &selected, &viewport);
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
  auto scroller = ftxui::Scroller(MakeLines(n), &selected, &viewport);
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
  auto scroller = ftxui::Scroller(MakeLines(n), &selected, &viewport);
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
  auto scroller = ftxui::Scroller(
      MakeLines(60), &selected, &viewport,
      [&](int before, int after) { calls.emplace_back(before, after); });
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
  auto scroller = ftxui::Scroller(MakeLines(n), &selected, &viewport);
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
  auto scroller = ftxui::Scroller(MakeLines(60), &selected, &viewport);

  // No Prime() call: the component has not measured content yet.
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::j));
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::PageDown));
  EXPECT_FALSE(scroller->OnEvent(ftxui::Event::End));
  EXPECT_EQ(selected, 0);
}
