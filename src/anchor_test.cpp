// Tests for the section-scoped content anchor (scroll/wrap toggle).
#include <gtest/gtest.h>

#include <algorithm>  // for clamp, max
#include <string>     // for string
#include <utility>    // for pair
#include <vector>     // for vector

#include <ftxui/dom/elements.hpp>       // for Element
#include <ftxui/dom/node.hpp>           // for Render
#include <ftxui/screen/screen.hpp>      // for Screen
#include <ftxui/screen/terminal.hpp>    // for Dimension

#include "anchor.hpp"
#include "chrome.hpp"
#include "config.hpp"
#include "markdown.hpp"

namespace {

constexpr int kWidth = 40;
constexpr int kHeight = 6;

markit::Config ScrollCfg() {
  markit::Config cfg;
  cfg.horizontal_wrap = markit::WrapMode::Scroll;
  return cfg;
}

markit::Config WrapCfg() {
  markit::Config cfg;
  cfg.horizontal_wrap = markit::WrapMode::Wrap;
  return cfg;
}

std::string Normalize(const std::string& s) {
  std::string out;
  bool pending = false;
  bool started = false;
  for (char c : s) {
    const bool ws = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (ws) {
      if (started) {
        pending = true;
      }
      continue;
    }
    if (pending) {
      out += ' ';
      pending = false;
    }
    out += c;
    started = true;
  }
  return out;
}

// Rendered text rows of a tree at the test width (content rows only).
std::vector<std::string> LayoutRows(ftxui::Element el, int width) {
  el->ComputeRequirement();
  int cap = std::clamp(el->requirement().min_y, 256, 65536);
  ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                              ftxui::Dimension::Fixed(cap));
  int last = -1;
  for (;;) {
    ftxui::Render(screen, el);
    last = -1;
    for (int row = 0; row < cap; ++row) {
      for (int col = 0; col < width; ++col) {
        const ftxui::Cell& cell = screen.CellAt(col, row);
        if ((cell.character != " " && !cell.character.empty()) ||
            cell.background_color != ftxui::Color::Default) {
          last = row;
          break;
        }
      }
    }
    if (last < cap - 1 || cap >= 65536) {
      break;
    }
    cap *= 4;
    screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                  ftxui::Dimension::Fixed(cap));
  }
  std::vector<std::string> rows;
  for (int row = 0; row <= last; ++row) {
    std::string raw;
    for (int col = 0; col < width; ++col) {
      raw += screen.CellAt(col, row).character;
    }
    rows.push_back(Normalize(raw));
  }
  return rows;
}

int FindPrefix(const std::vector<std::string>& rows, const std::string& prefix,
               int from = 0) {
  for (int r = from; r < static_cast<int>(rows.size()); ++r) {
    if (rows[r].rfind(prefix, 0) == 0) {
      return r;
    }
  }
  return -1;
}

const char* kDoc =
    "# Guide\n"
    "\n"
    "alpha one\n"
    "\n"
    "alpha two\n"
    "\n"
    "this long paragraph has many words so it must wrap onto several rows\n"
    "\n"
    "tail one\n"
    "\n"
    "tail two\n"
    "\n"
    "tail three\n"
    "\n"
    "tail four\n"
    "\n"
    "tail five\n"
    "\n"
    "tail six\n";

}  // namespace

// Scroll -> wrap: a full line maps to its first wrapped fragment.
TEST(Anchor, ScrollToWrapKeepsLine) {
  ScrollCfg();
  auto old_tree = markit::RenderMarkdown(kDoc, ScrollCfg());
  auto new_tree = markit::RenderMarkdown(kDoc, WrapCfg());
  const auto headings = markit::ExtractHeadings(kDoc);
  const auto old_rows = LayoutRows(markit::RenderMarkdown(kDoc, ScrollCfg()),
                                   kWidth);
  const int anchor = FindPrefix(old_rows, "this long paragraph");
  ASSERT_GE(anchor, 0);

  const int mapped = markit::MapTogglePosition(
      old_tree, new_tree, headings, anchor, kWidth, kHeight, true);

  const auto new_rows = LayoutRows(markit::RenderMarkdown(kDoc, WrapCfg()),
                                   kWidth);
  ASSERT_LT(mapped, static_cast<int>(new_rows.size()));
  EXPECT_EQ(new_rows[mapped].rfind("this long paragraph", 0), 0u)
      << "mapped row: '" << new_rows[mapped] << "'";
}

