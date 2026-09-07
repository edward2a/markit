// Tests for the screen chrome builders (src/chrome.cpp).
#include "chrome.hpp"

#include <string>  // for string
#include <vector>  // for vector

#include <ftxui/dom/elements.hpp>  // for Element
#include <ftxui/screen/screen.hpp>  // for Screen, Dimension
#include <gtest/gtest.h>  // for TEST, EXPECT_*

namespace {

// Render an element to left-trimmed? No: keep full rows, tests check
// containment. Wide CJK/fullwidth chars are avoided by the builders.
std::vector<std::string> RenderToStrings(ftxui::Element element, int width,
                                         int height) {
  ftxui::Screen screen =
      ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                            ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, std::move(element));
  std::vector<std::string> rows;
  for (int y = 0; y < height; ++y) {
    std::string line;
    for (int x = 0; x < width; ++x) {
      line += screen.CellAt(x, y).character;
    }
    rows.push_back(line);
  }
  return rows;
}

bool AnyLineContains(const std::vector<std::string>& rows,
                     const std::string& needle) {
  for (const auto& row : rows) {
    if (row.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

// Headings come back in document order with levels and plain text.
TEST(Chrome, ExtractHeadingsOrderLevelsAndText) {
  const std::vector<markit::Heading> headings = markit::ExtractHeadings(
      "# Top\n\ntext\n\n## Mid **bold**\n\n### Deep `code`\n");
  ASSERT_EQ(headings.size(), 3u);
  EXPECT_EQ(headings[0].level, 1);
  EXPECT_EQ(headings[0].text, "Top");
  EXPECT_EQ(headings[1].level, 2);
  EXPECT_EQ(headings[1].text, "Mid bold");
  EXPECT_EQ(headings[2].level, 3);
  EXPECT_EQ(headings[2].text, "Deep code");
}

TEST(Chrome, ExtractHeadingsEmptyWhenNone) {
  EXPECT_TRUE(markit::ExtractHeadings("just a paragraph\n").empty());
  EXPECT_TRUE(markit::ExtractHeadings("").empty());
}

// Position formatting: current/total, rounded percent, mode name.
TEST(Chrome, FormatPosition) {
  using markit::WrapMode;
  EXPECT_EQ(markit::FormatPosition(0, 0, WrapMode::Wrap), "1/1  100%  wrap");
  EXPECT_EQ(markit::FormatPosition(0, 99, WrapMode::Wrap), "1/100  0%  wrap");
  EXPECT_EQ(markit::FormatPosition(99, 99, WrapMode::Scroll),
            "100/100  100%  scroll");
  EXPECT_EQ(markit::FormatPosition(1, 2, WrapMode::Wrap), "2/3  50%  wrap");
  // Out-of-range inputs clamp instead of printing nonsense.
  EXPECT_EQ(markit::FormatPosition(50, 10, WrapMode::Wrap),
            "11/11  100%  wrap");
  EXPECT_EQ(markit::FormatPosition(-3, 10, WrapMode::Wrap), "1/11  0%  wrap");
}

// The status bar shows the basename plus position and mode.
TEST(Chrome, StatusBarShowsFilePositionAndMode) {
  const auto rows =
      RenderToStrings(markit::StatusBar("/a/b/notes.md", 0, 9,
                                        markit::WrapMode::Scroll),
                      60, 1);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_TRUE(AnyLineContains(rows, "notes.md"));
  EXPECT_TRUE(AnyLineContains(rows, "1/10"));
  EXPECT_TRUE(AnyLineContains(rows, "scroll"));
}

// The action bar is a non-empty single row naming the main keys.
TEST(Chrome, ActionBarListsKeys) {
  const auto rows = RenderToStrings(markit::ActionBar(), 100, 1);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_TRUE(AnyLineContains(rows, "q:quit"));
  EXPECT_TRUE(AnyLineContains(rows, "w:wrap/scroll"));
  EXPECT_TRUE(AnyLineContains(rows, "n:nav"));
}

// The nav bar lists the headings under an "Outline" title.
TEST(Chrome, NavBarListsHeadings) {
  const std::vector<markit::Heading> headings =
      markit::ExtractHeadings("# Top\n\n## Mid\n");
  const auto rows = RenderToStrings(markit::NavBar(headings), 30, 5);
  EXPECT_TRUE(AnyLineContains(rows, "Outline"));
  EXPECT_TRUE(AnyLineContains(rows, "Top"));
  EXPECT_TRUE(AnyLineContains(rows, "Mid"));
}

// Documents without headings get an explicit placeholder row.
TEST(Chrome, NavBarPlaceholderWithoutHeadings) {
  const auto rows = RenderToStrings(markit::NavBar({}), 30, 3);
  EXPECT_TRUE(AnyLineContains(rows, "Outline"));
  EXPECT_TRUE(AnyLineContains(rows, "(no headings)"));
}
