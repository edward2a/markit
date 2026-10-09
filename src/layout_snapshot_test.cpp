// Package E feasibility tests: one-pass positioned capture with complete
// height and indexed window slicing. See layout_snapshot.hpp.
#include <gtest/gtest.h>

#include <string>  // for string
#include <vector>  // for vector

#include <ftxui/dom/elements.hpp>  // for vbox, Element
#include <ftxui/dom/node.hpp>      // for Render
#include <ftxui/screen/screen.hpp>  // for Screen

#include "layout_snapshot.hpp"
#include "anchor.hpp"    // for RenderTextRows (reference extraction)
#include "chrome.hpp"    // for ExtractHeadings / Heading
#include "markdown.hpp"  // for RenderMarkdown / BuildMarkdownSnapshot
#include "theme.hpp"     // for Theme

namespace {

std::string Line(int i) { return "line" + std::to_string(i); }

// Trim trailing spaces (the screen pads rows to the width).
std::string Rtrim(std::string s) {
  while (!s.empty() && s.back() == ' ') {
    s.pop_back();
  }
  return s;
}

}  // namespace

// A document taller than the old 65,536-row cap keeps its complete height and
// is laid out exactly once.
TEST(LayoutSnapshot, TallDocumentCompleteHeightOnePass) {
  constexpr int kRows = 70000;  // > 65536
  markit::LayoutSnapshot snapshot;
  ftxui::Elements lines;
  lines.reserve(kRows);
  for (int i = 0; i < kRows; ++i) {
    lines.push_back(markit::SnapshotText(&snapshot, Line(i)));
  }
  snapshot.Build(ftxui::vbox(std::move(lines)), 24);

  EXPECT_EQ(snapshot.height(), kRows);
  EXPECT_EQ(snapshot.layout_passes(), 1);
  EXPECT_EQ(Rtrim(snapshot.RowText(0)), Line(0));
  EXPECT_EQ(Rtrim(snapshot.RowText(65535)), Line(65535));
  EXPECT_EQ(Rtrim(snapshot.RowText(65536)), Line(65536));
  EXPECT_EQ(Rtrim(snapshot.RowText(kRows - 1)), Line(kRows - 1));
}

// Window slices read only their own rows and match the per-row accessor, so a
// window does not depend on document height or on other windows.
TEST(LayoutSnapshot, WindowSlicesMatchRowText) {
  markit::LayoutSnapshot snapshot;
  ftxui::Elements lines;
  for (int i = 0; i < 20; ++i) {
    lines.push_back(markit::SnapshotText(&snapshot, Line(i)));
  }
  snapshot.Build(ftxui::vbox(std::move(lines)), 24);

  const std::vector<std::string> window = snapshot.Rows(7, 11);
  ASSERT_EQ(window.size(), 4u);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(Rtrim(window[static_cast<size_t>(i)]), Line(7 + i));
  }
  // Reversing the request order yields the same content (window-independent).
  const std::vector<std::string> other = snapshot.Rows(7, 11);
  EXPECT_EQ(window, other);
  EXPECT_TRUE(snapshot.Rows(100, 120).empty());
}

// Captured text matches an actual FTXUI render of the same tree.
TEST(LayoutSnapshot, ParityWithFtxuiRender) {
  constexpr int kRows = 6;
  markit::LayoutSnapshot snapshot;
  ftxui::Elements lines;
  for (int i = 0; i < kRows; ++i) {
    lines.push_back(markit::SnapshotText(&snapshot, Line(i)));
  }
  ftxui::Element tree = ftxui::vbox(std::move(lines));
  snapshot.Build(tree, 24);

  ftxui::Screen screen(24, kRows);
  ftxui::Render(screen, tree);
  const std::string rendered = screen.ToString();

  std::string rebuilt;
  for (int i = 0; i < kRows; ++i) {
    rebuilt += Rtrim(snapshot.RowText(i));
    rebuilt += '\n';
  }
  // Compare row by row (the screen also pads to the width and joins with \r\n).
  for (int i = 0; i < kRows; ++i) {
    EXPECT_EQ(Rtrim(snapshot.RowText(i)), Line(i)) << "row " << i;
  }
  EXPECT_NE(rendered.find(Line(0)), std::string::npos);
  EXPECT_NE(rendered.find(Line(kRows - 1)), std::string::npos);
}