// Wrap -> scroll: a mid-line continuation fragment maps to its owning line.
TEST(Anchor, WrapFragmentToScrollFindsOwner) {
  auto old_tree = markit::RenderMarkdown(kDoc, WrapCfg());
  auto new_tree = markit::RenderMarkdown(kDoc, ScrollCfg());
  const auto headings = markit::ExtractHeadings(kDoc);
  const auto old_rows = LayoutRows(markit::RenderMarkdown(kDoc, WrapCfg()),
                                   kWidth);
  // Continuation fragment: non-empty, not a line start.
  int anchor = -1;
  for (int r = 0; r < static_cast<int>(old_rows.size()); ++r) {
    if (!old_rows[r].empty() &&
        old_rows[r].rfind("this long paragraph", 0) != 0 &&
        old_rows[r].find("wrap onto") != std::string::npos) {
      anchor = r;
      break;
    }
  }
  ASSERT_GE(anchor, 0) << "need a wrapped continuation fragment";

  const int mapped = markit::MapTogglePosition(
      old_tree, new_tree, headings, anchor, kWidth, kHeight, false);

  const auto new_rows = LayoutRows(markit::RenderMarkdown(kDoc, ScrollCfg()),
                                   kWidth);
  // The fragment lives past the viewport clip, so the scroll layout shows
  // the owning line's visible prefix: the mapped row must be that line.
  EXPECT_EQ(mapped, FindPrefix(new_rows, "this long paragraph"));
}

// Identical long lines in two sections: the section scopes the match.
TEST(Anchor, IdenticalLinesResolveBySection) {
  const std::string shared =
      "shared marker line with many extra words to force wrapping across "
      "modes today";
  const std::string doc =
      "# Alpha\n"
      "\n"
      "alpha filler line one\n"
      "\n"
      "alpha filler line two\n"
      "\n"
      "alpha filler line three\n"
      "\n" +
      shared +
      "\n"
      "\n"
      "# Beta\n"
      "\n"
      "beta filler line one\n"
      "\n"
      "beta filler line two\n"
      "\n"
      "beta filler line three\n"
      "\n" +
      shared +
      "\n"
      "\n"
      "tail one\n"
      "\n"
      "tail two\n"
      "\n"
      "tail three\n"
      "\n"
      "tail four\n"
      "\n"
      "tail five\n"
      "\n"
      "tail six\n";
  auto old_tree = markit::RenderMarkdown(doc, ScrollCfg());
  auto new_tree = markit::RenderMarkdown(doc, WrapCfg());
  const auto headings = markit::ExtractHeadings(doc);
  ASSERT_EQ(headings.size(), 2u);
  const auto old_rows = LayoutRows(markit::RenderMarkdown(doc, ScrollCfg()),
                                   kWidth);
  const auto new_rows = LayoutRows(markit::RenderMarkdown(doc, WrapCfg()),
                                   kWidth);

  // Anchor on the Beta (second) occurrence in scroll mode.
  const int first = FindPrefix(old_rows, "shared marker");
  ASSERT_GE(first, 0);
  const int second = FindPrefix(old_rows, "shared marker", first + 1);
  ASSERT_GE(second, 0);
  const int beta_row = FindPrefix(old_rows, "Beta");
  ASSERT_GT(second, beta_row) << "second occurrence must be under Beta";

  const int mapped = markit::MapTogglePosition(
      old_tree, new_tree, headings, second, kWidth, kHeight, true);

  const int new_beta = FindPrefix(new_rows, "Beta");
  EXPECT_GT(mapped, new_beta) << "match must stay inside the Beta section";
  EXPECT_EQ(new_rows[mapped].rfind("shared marker", 0), 0u);
  // It must be the second occurrence, not the Alpha one.
  int count_before = 0;
  for (int r = 0; r < mapped; ++r) {
    if (new_rows[r].rfind("shared marker", 0) == 0) {
      ++count_before;
    }
  }
  EXPECT_EQ(count_before, 1);
}

