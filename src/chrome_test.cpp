// Tests for the screen chrome builders (src/chrome.cpp).
#include "chrome.hpp"

#include <string>  // for string
#include <utility>  // for pair
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

// Inverted flag of the middle of `needle` on `row` (false when absent).
bool IsInvertedOnRow(const ftxui::Screen& screen,
                     const std::vector<std::string>& rows, int row,
                     const std::string& needle) {
  const std::size_t col = rows[row].find(needle);
  if (col == std::string::npos) {
    return false;
  }
  return screen.CellAt(static_cast<int>(col + needle.size() / 2), row).inverted;
}

// No heading row (any row past the title + separator) is inverted.
bool NoHeadingRowInverted(const ftxui::Screen& screen,
                          const std::vector<std::string>& rows, int width) {
  for (int y = 2; y < static_cast<int>(rows.size()); ++y) {
    for (int x = 0; x < width; ++x) {
      if (screen.CellAt(x, y).inverted) {
        return false;
      }
    }
  }
  return true;
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

// The search state formatter covers every status-bar state.
TEST(Chrome, FormatSearchStatusStates) {
  EXPECT_EQ(markit::FormatSearchStatus(-1, 0, true), "invalid pattern");
  EXPECT_EQ(markit::FormatSearchStatus(-1, 0, false), "no matches");
  EXPECT_EQ(markit::FormatSearchStatus(-1, 7, false), "7 matches");
  EXPECT_EQ(markit::FormatSearchStatus(9, 7, false), "7 matches");
  EXPECT_EQ(markit::FormatSearchStatus(0, 7, false), "1/7");
  EXPECT_EQ(markit::FormatSearchStatus(6, 7, false), "7/7");
}

// The status bar appends the search suffix while search is open, and is
// unchanged when it is empty.
TEST(Chrome, StatusBarAppendsSearchSuffix) {
  const auto with =
      RenderToStrings(markit::StatusBar("/a/b/notes.md", 0, 9,
                                        markit::WrapMode::Wrap, "2/7"),
                      60, 1);
  ASSERT_EQ(with.size(), 1u);
  EXPECT_TRUE(AnyLineContains(with, "2/7"));
  const auto without =
      RenderToStrings(markit::StatusBar("/a/b/notes.md", 0, 9,
                                        markit::WrapMode::Wrap),
                      60, 1);
  ASSERT_EQ(without.size(), 1u);
  EXPECT_FALSE(AnyLineContains(without, "matches"));
}

// The action bar is a non-empty single row naming the main keys.
TEST(Chrome, ActionBarListsKeys) {
  const auto rows = RenderToStrings(markit::ActionBar(), 100, 1);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_TRUE(AnyLineContains(rows, "q:quit"));
  EXPECT_TRUE(AnyLineContains(rows, "w:wrap/scroll"));
  EXPECT_TRUE(AnyLineContains(rows, "Ctrl+N:nav"));
  EXPECT_TRUE(AnyLineContains(rows, "/:search"));
  EXPECT_TRUE(AnyLineContains(rows, "n/N:match"));
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

// The keyboard cursor inverts its row plus the title only while focused;
// the view highlight stays independent.
TEST(Chrome, NavBarCursorInvertedWhenFocused) {
  const std::vector<markit::Heading> headings =
      markit::ExtractHeadings("# Alpha\n\n## Beta\n\n### Gamma\n");
  ASSERT_EQ(headings.size(), 3u);
  constexpr int kWidth = 30;
  constexpr int kHeight = 5;
  ftxui::Screen screen =
      RenderToScreen(markit::NavBar(headings, 0, 2, true), kWidth, kHeight);
  const auto rows =
      RenderToStrings(markit::NavBar(headings, 0, 2, true), kWidth, kHeight);
  const int gamma = FindRowContaining(rows, "Gamma");
  ASSERT_GE(gamma, 0);
  EXPECT_TRUE(IsInvertedOnRow(screen, rows, gamma, "Gamma"));
  const int title = FindRowContaining(rows, "Outline");
  ASSERT_GE(title, 0);
  EXPECT_TRUE(IsInvertedOnRow(screen, rows, title, "Outline"));
  // View highlight untouched: Alpha still accented, cursor row is not.
  const int alpha = FindRowContaining(rows, "Alpha");
  ASSERT_GE(alpha, 0);
  EXPECT_EQ(TextColorOnRow(screen, rows, alpha, "Alpha"),
            ftxui::Color::Yellow);
  EXPECT_NE(TextColorOnRow(screen, rows, gamma, "Gamma"),
            ftxui::Color::Yellow);
}

// Unfocused: cursor index and title render plain, view highlight intact.
TEST(Chrome, NavBarCursorIgnoredWhenUnfocused) {
  const std::vector<markit::Heading> headings =
      markit::ExtractHeadings("# Alpha\n\n## Beta\n");
  constexpr int kWidth = 30;
  constexpr int kHeight = 4;
  ftxui::Screen screen =
      RenderToScreen(markit::NavBar(headings, 0, 1, false), kWidth, kHeight);
  const auto rows =
      RenderToStrings(markit::NavBar(headings, 0, 1, false), kWidth, kHeight);
  EXPECT_TRUE(NoHeadingRowInverted(screen, rows, kWidth));
  const int title = FindRowContaining(rows, "Outline");
  ASSERT_GE(title, 0);
  EXPECT_FALSE(IsInvertedOnRow(screen, rows, title, "Outline"));
  const int alpha = FindRowContaining(rows, "Alpha");
  ASSERT_GE(alpha, 0);
  EXPECT_EQ(TextColorOnRow(screen, rows, alpha, "Alpha"),
            ftxui::Color::Yellow);
}

// Cursor on the section in view combines accent + inversion.
TEST(Chrome, NavBarCursorCombinesWithCurrent) {
  const std::vector<markit::Heading> headings =
      markit::ExtractHeadings("# Alpha\n\n## Beta\n");
  constexpr int kWidth = 30;
  constexpr int kHeight = 4;
  ftxui::Screen screen =
      RenderToScreen(markit::NavBar(headings, 1, 1, true), kWidth, kHeight);
  const auto rows =
      RenderToStrings(markit::NavBar(headings, 1, 1, true), kWidth, kHeight);
  const int beta = FindRowContaining(rows, "Beta");
  ASSERT_GE(beta, 0);
  EXPECT_TRUE(IsInvertedOnRow(screen, rows, beta, "Beta"));
  EXPECT_EQ(TextColorOnRow(screen, rows, beta, "Beta"),
            ftxui::Color::Yellow);
  const int alpha = FindRowContaining(rows, "Alpha");
  ASSERT_GE(alpha, 0);
  EXPECT_FALSE(IsInvertedOnRow(screen, rows, alpha, "Alpha"));
}

// Out-of-range cursor inverts no heading row (title still shows focus).
TEST(Chrome, NavBarInvalidCursorInvertsNoHeadingRow) {
  const std::vector<markit::Heading> headings =
      markit::ExtractHeadings("# Alpha\n\n## Beta\n");
  constexpr int kWidth = 30;
  constexpr int kHeight = 4;
  ftxui::Screen screen =
      RenderToScreen(markit::NavBar(headings, -1, 99, true), kWidth, kHeight);
  const auto rows =
      RenderToStrings(markit::NavBar(headings, -1, 99, true), kWidth, kHeight);
  EXPECT_TRUE(NoHeadingRowInverted(screen, rows, kWidth));
}

// Nav offset clamps to the valid window range.
TEST(Chrome, ClampNavOffset) {
  EXPECT_EQ(markit::ClampNavOffset(0, 5, 3), 0);
  EXPECT_EQ(markit::ClampNavOffset(2, 5, 3), 2);
  EXPECT_EQ(markit::ClampNavOffset(9, 5, 3), 2);
  EXPECT_EQ(markit::ClampNavOffset(-4, 5, 3), 0);
  EXPECT_EQ(markit::ClampNavOffset(3, 2, 5), 0);  // fewer rows than window.
  EXPECT_EQ(markit::ClampNavOffset(0, 0, 3), 0);  // no headings.
}

// Follow-scroll moves the offset only as far as needed to reveal the
// cursor, then clamps.
TEST(Chrome, FollowNavOffset) {
  EXPECT_EQ(markit::FollowNavOffset(0, 1, 5, 3), 0);  // inside: unchanged.
  EXPECT_EQ(markit::FollowNavOffset(0, 4, 5, 3), 2);  // below: shift up.
  EXPECT_EQ(markit::FollowNavOffset(2, 0, 5, 3), 0);  // above: shift down.
  EXPECT_EQ(markit::FollowNavOffset(9, 4, 5, 3), 2);  // clamps at the end.
  EXPECT_EQ(markit::FollowNavOffset(0, 1, 2, 5), 0);  // window fits all.
}

// The focused action bar names the nav keys instead of the content keys.
TEST(Chrome, ActionBarFocusHints) {
  const auto rows = RenderToStrings(markit::ActionBar(true), 100, 1);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_TRUE(AnyLineContains(rows, "Enter"));
  EXPECT_TRUE(AnyLineContains(rows, "Tab:main"));
  const auto plain = RenderToStrings(markit::ActionBar(), 120, 1);
  EXPECT_TRUE(AnyLineContains(plain, "Tab:focus"));
}

// Render through SearchHighlight and return the screen for style checks.
ftxui::Screen RenderHighlight(ftxui::Element element, const int* row,
                              const std::vector<std::pair<int, int>>* spans,
                              int width, int height) {
  ftxui::Screen screen =
      ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                            ftxui::Dimension::Fixed(height));
  ftxui::Render(
      screen, markit::SearchHighlight(std::move(element), row, spans));
  return screen;
}

// Only the span cells invert; the rest of the row is untouched.
TEST(Chrome, SearchHighlightInvertsSpanCellsOnly) {
  const int row = 0;
  const std::vector<std::pair<int, int>> spans = {{6, 11}};
  ftxui::Screen screen =
      RenderHighlight(ftxui::text("hello world"), &row, &spans, 20, 1);
  for (int x = 0; x < 20; ++x) {
    EXPECT_EQ(screen.CellAt(x, 0).inverted, x >= 6 && x < 11) << "x=" << x;
  }
}

// The mark lands on the targeted content row, not the first row.
TEST(Chrome, SearchHighlightTargetsGivenRow) {
  using ftxui::text;
  const int row = 1;
  const std::vector<std::pair<int, int>> spans = {{0, 3}};
  ftxui::Screen screen = RenderHighlight(
      ftxui::vbox({text("alpha"), text("beta")}), &row, &spans, 10, 2);
  EXPECT_FALSE(screen.CellAt(0, 0).inverted);
  for (int x = 0; x < 3; ++x) {
    EXPECT_TRUE(screen.CellAt(x, 1).inverted) << "x=" << x;
  }
  EXPECT_FALSE(screen.CellAt(3, 1).inverted);
}

// Inactive inputs render the child untouched: null row, negative row,
// out-of-range row, and empty spans.
TEST(Chrome, SearchHighlightInactiveRendersUntouched) {
  const std::vector<std::pair<int, int>> spans = {{0, 5}};
  const std::vector<std::pair<int, int>> empty;
  const int neg = -1;
  const int far = 7;
  const int row = 0;
  const int* rows[] = {nullptr, &neg, &far, &row};
  const std::vector<std::pair<int, int>>* span_sets[] = {&spans, &spans,
                                                         &spans, &empty};
  for (int i = 0; i < 4; ++i) {
    ftxui::Screen screen = RenderHighlight(ftxui::text("hello"), rows[i],
                                           span_sets[i], 10, 1);
    for (int x = 0; x < 10; ++x) {
      EXPECT_FALSE(screen.CellAt(x, 0).inverted) << "case=" << i << " x=" << x;
    }
  }
}

// Byte spans map across multi-byte graphemes: "a" + é + "b", so the é
// span [1,3) inverts exactly the middle cell. (Adjacent literals: \xA9
// would swallow the "b" into the hex escape.)
TEST(Chrome, SearchHighlightMultibyteSpan) {
  const int row = 0;
  const std::vector<std::pair<int, int>> spans = {{1, 3}};
  ftxui::Screen screen =
      RenderHighlight(ftxui::text("a\xC3\xA9"
                                  "b"),
                      &row, &spans, 10, 1);
  EXPECT_FALSE(screen.CellAt(0, 0).inverted);
  EXPECT_TRUE(screen.CellAt(1, 0).inverted);
  EXPECT_FALSE(screen.CellAt(2, 0).inverted);
}