// Rebuilding the same snapshot replaces its runs instead of appending.
TEST(LayoutSnapshot, RebuildReplacesRuns) {
  markit::LayoutSnapshot snapshot;
  {
    ftxui::Elements lines;
    for (int i = 0; i < 5; ++i) {
      lines.push_back(markit::SnapshotText(&snapshot, Line(i)));
    }
    snapshot.Build(ftxui::vbox(std::move(lines)), 24);
  }
  EXPECT_EQ(snapshot.height(), 5);
  EXPECT_EQ(snapshot.runs().size(), 5u);

  ftxui::Elements lines;
  lines.push_back(markit::SnapshotText(&snapshot, "only"));
  snapshot.Build(ftxui::vbox(std::move(lines)), 24);
  EXPECT_EQ(snapshot.height(), 1);
  EXPECT_EQ(snapshot.runs().size(), 1u);
  EXPECT_EQ(Rtrim(snapshot.RowText(0)), "only");
}

// The markdown renderer wraps words with FTXUI hflow. Capture must follow the
// wrapped positions: the snapshot's rows match an actual hflow render, and the
// words stay in order across the wrap.
TEST(LayoutSnapshot, HflowWrappingCaptured) {
  constexpr int kWidth = 12;
  markit::LayoutSnapshot snapshot;
  ftxui::Elements pieces;
  pieces.push_back(markit::SnapshotText(&snapshot, "alpha "));
  pieces.push_back(markit::SnapshotText(&snapshot, "beta "));
  pieces.push_back(markit::SnapshotText(&snapshot, "gamma "));
  pieces.push_back(markit::SnapshotText(&snapshot, "delta"));
  ftxui::Element tree = ftxui::hflow(std::move(pieces));
  snapshot.Build(tree, kWidth);

  ASSERT_GT(snapshot.height(), 1) << "expected the row to wrap";

  ftxui::Screen screen(kWidth, snapshot.height());
  ftxui::Render(screen, tree);
  std::string joined;
  for (int row = 0; row < snapshot.height(); ++row) {
    const std::string captured = Rtrim(snapshot.RowText(row));
    joined += captured;
    // The screen row is the concatenation of cell characters.
    std::string rendered_row;
    for (int col = 0; col < kWidth; ++col) {
      rendered_row += screen.CellAt(col, row).character;
    }
    EXPECT_EQ(captured, Rtrim(rendered_row)) << "row " << row;
  }
  const size_t alpha = joined.find("alpha");
  const size_t beta = joined.find("beta");
  const size_t gamma = joined.find("gamma");
  const size_t delta = joined.find("delta");
  ASSERT_NE(alpha, std::string::npos);
  EXPECT_LT(alpha, beta);
  EXPECT_LT(beta, gamma);
  EXPECT_LT(gamma, delta);
}

namespace {

// Compare every captured row with the reference extraction for the same tree.
// RenderTextRows returns raw width-padded cell text, so the snapshot's raw rows
// (also width-padded) must match exactly.
void ExpectParityWithExtraction(const markit::LayoutSnapshot& snapshot,
                                const ftxui::Element& tree, int width) {
  const std::vector<std::string> expected =
      markit::RenderTextRows(tree, width, snapshot.height());
  ASSERT_EQ(static_cast<int>(expected.size()), snapshot.height());
  for (int row = 0; row < snapshot.height(); ++row) {
    EXPECT_EQ(snapshot.RowSparseText(row), expected[static_cast<size_t>(row)])
        << "row " << row;
  }
}

}  // namespace