// Duplicate headings map by occurrence rank.
TEST(Anchor, DuplicateHeadingsMapByRank) {
  const std::string doc =
      "# Repeat\n"
      "\n"
      "first filler line one\n"
      "\n"
      "first filler line two\n"
      "\n"
      "first filler line three\n"
      "\n"
      "first filler line four\n"
      "\n"
      "first filler line five\n"
      "\n"
      "first filler line six\n"
      "\n"
      "# Repeat\n"
      "\n"
      "second filler line one\n"
      "\n"
      "second filler line two\n"
      "\n"
      "second filler line three\n"
      "\n"
      "second filler line four\n"
      "\n"
      "closing line one\n"
      "\n"
      "closing line two\n"
      "\n"
      "closing line three\n"
      "\n"
      "closing line four\n";
  auto old_tree = markit::RenderMarkdown(doc, ScrollCfg());
  auto new_tree = markit::RenderMarkdown(doc, WrapCfg());
  const auto headings = markit::ExtractHeadings(doc);
  ASSERT_EQ(headings.size(), 2u);
  const auto old_rows = LayoutRows(markit::RenderMarkdown(doc, ScrollCfg()),
                                   kWidth);
  const auto new_rows = LayoutRows(markit::RenderMarkdown(doc, WrapCfg()),
                                   kWidth);

  const int anchor = FindPrefix(old_rows, "second filler line two");
  ASSERT_GE(anchor, 0);
  const int mapped = markit::MapTogglePosition(
      old_tree, new_tree, headings, anchor, kWidth, kHeight, true);

  // Second "Repeat" heading in the new layout; the match stays below it.
  const int h1 = FindPrefix(new_rows, "Repeat");
  const int h2 = FindPrefix(new_rows, "Repeat", h1 + 1);
  ASSERT_GE(h2, 0);
  EXPECT_GT(mapped, h2);
  EXPECT_EQ(new_rows[mapped].rfind("second filler line two", 0), 0u);
}

// A heading anchor switches exactly to the heading row.
TEST(Anchor, HeadingAnchorSwitchesExactly) {
  const std::string doc =
      "# Alpha\n"
      "\n"
      "alpha filler line one\n"
      "\n"
      "alpha filler line two\n"
      "\n"
      "alpha filler line three\n"
      "\n"
      "alpha filler line four\n"
      "\n"
      "alpha filler line five\n"
      "\n"
      "alpha filler line six\n"
      "\n"
      "# Beta target\n"
      "\n"
      "beta filler line one\n"
      "\n"
      "beta filler line two\n"
      "\n"
      "beta filler line three\n"
      "\n"
      "beta filler line four\n"
      "\n"
      "closing line one\n"
      "\n"
      "closing line two\n";
  auto old_tree = markit::RenderMarkdown(doc, ScrollCfg());
  auto new_tree = markit::RenderMarkdown(doc, WrapCfg());
  const auto headings = markit::ExtractHeadings(doc);
  const auto old_rows = LayoutRows(markit::RenderMarkdown(doc, ScrollCfg()),
                                   kWidth);
  const auto new_rows = LayoutRows(markit::RenderMarkdown(doc, WrapCfg()),
                                   kWidth);
  const int anchor = FindPrefix(old_rows, "Beta target");
  ASSERT_GE(anchor, 0);

  const int mapped = markit::MapTogglePosition(
      old_tree, new_tree, headings, anchor, kWidth, kHeight, true);

  EXPECT_EQ(mapped, FindPrefix(new_rows, "Beta target"));
}

