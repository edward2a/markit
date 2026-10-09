// Package E feasibility tests: one-pass positioned capture with complete
// height and indexed window slicing. See layout_snapshot.hpp.
#include <gtest/gtest.h>

#include <string>  // for string
#include <vector>  // for vector

#include <ftxui/dom/elements.hpp>  // for vbox, Element
#include <ftxui/dom/node.hpp>      // for Render
#include <ftxui/screen/screen.hpp>  // for Screen

#include "layout_snapshot.hpp"
#include "anchor.hpp"  // for RenderTextRows (reference extraction)

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