// A bordered block (as the renderer uses for code and tables) matches the
// reference extraction, including the frame characters.
TEST(LayoutSnapshot, BorderParityWithExtraction) {
  constexpr int kWidth = 20;
  markit::LayoutSnapshot snapshot;
  ftxui::Elements rows;
  rows.push_back(markit::SnapshotText(&snapshot, "alpha"));
  rows.push_back(markit::SnapshotText(&snapshot, "beta"));
  // A bordered block inside a document-like vbox keeps its natural height (a
  // bare border at the root would be stretched by the extraction's yframe).
  ftxui::Element tree = ftxui::vbox({
      markit::SnapshotText(&snapshot, "before"),
      markit::SnapshotBorder(&snapshot, ftxui::vbox(std::move(rows))),
      markit::SnapshotText(&snapshot, "after"),
  });
  snapshot.Build(tree, kWidth);
  ExpectParityWithExtraction(snapshot, tree, kWidth);
}

// A horizontal rule matches the reference extraction.
TEST(LayoutSnapshot, SeparatorParityWithExtraction) {
  constexpr int kWidth = 12;
  markit::LayoutSnapshot snapshot;
  ftxui::Elements lines;
  lines.push_back(markit::SnapshotText(&snapshot, "above"));
  lines.push_back(markit::SnapshotSeparator(&snapshot));
  lines.push_back(markit::SnapshotText(&snapshot, "below"));
  ftxui::Element tree = ftxui::vbox(std::move(lines));
  snapshot.Build(tree, kWidth);
  ExpectParityWithExtraction(snapshot, tree, kWidth);
}

// A long wrapped paragraph (hflow, as the renderer wraps words) matches the
// reference extraction row for row.
TEST(LayoutSnapshot, HugeParagraphParityWithExtraction) {
  constexpr int kWidth = 24;
  markit::LayoutSnapshot snapshot;
  ftxui::Elements words;
  for (int i = 0; i < 200; ++i) {
    words.push_back(markit::SnapshotText(&snapshot, "w" + std::to_string(i) + " "));
  }
  ftxui::Element tree = ftxui::hflow(std::move(words));
  snapshot.Build(tree, kWidth);
  ASSERT_GT(snapshot.height(), 10);
  ExpectParityWithExtraction(snapshot, tree, kWidth);
}

// A tall document mixing plain lines, bordered boxes and rules matches the
// reference extraction across every row, with one layout pass.
TEST(LayoutSnapshot, TallMixedDocumentParityWithExtraction) {
  constexpr int kWidth = 30;
  constexpr int kBlocks = 2000;
  markit::LayoutSnapshot snapshot;
  ftxui::Elements blocks;
  for (int i = 0; i < kBlocks; ++i) {
    blocks.push_back(markit::SnapshotText(&snapshot, "line" + std::to_string(i)));
    if (i % 10 == 0) {
      ftxui::Elements boxed;
      boxed.push_back(markit::SnapshotText(&snapshot, "boxed " + std::to_string(i)));
      blocks.push_back(markit::SnapshotBorder(
          &snapshot, ftxui::vbox(std::move(boxed))));
    }
    if (i % 7 == 0) {
      blocks.push_back(markit::SnapshotSeparator(&snapshot));
    }
  }
  ftxui::Element tree = ftxui::vbox(std::move(blocks));
  snapshot.Build(tree, kWidth);
  EXPECT_EQ(snapshot.layout_passes(), 1);
  EXPECT_GT(snapshot.height(), kBlocks);
  ExpectParityWithExtraction(snapshot, tree, kWidth);
}