// Short rows (code) fingerprint on their whole text.
TEST(Anchor, ShortRowUsesWholeText) {
  const std::string doc =
      "# Code\n"
      "\n"
      "filler line one here\n"
      "\n"
      "filler line two here\n"
      "\n"
      "```\nhi\n```\n"
      "\n"
      "tail line one here\n"
      "\n"
      "tail line two here\n"
      "\n"
      "tail line three here\n"
      "\n"
      "tail line four here\n"
      "\n"
      "tail line five here\n"
      "\n"
      "tail line six here\n";
  auto old_tree = markit::RenderMarkdown(doc, ScrollCfg());
  auto new_tree = markit::RenderMarkdown(doc, WrapCfg());
  const auto headings = markit::ExtractHeadings(doc);
  const auto old_rows = LayoutRows(markit::RenderMarkdown(doc, ScrollCfg()),
                                   kWidth);
  int anchor = -1;
  for (int r = 0; r < static_cast<int>(old_rows.size()); ++r) {
    if (old_rows[r].find("hi") != std::string::npos) {
      anchor = r;
      break;
    }
  }
  ASSERT_GE(anchor, 0);

  const int mapped = markit::MapTogglePosition(
      old_tree, new_tree, headings, anchor, kWidth, kHeight, true);

  const auto new_rows = LayoutRows(markit::RenderMarkdown(doc, WrapCfg()),
                                   kWidth);
  ASSERT_LT(mapped, static_cast<int>(new_rows.size()));
  EXPECT_NE(new_rows[mapped].find("hi"), std::string::npos);
}

// Blank top row walks up (here: onto the heading gap -> heading rule).
TEST(Anchor, BlankTopRowWalksUpWithDelta) {
  const std::string doc =
      "# Title here\n"
      "\n"
      "body filler line one\n"
      "\n"
      "body filler line two\n"
      "\n"
      "body filler line three\n"
      "\n"
      "body filler line four\n"
      "\n"
      "body filler line five\n"
      "\n"
      "body filler line six\n"
      "\n"
      "body filler line seven\n"
      "\n"
      "body filler line eight\n";
  auto old_tree = markit::RenderMarkdown(doc, ScrollCfg());
  auto new_tree = markit::RenderMarkdown(doc, WrapCfg());
  const auto headings = markit::ExtractHeadings(doc);
  const auto old_rows = LayoutRows(markit::RenderMarkdown(doc, ScrollCfg()),
                                   kWidth);
  const auto new_rows = LayoutRows(markit::RenderMarkdown(doc, WrapCfg()),
                                   kWidth);
  ASSERT_GT(old_rows.size(), 1u);
  ASSERT_TRUE(old_rows[1].empty()) << "heading gap row must be blank";
  const int new_title = FindPrefix(new_rows, "Title here");
  ASSERT_GE(new_title, 0);

  const int mapped = markit::MapTogglePosition(old_tree, new_tree, headings,
                                               1, kWidth, kHeight, true);

  // One row below the heading in the old layout -> one below it in the new.
  EXPECT_EQ(mapped, new_title + 1);
  EXPECT_TRUE(new_rows[mapped].empty());
}

// Documents without headings match by content alone.
TEST(Anchor, NoHeadingsContentMatch) {
  const std::string doc =
      "plain filler line one\n"
      "\n"
      "plain filler line two\n"
      "\n"
      "this long paragraph has many words so it must wrap onto several rows\n"
      "\n"
      "plain filler line three\n"
      "\n"
      "plain filler line four\n"
      "\n"
      "plain filler line five\n"
      "\n"
      "plain filler line six\n"
      "\n"
      "plain filler line seven\n"
      "\n"
      "plain filler line eight\n";
  auto old_tree = markit::RenderMarkdown(doc, ScrollCfg());
  auto new_tree = markit::RenderMarkdown(doc, WrapCfg());
  const auto headings = markit::ExtractHeadings(doc);
  ASSERT_TRUE(headings.empty());
  const auto old_rows = LayoutRows(markit::RenderMarkdown(doc, ScrollCfg()),
                                   kWidth);
  const int anchor = FindPrefix(old_rows, "this long paragraph");
  ASSERT_GE(anchor, 0);

  const int mapped = markit::MapTogglePosition(
      old_tree, new_tree, headings, anchor, kWidth, kHeight, true);

  const auto new_rows = LayoutRows(markit::RenderMarkdown(doc, WrapCfg()),
                                   kWidth);
  ASSERT_LT(mapped, static_cast<int>(new_rows.size()));
  EXPECT_EQ(new_rows[mapped].rfind("this long paragraph", 0), 0u);
}

