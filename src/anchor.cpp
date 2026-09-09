// Implementation of the section-scoped content anchor. See anchor.hpp.
#include "anchor.hpp"

#include <algorithm>  // for clamp, count_if, max
#include <cmath>      // for llround
#include <cstdlib>    // for abs
#include <optional>   // for optional
#include <string>     // for string, to_string
#include <vector>     // for vector

#include <ftxui/dom/node.hpp>           // for Render
#include <ftxui/screen/screen.hpp>      // for Screen, Cell
#include <ftxui/screen/terminal.hpp>    // for Dimension
#include <ftxui/screen/color.hpp>       // for Color

namespace markit {

namespace {

// Words carried from the anchor row into the fingerprint.
constexpr int kAnchorWords = 10;

struct Row {
  std::string text;  // trimmed, internal whitespace collapsed to one space.
  bool bold = false;  // first non-blank cell bold (headings render bold).
  bool clipped = false;  // non-blank content reaches the last column: text
                         // past the render width may be missing.
};

bool IsBlankCell(const ftxui::Cell& cell) {
  return (cell.character == " " || cell.character.empty()) &&
         cell.background_color == ftxui::Color::Default;
}

// Collapse leading/trailing whitespace and internal runs to single spaces.
std::string Normalize(const std::string& s) {
  std::string out;
  bool pending_space = false;
  bool started = false;
  for (char c : s) {
    const bool ws = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (ws) {
      if (started) {
        pending_space = true;
      }
      continue;
    }
    if (pending_space) {
      out += ' ';
      pending_space = false;
    }
    out += c;
    started = true;
  }
  return out;
}

std::vector<std::string> SplitWords(const std::string& s) {
  std::vector<std::string> words;
  std::string cur;
  for (char c : s) {
    if (c == ' ') {
      if (!cur.empty()) {
        words.push_back(cur);
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) {
    words.push_back(cur);
  }
  return words;
}

std::string JoinWords(const std::vector<std::string>& words, int count) {
  std::string out;
  const int n = std::min<int>(count, words.size());
  for (int i = 0; i < n; ++i) {
    if (i > 0) {
      out += ' ';
    }
    out += words[i];
  }
  return out;
}

// First up-to-10 words; short rows contribute their whole trimmed text.
std::string Fingerprint(const std::string& normalized) {
  return JoinWords(SplitWords(normalized), kAnchorWords);
}

bool StartsWithWord(const std::string& text, const std::string& fp) {
  if (fp.empty() || text.size() < fp.size()) {
    return false;
  }
  if (text.compare(0, fp.size(), fp) != 0) {
    return false;
  }
  return text.size() == fp.size() || text[fp.size()] == ' ';
}

bool ContainsWordSeq(const std::string& text, const std::string& fp) {
  if (fp.empty()) {
    return false;
  }
  std::size_t pos = 0;
  while ((pos = text.find(fp, pos)) != std::string::npos) {
    const bool left_ok = (pos == 0 || text[pos - 1] == ' ');
    const std::size_t end = pos + fp.size();
    const bool right_ok = (end >= text.size() || text[end] == ' ');
    if (left_ok && right_ok) {
      return true;
    }
    ++pos;
  }
  return false;
}

// Word-boundary prefix overlap in either direction: the row starts with the
// fingerprint (scroll -> wrap, fingerprint fits the first fragment) or the
// fingerprint starts with the row (fingerprint longer than a narrow first
// fragment). Ranks candidates later by distance to the proportional
// estimate, so short rows sharing leading words cannot hijack the match.
bool OverlapPrefix(const std::string& row, const std::string& fp) {
  if (fp.empty() || row.empty()) {
    return false;
  }
  return StartsWithWord(row, fp) || StartsWithWord(fp, row);
}

// Natural (unclipped) content width, capped: scroll-mode rows never split,
// so rendering a scroll tree wide keeps row indices while exposing full
// line text for fingerprints past the viewport edge.
int NaturalWidth(int min_x, int viewport_width) {
  return std::clamp(std::max(1, min_x), viewport_width, 8192);
}

// Seed cap for the offscreen render: the unwrapped requirement scaled by the
// reflow ratio (narrow widths multiply rows), plus viewport slack. The old
// seed (exactly min_y) guaranteed a wasted grow-and-re-render pass whenever
// content filled the screen, because a full screen is indistinguishable from
// a truncated one; the slack makes the common case a single pass while the
// growth loop below stays as the correctness backstop.
int SeedCap(int min_y, int viewport_height, int ratio) {
  const long long est =
      static_cast<long long>(min_y) * ratio + viewport_height + 1;
  return std::clamp<long long>(est, 256, 65536);
}

// Render the tree offscreen at `width`, returning one Row per content row
// (same content-height semantics as the scroller's wrap measurement: rows
// through the last non-blank one; interior blanks kept for offset math).
// `min_y`/`min_x` must come from a prior ComputeRequirement on the tree.
// `may_reflow` selects the seed: wrap trees multiply rows at narrow widths
// (scale by the reflow ratio), scroll trees never split rows (no scaling).
std::vector<Row> RenderRows(ftxui::Element element, int width, int min_y,
                            int min_x, int viewport_height, bool may_reflow) {
  const int ratio = may_reflow ? std::max(1, (min_x + width - 1) / width) : 1;
  int cap = SeedCap(min_y, viewport_height, ratio);
  int last = -1;
  ftxui::Screen screen =
      ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                            ftxui::Dimension::Fixed(cap));
  for (;;) {
    ftxui::Render(screen, element);
    last = -1;
    for (int row = 0; row < cap; ++row) {
      for (int col = 0; col < width; ++col) {
        if (!IsBlankCell(screen.CellAt(col, row))) {
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
  std::vector<Row> rows;
  for (int row = 0; row <= last; ++row) {
    std::string raw;
    raw.reserve(static_cast<size_t>(width));
    bool bold = false;
    bool seen_text = false;
    bool edge_mark = false;
    for (int col = 0; col < width; ++col) {
      const ftxui::Cell& cell = screen.CellAt(col, row);
      const std::string& ch = cell.character;
      raw += ch;
      if (!seen_text && ch != " " && !ch.empty()) {
        seen_text = true;
        bold = cell.bold;
      }
      if (col == width - 1) {
        edge_mark = (ch != " " && !ch.empty()) ||
                    cell.background_color != ftxui::Color::Default;
      }
    }
    rows.push_back(Row{Normalize(raw), bold, seen_text && edge_mark});
  }
  return rows;
}

// Locate heading rows in order: row contains the heading's fingerprint and is
// bold. Each search starts after the previous hit, so duplicate titles map by
// occurrence rank. Misses are skipped without consuming position.
std::vector<int> FindHeadingRows(const std::vector<Row>& rows,
                                 const std::vector<Heading>& headings) {
  std::vector<int> found;
  std::size_t pos = 0;
  for (const Heading& h : headings) {
    const std::string fp = Fingerprint(Normalize(h.text));
    if (fp.empty()) {
      continue;
    }
    for (std::size_t r = pos; r < rows.size(); ++r) {
      if (rows[r].bold && ContainsWordSeq(rows[r].text, fp)) {
        found.push_back(static_cast<int>(r));
        pos = r + 1;
        break;
      }
    }
  }
  return found;
}

// Section id = number of heading boundaries at or above the row. The
// preamble (before the first heading) is section 0 when headings exist.
int SectionId(const std::vector<int>& boundaries, int row) {
  int id = 0;
  for (int b : boundaries) {
    if (b <= row) {
      ++id;
    } else {
      break;
    }
  }
  return id;
}

int Proportional(int old_selected, int old_max, int new_max) {
  if (old_max <= 0 || new_max <= 0) {
    return 0;
  }
  const double est =
      static_cast<double>(old_selected) / old_max * new_max;
  return std::clamp(static_cast<int>(std::llround(est)), 0, new_max);
}

int ClampMax(const std::vector<Row>& rows, int viewport_height) {
  return std::max(0, static_cast<int>(rows.size()) - viewport_height);
}

// Section-scoped content match over one pair of row sets. Returns the best
// row or -1; `hits` counts matches and `clean_hit` is set when a match on a
// fully-visible (unclipped) row exists. Clipped rows may hide text, so only
// clean hits are conclusive.
struct SectionMatch {
  int row = -1;
  int hits = 0;
  bool clean_hit = false;
};

SectionMatch MatchSection(const std::vector<Row>& new_rows,
                          const std::vector<int>& new_bounds, int section,
                          const std::string& fp, int est, bool old_is_scroll) {
  SectionMatch m;
  const int sec_start =
      (section == 0) ? 0
      : (section - 1 < static_cast<int>(new_bounds.size())
             ? new_bounds[section - 1]
             : static_cast<int>(new_rows.size()));
  const int sec_end =
      (section < static_cast<int>(new_bounds.size()))
          ? new_bounds[section]
          : static_cast<int>(new_rows.size());
  if (fp.empty()) {
    return m;
  }
  for (int r = sec_start; r < sec_end; ++r) {
    // Scroll -> wrap: the line's first fragment overlaps the fingerprint
    // prefix. Wrap -> scroll: the owning full line contains the fragment
    // fingerprint (visible thanks to the natural-width render below).
    const bool hit = old_is_scroll ? OverlapPrefix(new_rows[r].text, fp)
                                   : ContainsWordSeq(new_rows[r].text, fp);
    if (!hit) {
      continue;
    }
    ++m.hits;
    if (!new_rows[r].clipped) {
      m.clean_hit = true;
    }
    if (m.row < 0 || std::abs(r - est) < std::abs(m.row - est)) {
      m.row = r;
    }
  }
  return m;
}

// Heading anchor: the row opens its section, so switch exactly there by
// occurrence rank. Returns the mapped position, or nullopt when the anchor
// row is not a heading boundary.
std::optional<int> HeadingAnchor(const std::vector<int>& old_bounds,
                                 const std::vector<int>& new_bounds, int idx,
                                 int delta, int old_selected, int old_max,
                                 int new_max) {
  for (std::size_t k = 0; k < old_bounds.size(); ++k) {
    if (old_bounds[k] == idx) {
      const int hit = (k < new_bounds.size())
                          ? new_bounds[k]
                          : Proportional(old_selected, old_max, new_max);
      return std::clamp(hit + delta, 0, new_max);
    }
  }
  return std::nullopt;
}

}  // namespace

int MapTogglePosition(const ftxui::Element& old_tree,
                      const ftxui::Element& new_tree,
                      const std::vector<Heading>& headings, int old_selected,
                      int viewport_width, int viewport_height,
                      bool old_is_scroll, int* new_height_out) {
  if (!old_tree || !new_tree || viewport_width < 1 || viewport_height < 1) {
    return 0;
  }
  // One requirement walk per tree: scroll-mode rows never split (hbox), so
  // both trees render narrow first for structure; a scroll tree is widened
  // only when clipping makes the narrow match ambiguous.
  old_tree->ComputeRequirement();
  new_tree->ComputeRequirement();
  const int old_min_x = std::max(1, old_tree->requirement().min_x);
  const int old_min_y = std::max(1, old_tree->requirement().min_y);
  const int new_min_x = std::max(1, new_tree->requirement().min_x);
  const int new_min_y = std::max(1, new_tree->requirement().min_y);

  const std::vector<Row> old_narrow =
      RenderRows(old_tree, viewport_width, old_min_y, old_min_x,
                 viewport_height, /*may_reflow=*/!old_is_scroll);
  const std::vector<Row> new_narrow =
      RenderRows(new_tree, viewport_width, new_min_y, new_min_x,
                 viewport_height, /*may_reflow=*/old_is_scroll);
  if (old_narrow.empty() || new_narrow.empty()) {
    return 0;
  }
  if (new_height_out != nullptr) {
    // Scroll trees keep row indices at any width (no reflow), so the narrow
    // count is the displayed height in both modes.
    *new_height_out = static_cast<int>(new_narrow.size());
  }
  const int new_max = ClampMax(new_narrow, viewport_height);
  const int old_max = ClampMax(old_narrow, viewport_height);

  // Capture: nearest non-blank row at/above the old offset + reapply delta.
  // Blank rows carry no words, so they can never fingerprint.
  int idx = std::clamp(old_selected, 0, static_cast<int>(old_narrow.size()) - 1);
  while (idx > 0 && old_narrow[idx].text.empty()) {
    --idx;
  }
  const int delta = old_selected - idx;
  const std::string fp = Fingerprint(old_narrow[idx].text);
  // A clipped anchor row may hide words past the viewport edge, leaving a
  // short (truncated) fingerprint. A complete fingerprint, or a unique
  // fully-visible hit, decides narrow; anything ambiguous falls through to
  // the exact wide rematch below.
  const bool fp_truncated =
      old_narrow[idx].clipped &&
      SplitWords(old_narrow[idx].text).size() < kAnchorWords;

  std::vector<int> old_bounds = FindHeadingRows(old_narrow, headings);
  std::vector<int> new_bounds = FindHeadingRows(new_narrow, headings);

  // (delta is 0 here: heading rows are never blank.)
  if (auto hit = HeadingAnchor(old_bounds, new_bounds, idx, delta,
                               old_selected, old_max, new_max)) {
    return *hit;
  }

  const int est = Proportional(old_selected, old_max, new_max);
  const int section = SectionId(old_bounds, idx);
  SectionMatch m =
      MatchSection(new_narrow, new_bounds, section, fp, est, old_is_scroll);

  // Narrow verdict: conclusive when the fingerprint is complete, or the
  // single section hit is fully visible. Otherwise widen the clipped side
  // for an exact rematch (same indices: scroll rows never reflow).
  const bool need_wide =
      old_is_scroll ? (fp_truncated && m.hits != 1) : !m.clean_hit;
  if (!need_wide) {
    const int pos = (m.row >= 0) ? m.row : est;
    return std::clamp(pos + delta, 0, new_max);
  }

  std::optional<std::vector<Row>> wide_old;
  std::optional<std::vector<Row>> wide_new;
  if (old_is_scroll) {
    wide_old = RenderRows(old_tree, NaturalWidth(old_min_x, viewport_width),
                          old_min_y, old_min_x, viewport_height,
                          /*may_reflow=*/false);
    if (wide_old->size() != old_narrow.size()) {
      // Row structure changed with width: fall back to proportional.
      return std::clamp(est + delta, 0, new_max);
    }
    old_bounds = FindHeadingRows(*wide_old, headings);
  } else {
    wide_new = RenderRows(new_tree, NaturalWidth(new_min_x, viewport_width),
                          new_min_y, new_min_x, viewport_height,
                          /*may_reflow=*/false);
    if (wide_new->size() != new_narrow.size()) {
      return std::clamp(est + delta, 0, new_max);
    }
    new_bounds = FindHeadingRows(*wide_new, headings);
  }
  const std::vector<Row>& old_rows = wide_old ? *wide_old : old_narrow;
  const std::vector<Row>& new_rows = wide_new ? *wide_new : new_narrow;
  const std::string wide_fp = Fingerprint(old_rows[idx].text);

  if (auto hit = HeadingAnchor(old_bounds, new_bounds, idx, delta,
                               old_selected, old_max, new_max)) {
    return *hit;
  }
  const int wide_section = SectionId(old_bounds, idx);
  SectionMatch wm = MatchSection(new_rows, new_bounds, wide_section, wide_fp,
                                 est, old_is_scroll);
  const int pos = (wm.row >= 0) ? wm.row : est;
  return std::clamp(pos + delta, 0, new_max);
}

}  // namespace markit