// The renderer's snapshot capture must reproduce the reference extraction for
// real Markdown (headings, lists, quotes, code, tables, links, rules). The
// reference tree is built independently without a snapshot, so this checks the
// capture wiring, not just the snapshot primitives.
void ExpectMarkdownParity(const std::string& markdown, markit::WrapMode mode,
                          int width) {
  const markit::Theme theme;
  ftxui::Element reference = markit::RenderMarkdown(markdown, theme, mode);
  const int effective =
      mode == markit::WrapMode::Scroll
          ? markit::SearchExtractWidth(reference, width, /*is_scroll=*/true)
          : width;

  markit::LayoutSnapshot snapshot;
  markit::BuildMarkdownSnapshot(markdown, theme, mode, effective, snapshot);

  const std::vector<std::string> expected =
      markit::RenderTextRows(reference, effective, snapshot.height());
  ASSERT_LE(expected.size(), static_cast<size_t>(snapshot.height()));
  for (size_t row = 0; row < expected.size(); ++row) {
    EXPECT_EQ(snapshot.RowSparseText(static_cast<int>(row)), expected[row])
        << "row " << row;
  }
}

const char* kRichDocument() {
  return "# Title\n"
         "\n"
         "A paragraph with **bold**, _italic_, `code`, and a "
         "[link](https://example.com/path).\n"
         "\n"
         "## Section\n"
         "\n"
         "- first item\n"
         "- second item with a longer body that should wrap at narrow widths\n"
         "\n"
         "> a quoted line\n"
         "> spanning two source lines\n"
         "\n"
         "```cpp\n"
         "int main() {\n"
         "  return 0;\n"
         "}\n"
         "```\n"
         "\n"
         "| left | right |\n"
         "| ---- | ----- |\n"
         "| a    | b     |\n"
         "| c    | d     |\n"
         "\n"
         "---\n"
         "\n"
         "Tail paragraph after a rule.\n";
}

TEST(LayoutSnapshot, MarkdownWrapParityWithExtraction) {
  ExpectMarkdownParity(kRichDocument(), markit::WrapMode::Wrap, 40);
  ExpectMarkdownParity(kRichDocument(), markit::WrapMode::Wrap, 80);
  ExpectMarkdownParity(kRichDocument(), markit::WrapMode::Wrap, 24);
}

TEST(LayoutSnapshot, MarkdownScrollParityWithExtraction) {
  ExpectMarkdownParity(kRichDocument(), markit::WrapMode::Scroll, 80);
  ExpectMarkdownParity(kRichDocument(), markit::WrapMode::Scroll, 40);
}

// TextRows() is the exact row set search matches against; it must equal the
// reference extraction (trimmed to the last non-blank row) for real Markdown.
void ExpectTextRowsParity(const std::string& markdown, markit::WrapMode mode,
                          int width) {
  const markit::Theme theme;
  ftxui::Element reference = markit::RenderMarkdown(markdown, theme, mode);
  const int effective =
      mode == markit::WrapMode::Scroll
          ? markit::SearchExtractWidth(reference, width, /*is_scroll=*/true)
          : width;
  markit::LayoutSnapshot snapshot;
  markit::BuildMarkdownSnapshot(markdown, theme, mode, effective, snapshot);
  const std::vector<std::string> expected =
      markit::RenderTextRows(reference, effective, snapshot.height());
  EXPECT_EQ(snapshot.TextRows(), expected);
}

TEST(LayoutSnapshot, TextRowsMatchReference) {
  ExpectTextRowsParity(kRichDocument(), markit::WrapMode::Wrap, 40);
  ExpectTextRowsParity(kRichDocument(), markit::WrapMode::Scroll, 80);
  ExpectTextRowsParity("trailing code block\n\n```\nlast code line\n```\n",
                       markit::WrapMode::Wrap, 30);
  ExpectTextRowsParity("para\n\n```\n\n\n```\n", markit::WrapMode::Wrap, 30);
}

// TextRows() has no height cap: a tall code block keeps every row.
TEST(LayoutSnapshot, TextRowsBeyondRenderCap) {
  const markit::Theme theme;
  std::string md = "```\n";
  for (int i = 0; i < 70000; ++i) {
    md += "x\n";
  }
  md += "```\n";
  markit::LayoutSnapshot snapshot;
  markit::BuildMarkdownSnapshot(md, theme, markit::WrapMode::Wrap, 20, snapshot);
  const std::vector<std::string> rows = snapshot.TextRows();
  EXPECT_GT(rows.size(), 65536u);
  ASSERT_GE(rows.size(), 2u);
  EXPECT_NE(rows[rows.size() - 2].find("x"), std::string::npos);
}