// Guards: unusable inputs yield 0; wild offsets clamp into range.
TEST(Anchor, GuardsAndClamp) {
  auto tree = markit::RenderMarkdown(kDoc, ScrollCfg());
  const auto headings = markit::ExtractHeadings(kDoc);
  ftxui::Element null;
  EXPECT_EQ(markit::MapTogglePosition(null, tree, headings, 3, kWidth,
                                      kHeight, true),
            0);
  EXPECT_EQ(markit::MapTogglePosition(tree, null, headings, 3, kWidth,
                                      kHeight, true),
            0);
  EXPECT_EQ(markit::MapTogglePosition(tree, tree, headings, 3, 0, kHeight,
                                      true),
            0);

  const int mapped = markit::MapTogglePosition(tree, tree, headings, 100000,
                                                kWidth, kHeight, true);
  const auto rows = LayoutRows(markit::RenderMarkdown(kDoc, ScrollCfg()),
                               kWidth);
  const int max_offset = std::max(0, static_cast<int>(rows.size()) - kHeight);
  EXPECT_EQ(mapped, max_offset);
}

// Narrow viewport: a continuation fragment past the clip forces the exact
// wide rematch, landing on the owning scroll line. The long paragraph sits
// at the top above many fillers so the proportional estimate provably misses
// the owning line (it lands a few rows down instead).
TEST(Anchor, NarrowContinuationToScrollUsesWideFallback) {
  constexpr int kNarrow = 20;
  std::string doc =
      "this long paragraph has many words so it must wrap onto several rows\n";
  for (int i = 0; i < 20; ++i) {
    doc += "\nplain filler line number " + std::to_string(i) + "\n";
  }
  auto old_tree = markit::RenderMarkdown(doc, WrapCfg());
  auto new_tree = markit::RenderMarkdown(doc, ScrollCfg());
  const auto headings = markit::ExtractHeadings(doc);
  const auto old_rows = LayoutRows(markit::RenderMarkdown(doc, WrapCfg()),
                                   kNarrow);
  int anchor = -1;
  for (int r = 0; r < static_cast<int>(old_rows.size()); ++r) {
    if (!old_rows[r].empty() &&
        old_rows[r].rfind("this long paragraph", 0) != 0 &&
        old_rows[r].find("wrap onto") != std::string::npos) {
      anchor = r;
      break;
    }
  }
  ASSERT_GE(anchor, 0) << "need a wrapped continuation fragment";

  const int mapped = markit::MapTogglePosition(
      old_tree, new_tree, headings, anchor, kNarrow, kHeight, false);

  const auto new_rows = LayoutRows(markit::RenderMarkdown(doc, ScrollCfg()),
                                   kWidth);
  EXPECT_EQ(mapped, FindPrefix(new_rows, "this long paragraph"));
}

