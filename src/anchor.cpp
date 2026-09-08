// Implementation of the section-scoped content anchor. See anchor.hpp.
#include "anchor.hpp"

#include <algorithm>  // for clamp, count_if, max
#include <cmath>      // for llround
#include <cstdlib>    // for abs
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
int NaturalWidth(ftxui::Element element, int viewport_width) {
  element->ComputeRequirement();
  return std::clamp(std::max(1, element->requirement().min_x), viewport_width,
                    8192);
}

// Render the tree offscreen at `width`, returning one Row per content row
// (same content-height semantics as the scroller's wrap measurement: rows
// through the last non-blank one; interior blanks kept for offset math).
std::vector<Row> RenderRows(ftxui::Element element, int width) {
  element->ComputeRequirement();
  const int seed = std::max(1, element->requirement().min_y);
  int cap = std::clamp(seed, 256, 65536);
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
    bool bold = false;
    bool seen_text = false;
    for (int col = 0; col < width; ++col) {
      const ftxui::Cell& cell = screen.CellAt(col, row);
      raw += cell.character;
      if (!seen_text && cell.character != " " && !cell.character.empty()) {
        seen_text = true;
        bold = cell.bold;
      }
    }
    rows.push_back(Row{Normalize(raw), bold});
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

}  // namespace

int MapTogglePosition(const ftxui::Element& old_tree,
                      const ftxui::Element& new_tree,
                      const std::vector<Heading>& headings, int old_selected,
                      int viewport_width, int viewport_height,
                      bool old_is_scroll) {
  if (!old_tree || !new_tree || viewport_width < 1 || viewport_height < 1) {
    return 0;
  }
  // Scroll trees render at natural width (same rows, full text); wrap trees
  // must render at the viewport width (wrapping is width-dependent).
  const int old_width =
      old_is_scroll ? NaturalWidth(old_tree, viewport_width) : viewport_width;
  const int new_width =
      old_is_scroll ? viewport_width : NaturalWidth(new_tree, viewport_width);
  const std::vector<Row> old_rows = RenderRows(old_tree, old_width);
  const std::vector<Row> new_rows = RenderRows(new_tree, new_width);
  if (old_rows.empty() || new_rows.empty()) {
    return 0;
  }
  const int new_max =
      std::max(0, static_cast<int>(new_rows.size()) - viewport_height);
  const int old_max =
      std::max(0, static_cast<int>(old_rows.size()) - viewport_height);

  // Capture: nearest non-blank row at/above the old offset + reapply delta.
  // Blank rows carry no words, so they can never fingerprint.
  int idx = std::clamp(old_selected, 0, static_cast<int>(old_rows.size()) - 1);
  while (idx > 0 && old_rows[idx].text.empty()) {
    --idx;
  }
  const int delta = old_selected - idx;
  const std::string fp = Fingerprint(old_rows[idx].text);

  const std::vector<int> old_bounds = FindHeadingRows(old_rows, headings);
  const std::vector<int> new_bounds = FindHeadingRows(new_rows, headings);

  // Heading anchor: the row opens its section, so switch exactly there by
  // occurrence rank. (delta is 0 here: heading rows are never blank.)
  for (std::size_t k = 0; k < old_bounds.size(); ++k) {
    if (old_bounds[k] == idx) {
      const int hit = (k < new_bounds.size())
                          ? new_bounds[k]
                          : Proportional(old_selected, old_max, new_max);
      return std::clamp(hit + delta, 0, new_max);
    }
  }

  // Section-scoped content match.
  const int section = SectionId(old_bounds, idx);
  const int sec_start =
      (section == 0) ? 0
      : (section - 1 < static_cast<int>(new_bounds.size())
             ? new_bounds[section - 1]
             : static_cast<int>(new_rows.size()));
  const int sec_end =
      (section < static_cast<int>(new_bounds.size()))
          ? new_bounds[section]
          : static_cast<int>(new_rows.size());

  const int est = Proportional(old_selected, old_max, new_max);
  int best = -1;
  if (!fp.empty()) {
    for (int r = sec_start; r < sec_end; ++r) {
      // Scroll -> wrap: the line's first fragment overlaps the fingerprint
      // prefix. Wrap -> scroll: the owning full line contains the fragment
      // fingerprint (visible thanks to the natural-width render above).
      const bool hit = old_is_scroll ? OverlapPrefix(new_rows[r].text, fp)
                                     : ContainsWordSeq(new_rows[r].text, fp);
      if (hit && (best < 0 || std::abs(r - est) < std::abs(best - est))) {
        best = r;
      }
    }
  }
  const int pos = (best >= 0) ? best : est;
  return std::clamp(pos + delta, 0, new_max);
}

}  // namespace markit
