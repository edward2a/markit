// Package E feasibility prototype: an immutable, indexed layout snapshot.
//
// The current extraction path re-runs FTXUI's full requirement computation for
// every bounded window (via yframe/focusPosition), which is O(document) per
// window, and caps documents at 65,536 rows. This prototype proves the selected
// direction instead: lay the tree out ONCE (one ComputeRequirement + SetBox
// pass, no full-height screen allocation), capture every text run's position,
// index the runs by row, and serve arbitrary windows from the index in
// O(runs in window).
//
// This is a spike, not the production snapshot. It captures text runs only;
// FTXUI-drawn decorations (borders, separators), resolved styles/colors and
// hyperlinks, and hflow byte-span mapping are still open (see the Package E
// report). Trees intended for capture must build their text with SnapshotText.
#ifndef MARKIT_LAYOUT_SNAPSHOT_HPP
#define MARKIT_LAYOUT_SNAPSHOT_HPP

#include <functional>  // for function
#include <string>  // for string
#include <utility>  // for pair
#include <vector>  // for vector

#include <ftxui/dom/elements.hpp>  // for Element

namespace markit {

class LayoutSnapshot;

// One captured positioned text run, in document cell coordinates.
struct SnapshotRun {
  int row = 0;
  int col = 0;
  std::string text;
  bool bold = false;
};

// A text node that records its position during a LayoutSnapshot::Build layout
// pass. Use it in place of ftxui::text for content that should be captured.
ftxui::Element SnapshotText(LayoutSnapshot* snapshot, std::string text,
                            bool bold = false);

// A light border around `child`, recorded as positioned runs so snapshot rows
// include the frame exactly like an FTXUI render (the renderer's tables and
// code boxes use ftxui::borderLight). Use in place of ftxui::borderLight.
ftxui::Element SnapshotBorder(LayoutSnapshot* snapshot, ftxui::Element child);

// A horizontal light rule recorded as a positioned run. Use in place of
// ftxui::separator for horizontal rules.
ftxui::Element SnapshotSeparator(LayoutSnapshot* snapshot);

// Wrap a heading element so its content-row span (inclusive first/last row) is
// recorded under `ordinal`, the markdown heading occurrence. Use only for
// MD_BLOCK_H headings; the ordinal must match the outline heading index.
ftxui::Element SnapshotHeading(LayoutSnapshot* snapshot, int ordinal,
                               ftxui::Element child);

class LayoutSnapshot {
 public:
  LayoutSnapshot() = default;

  // Lay out `element` at `width` in a single pass and capture its runs.
  // `element`'s SnapshotText nodes must have been created with `this`.
  // `cancelled` is checked between layout passes; if it fires, the build stops
  // and `cancelled()` is set (the snapshot is incomplete and must be discarded).
  void Build(ftxui::Element element, int width,
             const std::function<bool()>& cancelled = {});

  int width() const { return width_; }
  int height() const { return height_; }
  int layout_passes() const { return layout_passes_; }
  const std::vector<SnapshotRun>& runs() const { return runs_; }
  bool cancelled() const { return cancelled_; }

  // Heading content-row spans, indexed by markdown heading occurrence ordinal:
  // entry i is {first_row, last_row} (inclusive). Empty when the tree was not
  // built by a snapshot-recording renderer.
  const std::vector<std::pair<int, int>>& heading_spans() const {
    return heading_spans_;
  }

  // Plain text of one content row: run text placed at its columns, spaces
  // elsewhere. Returns an empty string for out-of-range rows.
  std::string RowText(int row) const;

  // Sparse text of one content row: run text concatenated in column order with
  // unwritten gaps skipped. This matches the reference extraction
  // (RenderTextRows), which concatenates only written screen cells.
  std::string RowSparseText(int row) const;

  // Plain text for rows [first, last). Only runs indexed to those rows are
  // visited, so the cost is independent of the document height.
  std::vector<std::string> Rows(int first, int last) const;

  // Sparse text for every content row, trimmed to the last row that carries a
  // non-space glyph. This is the displayed-text row set search matches against
  // and equals the reference RenderTextRows output for text, borders and
  // rules. A row that is non-blank only because of a background fill (no
  // glyph) is trimmed here but kept by the reference; that cannot change
  // matches, since such a row has no characters to match.
  std::vector<std::string> TextRows() const;

  // Build-time hook used by SnapshotText; not for general use.
  bool recording() const { return recording_; }
  void AddRun(const SnapshotRun& run);
  void AddHeadingSpan(int ordinal, int first_row, int last_row);

 private:
  int width_ = 0;
  int height_ = 0;
  int layout_passes_ = 0;
  bool recording_ = false;
  bool cancelled_ = false;
  std::vector<SnapshotRun> runs_;
  std::vector<std::vector<int>> row_runs_;  // row -> indices into runs_
  std::vector<std::pair<int, int>> heading_spans_;
};

}  // namespace markit

#endif  // MARKIT_LAYOUT_SNAPSHOT_HPP