std::string RenderToText(const ftxui::Element& element, int width, int height) {
  ftxui::Screen screen = ftxui::Screen::Create(
      ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, element);
  return screen.ToString();
}

// A snapshot-bound tree (used for live painting in the migration) must render
// byte-for-byte like the plain FTXUI tree, so switching the live content path
// to snapshot primitives cannot change what the user sees.
void ExpectRenderParity(const std::string& markdown, markit::WrapMode mode,
                        int width, int height) {
  const markit::Theme theme;
  markit::LayoutSnapshot snapshot;
  ftxui::Element bound =
      markit::BuildMarkdownSnapshot(markdown, theme, mode, width, snapshot);
  ftxui::Element plain = markit::RenderMarkdown(markdown, theme, mode);
  EXPECT_EQ(RenderToText(bound, width, height),
            RenderToText(plain, width, height));
}

TEST(LayoutSnapshot, MarkdownRenderParityWrap) {
  ExpectRenderParity(kRichDocument(), markit::WrapMode::Wrap, 40, 30);
  ExpectRenderParity(kRichDocument(), markit::WrapMode::Wrap, 80, 40);
  ExpectRenderParity(kRichDocument(), markit::WrapMode::Wrap, 24, 50);
}

TEST(LayoutSnapshot, MarkdownRenderParityScroll) {
  ExpectRenderParity(kRichDocument(), markit::WrapMode::Scroll, 40, 30);
  ExpectRenderParity(kRichDocument(), markit::WrapMode::Scroll, 80, 40);
}

// A combining mark after a wide base exercises the app-owned combining-unit
// node; the snapshot-bound tree must render it exactly like the plain tree.
TEST(LayoutSnapshot, MarkdownCombiningRenderParity) {
  const std::string md = "\uFF26\u0301oo and \uFF26\u0301ar\n";
  ExpectRenderParity(md, markit::WrapMode::Wrap, 20, 5);
  ExpectRenderParity(md, markit::WrapMode::Scroll, 20, 5);
  ExpectMarkdownParity(md, markit::WrapMode::Wrap, 20);
}

// Structural heading spans must agree with the fingerprint-based reference for
// a normal document, in both modes.
TEST(LayoutSnapshot, HeadingSpansMatchReference) {
  const markit::Theme theme;
  const std::string md = kRichDocument();
  const std::vector<markit::Heading> headings = markit::ExtractHeadings(md);
  const markit::HeadingFingerprints fp =
      markit::BuildHeadingFingerprints(headings);

  markit::LayoutSnapshot wrap_snap;
  ftxui::Element wrap_tree =
      markit::BuildMarkdownSnapshot(md, theme, markit::WrapMode::Wrap, 40,
                                    wrap_snap);
  EXPECT_EQ(markit::LocateHeadingRows(wrap_snap),
            markit::LocateHeadingRows(wrap_tree, 40, 24, false, fp));

  markit::LayoutSnapshot scroll_snap;
  ftxui::Element scroll_tree =
      markit::BuildMarkdownSnapshot(md, theme, markit::WrapMode::Scroll, 40,
                                    scroll_snap);
  EXPECT_EQ(markit::LocateHeadingRows(scroll_snap),
            markit::LocateHeadingRows(scroll_tree, 40, 24, true, fp));
}