// Narrow viewport: a truncated but unique fingerprint still maps narrow to
// the first fragment, and reports the new wrap height.
TEST(Anchor, NarrowTruncatedFingerprintKeepsUniqueLine) {
  constexpr int kNarrow = 20;
  auto old_tree = markit::RenderMarkdown(kDoc, ScrollCfg());
  auto new_tree = markit::RenderMarkdown(kDoc, WrapCfg());
  const auto headings = markit::ExtractHeadings(kDoc);
  const auto old_rows = LayoutRows(markit::RenderMarkdown(kDoc, ScrollCfg()),
                                   kNarrow);
  const int anchor = FindPrefix(old_rows, "this long paragraph");
  ASSERT_GE(anchor, 0);

  int new_height = -1;
  const int mapped = markit::MapTogglePosition(
      old_tree, new_tree, headings, anchor, kNarrow, kHeight, true,
      &new_height);

  const auto new_rows = LayoutRows(markit::RenderMarkdown(kDoc, WrapCfg()),
                                   kNarrow);
  ASSERT_LT(mapped, static_cast<int>(new_rows.size()));
  EXPECT_EQ(new_rows[mapped].rfind("this long paragraph", 0), 0u);
  EXPECT_EQ(new_height, static_cast<int>(new_rows.size()));
}

// Narrow viewport: two lines sharing a clipped prefix are ambiguous narrow,
// so the wide rematch plus nearness to the proportional estimate picks the
// right line. The anchor sits mid-document so the estimate is meaningful
// (a tiny doc degenerates it to 0); identical first-fragments can only be
// separated by position.
TEST(Anchor, NarrowAmbiguousPrefixFallsBackToWide) {
  constexpr int kNarrow = 20;
  std::string doc = "# T\n";
  for (int i = 0; i < 6; ++i) {
    doc += "\ntop filler line number " + std::to_string(i) + "\n";
  }
  doc +=
      "\nalpha beta gamma delta epsilon zeta eta theta iota kappa one tail end\n"
      "\n"
      "alpha beta gamma delta epsilon zeta eta theta iota kappa two tail end\n";
  for (int i = 0; i < 14; ++i) {
    doc += "\nbottom filler line number " + std::to_string(i) + "\n";
  }
  auto old_tree = markit::RenderMarkdown(doc, ScrollCfg());
  auto new_tree = markit::RenderMarkdown(doc, WrapCfg());
  const auto headings = markit::ExtractHeadings(doc);
  const auto old_rows = LayoutRows(markit::RenderMarkdown(doc, ScrollCfg()),
                                   kNarrow);
  // Anchor on the SECOND line's scroll row.
  const int first = FindPrefix(old_rows, "alpha beta gamma");
  ASSERT_GE(first, 0);
  const int second = FindPrefix(old_rows, "alpha beta gamma", first + 1);
  ASSERT_GE(second, 0) << "scroll rows keep one row per line";

  const int mapped = markit::MapTogglePosition(
      old_tree, new_tree, headings, second, kNarrow, kHeight, true);

  const auto new_rows = LayoutRows(markit::RenderMarkdown(doc, WrapCfg()),
                                   kNarrow);
  // Line 2's fragments run from its first fragment through "two tail end".
  int line2_last = -1;
  for (int r = 0; r < static_cast<int>(new_rows.size()); ++r) {
    if (new_rows[r].find("two tail end") != std::string::npos) {
      line2_last = r;
      break;
    }
  }
  ASSERT_GE(line2_last, 0);
  int line2_first = -1;
  for (int r = line2_last; r >= 0; --r) {
    if (new_rows[r].rfind("alpha beta gamma", 0) == 0) {
      line2_first = r;
      break;  // nearest line-start above the "two" tail: line 2's own.
    }
  }
  ASSERT_GE(line2_first, 0);
  EXPECT_GE(mapped, line2_first);
  EXPECT_LE(mapped, line2_last) << "mapped row: '" << new_rows[mapped] << "'";
}

// Mirrors main.cpp's nav-highlight loop: last boundary at/above selected.
int CurrentFor(const std::vector<std::pair<int, int>>& map, int selected) {
  int current = -1;
  for (const auto& [row, idx] : map) {
    if (row <= selected) {
      current = idx;
    } else {
      break;
    }
  }
  return current;
}

