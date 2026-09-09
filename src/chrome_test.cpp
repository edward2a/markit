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

// First row containing needle, or -1.
int FindRowContaining(const std::vector<std::string>& rows,
                      const std::string& needle) {
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    if (rows[i].find(needle) != std::string::npos) {
      return i;
    }
  }
  return -1;
}

// Render to a Screen for cell-attribute checks (bold/color of highlight).
ftxui::Screen RenderToScreen(ftxui::Element element, int width, int height) {
  ftxui::Screen screen =
      ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                            ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, std::move(element));
  return screen;
}

// Foreground color of the middle of `needle` on `row` (-1 when absent).
ftxui::Color TextColorOnRow(const ftxui::Screen& screen,
                            const std::vector<std::string>& rows, int row,
                            const std::string& needle) {
  const std::size_t col = rows[row].find(needle) + needle.size() / 2;
  return screen.CellAt(static_cast<int>(col), row).foreground_color;
}

bool AnyCellYellow(const ftxui::Screen& screen, int width, int height) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (screen.CellAt(x, y).foreground_color == ftxui::Color::Yellow) {
        return true;
      }
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

// The current section renders bold in the accent color; others keep their
// plain (bold for H1, dim otherwise) style.
TEST(Chrome, NavBarHighlightsCurrentRow) {
  const std::vector<markit::Heading> headings =
      markit::ExtractHeadings("# Alpha\n\n## Beta\n\n### Gamma\n");
  ASSERT_EQ(headings.size(), 3u);
  constexpr int kWidth = 30;
  constexpr int kHeight = 5;  // title + separator + 3 headings.
  ftxui::Screen screen = RenderToScreen(markit::NavBar(headings, 1), kWidth,
                                        kHeight);
  const auto rows = RenderToStrings(markit::NavBar(headings, 1), kWidth,
                                    kHeight);
  const int beta = FindRowContaining(rows, "Beta");
  ASSERT_GE(beta, 0);
  EXPECT_EQ(TextColorOnRow(screen, rows, beta, "Beta"),
            ftxui::Color::Yellow);
  const std::size_t beta_col = rows[beta].find("Beta");
  EXPECT_TRUE(screen.CellAt(static_cast<int>(beta_col), beta).bold);

  // Neighbors stay unaccented.
  const int alpha = FindRowContaining(rows, "Alpha");
  const int gamma = FindRowContaining(rows, "Gamma");
  ASSERT_GE(alpha, 0);
  ASSERT_GE(gamma, 0);
  EXPECT_NE(TextColorOnRow(screen, rows, alpha, "Alpha"),
            ftxui::Color::Yellow);
  EXPECT_NE(TextColorOnRow(screen, rows, gamma, "Gamma"),
            ftxui::Color::Yellow);
}

// Invalid indices highlight nothing instead of mis-marking a row.
TEST(Chrome, NavBarInvalidCurrentHighlightsNothing) {
  const std::vector<markit::Heading> headings =
      markit::ExtractHeadings("# Alpha\n\n## Beta\n");
  constexpr int kWidth = 30;
  constexpr int kHeight = 4;
  EXPECT_FALSE(AnyCellYellow(
      RenderToScreen(markit::NavBar(headings, -1), kWidth, kHeight), kWidth,
      kHeight));
  EXPECT_FALSE(AnyCellYellow(
      RenderToScreen(markit::NavBar(headings, 99), kWidth, kHeight), kWidth,
      kHeight));
  EXPECT_FALSE(AnyCellYellow(RenderToScreen(markit::NavBar({}, -1), kWidth, 3),
                             kWidth, 3));
}

// Duplicate titles highlight by index: exactly one row is accented.
TEST(Chrome, NavBarHighlightDuplicateHeadingsByIndex) {
  const std::vector<markit::Heading> headings =
      markit::ExtractHeadings("# Same\n\nbody\n\n# Same\n\nmore\n");
  ASSERT_EQ(headings.size(), 2u);
  constexpr int kWidth = 30;
  constexpr int kHeight = 4;
  ftxui::Screen screen =
      RenderToScreen(markit::NavBar(headings, 1), kWidth, kHeight);
  const auto rows =
      RenderToStrings(markit::NavBar(headings, 1), kWidth, kHeight);
  const int first = FindRowContaining(rows, "Same");
  ASSERT_GE(first, 0);
  const int second = FindRowContaining(
      std::vector<std::string>(rows.begin() + first + 1, rows.end()), "Same");
  ASSERT_GE(second, 0);
  const int second_row = first + 1 + second;
  EXPECT_NE(TextColorOnRow(screen, rows, first, "Same"),
            ftxui::Color::Yellow);
  EXPECT_EQ(TextColorOnRow(screen, rows, second_row, "Same"),
            ftxui::Color::Yellow);
}