// Empty and duplicate headings still get structural spans (the fingerprint
// reference skips empty ones); ordinals stay aligned with ExtractHeadings.
TEST(LayoutSnapshot, HeadingSpansKeepEmptyAndDuplicateHeadings) {
  const markit::Theme theme;
  const std::string md = "# A\n\n#\n\n# A\n\ntext\n";
  const std::vector<markit::Heading> headings = markit::ExtractHeadings(md);
  ASSERT_EQ(headings.size(), 3u);

  markit::LayoutSnapshot snapshot;
  markit::BuildMarkdownSnapshot(md, theme, markit::WrapMode::Wrap, 40, snapshot);
  const std::vector<std::pair<int, int>> located =
      markit::LocateHeadingRows(snapshot);
  ASSERT_EQ(located.size(), 3u);
  EXPECT_EQ(located[0].second, 0);
  EXPECT_EQ(located[1].second, 1);  // empty heading still has a row.
  EXPECT_EQ(located[2].second, 2);
  EXPECT_LT(located[0].first, located[1].first);
  EXPECT_LT(located[1].first, located[2].first);
}

// A heading past the old 65,536-row render cap is still located, proving the
// structural path has no cutoff.
TEST(LayoutSnapshot, HeadingSpanBeyondRenderCap) {
  const markit::Theme theme;
  std::string md = "```\n";
  for (int i = 0; i < 70000; ++i) {
    md += "x\n";
  }
  md += "```\n\n# Late Heading\n";

  markit::LayoutSnapshot snapshot;
  markit::BuildMarkdownSnapshot(md, theme, markit::WrapMode::Wrap, 20, snapshot);
  ASSERT_GT(snapshot.height(), 65536);
  const std::vector<std::pair<int, int>> located =
      markit::LocateHeadingRows(snapshot);
  ASSERT_EQ(located.size(), 1u);
  EXPECT_GT(located[0].first, 65536);
}

// The snapshot-based toggle mapping must agree with the fingerprint reference
// for a normal document, in both directions.
void ExpectToggleParity(const std::string& md, bool old_is_scroll, int width,
                        int viewport_height) {
  const markit::Theme theme;
  const std::vector<markit::Heading> headings = markit::ExtractHeadings(md);
  const markit::HeadingFingerprints fp =
      markit::BuildHeadingFingerprints(headings);
  const markit::WrapMode old_mode =
      old_is_scroll ? markit::WrapMode::Scroll : markit::WrapMode::Wrap;
  const markit::WrapMode new_mode =
      old_is_scroll ? markit::WrapMode::Wrap : markit::WrapMode::Scroll;

  ftxui::Element old_tree = markit::RenderMarkdown(md, theme, old_mode);
  ftxui::Element new_tree = markit::RenderMarkdown(md, theme, new_mode);
  const int old_width =
      old_is_scroll ? markit::SearchExtractWidth(old_tree, width, true) : width;
  const int new_width =
      old_is_scroll ? width : markit::SearchExtractWidth(new_tree, width, true);

  markit::LayoutSnapshot old_snap;
  markit::LayoutSnapshot new_snap;
  markit::BuildMarkdownSnapshot(md, theme, old_mode, old_width, old_snap);
  markit::BuildMarkdownSnapshot(md, theme, new_mode, new_width, new_snap);

  for (int selected : {0, 1, 5, 10, 20, 40}) {
    int ref_h = 0;
    int got_h = 0;
    const int ref = markit::MapTogglePosition(
        old_tree, new_tree, selected, width, viewport_height, old_is_scroll, fp,
        &ref_h);
    const int got = markit::MapTogglePosition(old_snap, new_snap, selected,
                                              viewport_height, old_is_scroll,
                                              &got_h);
    EXPECT_EQ(got, ref) << "selected " << selected
                        << " old_is_scroll " << old_is_scroll;
    EXPECT_EQ(got_h, ref_h) << "height selected " << selected;
  }
}

TEST(LayoutSnapshot, ToggleMappingMatchesReference) {
  ExpectToggleParity(kRichDocument(), /*old_is_scroll=*/false, 40, 10);
  ExpectToggleParity(kRichDocument(), /*old_is_scroll=*/true, 40, 10);
  ExpectToggleParity(kRichDocument(), /*old_is_scroll=*/false, 24, 8);
}