// Headings locate in order with nav-row indices; the top of view maps to
// the section at or above it.
TEST(Anchor, LocateHeadingRowsFindsSectionsInOrder) {
  const char* doc = "# Alpha\n\nbody one\n\n## Beta\n\nbody two\n";
  auto tree = markit::RenderMarkdown(doc, ScrollCfg());
  const auto headings = markit::ExtractHeadings(doc);
  ASSERT_EQ(headings.size(), 2u);
  const auto map =
      markit::LocateHeadingRows(tree, headings, kWidth, kHeight, true);
  ASSERT_EQ(map.size(), 2u);
  EXPECT_EQ(map[0].first, 0);
  EXPECT_EQ(map[0].second, 0);
  EXPECT_GT(map[1].first, map[0].first);
  EXPECT_EQ(map[1].second, 1);
  EXPECT_EQ(CurrentFor(map, 0), 0) << "heading row highlights its own section";
  EXPECT_EQ(CurrentFor(map, map[1].first), 1);
  EXPECT_EQ(CurrentFor(map, map[1].first + 1), 1);
}

// Preamble rows (above the first heading) map to no section.
TEST(Anchor, LocateHeadingRowsPreambleMapsToNone) {
  const char* doc = "preamble text\n\n# Alpha\n\nbody\n";
  auto tree = markit::RenderMarkdown(doc, ScrollCfg());
  const auto headings = markit::ExtractHeadings(doc);
  ASSERT_EQ(headings.size(), 1u);
  const auto map =
      markit::LocateHeadingRows(tree, headings, kWidth, kHeight, true);
  ASSERT_EQ(map.size(), 1u);
  EXPECT_GT(map[0].first, 0);
  EXPECT_EQ(CurrentFor(map, 0), -1);
  EXPECT_EQ(CurrentFor(map, map[0].first), 0);
}

// Duplicate titles locate by occurrence rank with stable indices.
TEST(Anchor, LocateHeadingRowsDuplicatesMapByRank) {
  const char* doc = "# Same\n\none\n\n# Same\n\ntwo\n";
  auto tree = markit::RenderMarkdown(doc, ScrollCfg());
  const auto headings = markit::ExtractHeadings(doc);
  ASSERT_EQ(headings.size(), 2u);
  const auto map =
      markit::LocateHeadingRows(tree, headings, kWidth, kHeight, true);
  ASSERT_EQ(map.size(), 2u);
  EXPECT_EQ(map[0].second, 0);
  EXPECT_EQ(map[1].second, 1);
  EXPECT_GT(map[1].first, map[0].first);
  EXPECT_EQ(CurrentFor(map, map[0].first), 0);
  EXPECT_EQ(CurrentFor(map, map[1].first), 1);
}

// Wrap-mode trees locate too (boundaries are width-dependent there).
TEST(Anchor, LocateHeadingRowsWorksInWrapMode) {
  const char* doc = "# Alpha\n\nbody one\n\n## Beta\n\nbody two\n";
  auto tree = markit::RenderMarkdown(doc, WrapCfg());
  const auto headings = markit::ExtractHeadings(doc);
  const auto map =
      markit::LocateHeadingRows(tree, headings, kWidth, kHeight, false);
  ASSERT_EQ(map.size(), 2u);
  EXPECT_EQ(map[0].second, 0);
  EXPECT_EQ(map[1].second, 1);
  EXPECT_GT(map[1].first, map[0].first);
}

// Unusable inputs locate nothing instead of crashing.
TEST(Anchor, LocateHeadingRowsGuards) {
  EXPECT_TRUE(
      markit::LocateHeadingRows(ftxui::Element(), {}, kWidth, kHeight, true)
          .empty());
  auto tree = markit::RenderMarkdown("# A\n", ScrollCfg());
  const auto headings = markit::ExtractHeadings("# A\n");
  EXPECT_TRUE(markit::LocateHeadingRows(tree, headings, 0, kHeight, true)
                  .empty());
  EXPECT_TRUE(markit::LocateHeadingRows(tree, headings, kWidth, 0, true)
                  .empty());
  EXPECT_TRUE(markit::LocateHeadingRows(tree, {}, kWidth, kHeight, true)
                  .empty());
}
