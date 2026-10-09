// Implementation of markdown rendering for markit. See markdown.hpp.
#include "markdown.hpp"

#include "config.hpp"
#include "entity_decode.hpp"
#include "layout_snapshot.hpp"

#include <md4c.h>

#include <ftxui/dom/node.hpp>       // for Node (combining-unit text node)
#include <ftxui/screen/box.hpp>     // for Box (combining-unit text node)
#include <ftxui/screen/screen.hpp>  // for Screen (combining-unit text node)
#include <ftxui/screen/string.hpp>  // for Utf8ToGlyphs (combining-unit split)

#include <algorithm>  // for max
#include <cctype>     // for tolower/isspace/isalnum (HTML tag parsing)
#include <cstdint>    // for uint32_t (UTF-8 validation)
#include <functional>  // for function (cancellation predicate)
#include <memory>     // for make_shared
#include <string>     // for string, to_string
#include <string_view>  // for string_view
#include <utility>    // for move
#include <vector>     // for vector

namespace markit {

namespace {

using ftxui::Color;
using ftxui::Decorator;
using ftxui::Element;
using ftxui::Elements;

// Decode an md4c attribute (e.g. a link/image href or title) exactly once:
// typed MD_TEXT_ENTITY substrings are decoded, everything else is copied
// verbatim. Invalid numeric scalars are flagged so link destinations can be
// rejected rather than rewritten.
DecodedDestination DecodeMdAttribute(const MD_ATTRIBUTE& attr) {
  DecodedDestination result;
  if (attr.text == nullptr) {
    return result;
  }
  if (attr.substr_types == nullptr || attr.substr_offsets == nullptr) {
    result.text.assign(attr.text, attr.size);
    return result;
  }
  // md4c guarantees substr_offsets has one more entry than substr_types and
  // that the final offset equals attr.size, so reading substr_offsets[k + 1]
  // while substr_offsets[k] < attr.size stays in bounds. Each piece is still
  // validated and malformed metadata stops the walk rather than being trusted.
  size_t k = 0;
  while (attr.substr_offsets[k] < attr.size) {
    const size_t start = attr.substr_offsets[k];
    const size_t end = attr.substr_offsets[k + 1];
    if (start > end || end > attr.size) {
      break;  // defensive: malformed metadata; keep what we have.
    }
    const std::string_view piece(attr.text + start, end - start);
    if (attr.substr_types[k] == MD_TEXT_ENTITY) {
      std::string decoded;
      switch (DecodeEntityReference(piece, &decoded)) {
        case EntityDecodeStatus::kNotEntity:
          result.text.append(piece);
          break;
        case EntityDecodeStatus::kInvalid:
          result.invalid = true;
          result.text.append("\xEF\xBF\xBD");
          break;
        case EntityDecodeStatus::kDecoded:
          result.text += decoded;
          break;
      }
    } else {
      result.text.append(piece);
    }
    ++k;
  }
  return result;
}

// True when `s` is a safe OSC 8 hyperlink target: well-formed UTF-8 with no
// C0 (U+0000-001F), DEL (U+007F) or C1 (U+0080-009F) code points. Entity
// decoding can introduce such controls that never appeared in the source
// bytes, so destinations are validated after decoding, never by stripping or
// rewriting them. Valid non-ASCII links are preserved.
bool IsSafeLinkDestination(std::string_view s) {
  static const uint32_t kMinCodePoint[5] = {0, 0, 0x80, 0x800, 0x10000};
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0;
    size_t len = 0;
    if (lead < 0x80) {
      cp = lead;
      len = 1;
    } else if ((lead & 0xE0) == 0xC0) {
      cp = lead & 0x1F;
      len = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      cp = lead & 0x0F;
      len = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      cp = lead & 0x07;
      len = 4;
    } else {
      return false;  // stray continuation byte or invalid lead byte.
    }
    if (i + len > n) {
      return false;  // truncated sequence.
    }
    for (size_t k = 1; k < len; ++k) {
      const unsigned char cont = static_cast<unsigned char>(s[i + k]);
      if ((cont & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (cont & 0x3F);
    }
    if (len >= 2 && cp < kMinCodePoint[len]) {
      return false;  // overlong encoding.
    }
    if (cp > 0x10FFFF) {
      return false;  // beyond Unicode range.
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      return false;  // UTF-8-encoded surrogate: not a valid scalar.
    }
    if (cp <= 0x1F || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
      return false;  // C0 / DEL / C1.
    }
    i += len;
  }
  return true;
}

// Decode one UTF-8 code point at `start`. Returns false for a malformed or
// truncated sequence; `*len` is the byte length on success.
bool DecodeUtf8(std::string_view s, size_t start, uint32_t* cp, size_t* len) {
  const unsigned char b0 = static_cast<unsigned char>(s[start]);
  uint32_t value = 0;
  size_t need = 0;
  if (b0 < 0x80) {
    value = b0;
    need = 1;
  } else if ((b0 & 0xE0) == 0xC0) {
    value = b0 & 0x1F;
    need = 2;
  } else if ((b0 & 0xF0) == 0xE0) {
    value = b0 & 0x0F;
    need = 3;
  } else if ((b0 & 0xF8) == 0xF0) {
    value = b0 & 0x07;
    need = 4;
  } else {
    return false;
  }
  if (start + need > s.size()) {
    return false;
  }
  for (size_t k = 1; k < need; ++k) {
    const unsigned char c = static_cast<unsigned char>(s[start + k]);
    if ((c & 0xC0) != 0x80) {
      return false;
    }
    value = (value << 6) | (c & 0x3F);
  }
  *cp = value;
  *len = need;
  return true;
}

bool IsControlCodePoint(uint32_t cp) {
  return cp == 0 || cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F);
}

// Expand horizontal tabs in a code/verbatim line to four-display-column stops
// measured from logical column zero of the source line (the surrounding border
// is not part of the line). Columns advance by each code point's display width
// (wide glyphs two, combining marks zero), not by leading-byte count. Callers
// expand before wrapping so a tab never counts as one column or wraps as a bare
// separator; source data is unchanged.
std::string ExpandTabs(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  int col = 0;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '\t') {
      const int spaces = 4 - (col % 4);
      out.append(static_cast<size_t>(spaces), ' ');
      col += spaces;
      ++i;
      continue;
    }
    if (s[i] == '\n') {
      out.push_back('\n');
      col = 0;  // tab stops reset at source newlines.
      ++i;
      continue;
    }
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0;
    size_t len = 0;
    if (!DecodeUtf8(s, i, &cp, &len)) {
      len = 1;  // malformed/truncated: copy one byte, never a tab/newline.
    }
    const std::string_view cp_bytes = s.substr(i, len);
    out.append(cp_bytes);
    col += ftxui::string_width(cp_bytes);
    i += len;
  }
  return out;
}

// Number of leading bytes of `s` that FTXUI's Utf8ToGlyphs would discard: a
// combining mark or control code point with no preceding glyph to attach to.
// Querying the renderer's own glyph grouping keeps the combining-unit split
// exact instead of duplicating (and drifting from) FTXUI's Unicode tables.
size_t LeadingDroppedGlyphBytes(std::string_view s) {
  if (s.empty()) {
    return 0;
  }
  // Fast path: a leading base glyph (a non-control code point of nonzero
  // width) is never dropped, so no glyph grouping is needed.
  uint32_t cp = 0;
  size_t len = 0;
  if (DecodeUtf8(s, 0, &cp, &len) && !IsControlCodePoint(cp) &&
      ftxui::string_width(s.substr(0, len)) > 0) {
    return 0;
  }
  const std::vector<std::string> glyphs = ftxui::Utf8ToGlyphs(s);
  if (glyphs.empty()) {
    return s.size();  // every code point is dropped (no base to attach to).
  }
  const size_t first = s.find(glyphs.front());
  return first == std::string_view::npos ? 0 : first;
}

// True when `s` contains a combining mark (a code point FTXUI would attach to
// the preceding glyph). Combining marks are always non-ASCII, so pure ASCII
// text skips the scan.
bool ContainsCombiningMark(std::string_view s) {
  bool non_ascii = false;
  for (const unsigned char c : s) {
    if (c >= 0x80) {
      non_ascii = true;
      break;
    }
  }
  if (!non_ascii) {
    return false;
  }
  size_t i = 0;
  while (i < s.size()) {
    uint32_t cp = 0;
    size_t len = 0;
    if (!DecodeUtf8(s, i, &cp, &len)) {
      ++i;
      continue;
    }
    if (!IsControlCodePoint(cp) && ftxui::string_width(s.substr(i, len)) == 0) {
      return true;
    }
    i += len;
  }
  return false;
}

// Text node that groups each base code point with its following combining
// marks before cells are laid out. FTXUI's text() emits a wide base's
// continuation cell immediately, so a combining mark after a wide base lands in
// the continuation cell instead of the base cell. This node keeps the whole
// base-plus-marks unit in the leading cell and reserves an empty continuation,
// matching the combining-unit contract. Newlines still start new rows, like
// text(). It is used only for fragments that contain a combining mark; ordinary
// text keeps FTXUI's text() (and its selection support).
class CombiningText : public ftxui::Node {
 public:
  explicit CombiningText(std::string_view text,
                         LayoutSnapshot* snapshot = nullptr)
      : snapshot_(snapshot) {
    Build(text);
    requirement_.min_x = max_width_;
    requirement_.min_y = static_cast<int>(lines_.size());
  }

  void ComputeRequirement() override {}

  void SetBox(ftxui::Box box) override {
    ftxui::Node::SetBox(box);
    if (snapshot_ == nullptr || !snapshot_->recording()) {
      return;
    }
    // Record exactly what Render() writes: each laid-out line's glyphs
    // concatenated. A leading orphan combining mark (dropped by Build) and
    // wide-unit continuation cells contribute nothing, so search never sees
    // undisplayed bytes.
    for (size_t line = 0; line < lines_.size(); ++line) {
      const int y = box.y_min + static_cast<int>(line);
      if (y < box.y_min || y > box.y_max) {
        continue;
      }
      std::string text;
      for (const std::string& glyph : lines_[line]) {
        text += glyph;
      }
      if (!text.empty()) {
        snapshot_->AddRun(SnapshotRun{y, box.x_min, text, false});
      }
    }
  }

  void Render(ftxui::Screen& screen) override {
    const ftxui::Box visible = ftxui::Box::Intersection(screen.stencil, box_);
    if (visible.IsEmpty()) {
      return;
    }
    const size_t first_line = static_cast<size_t>(visible.y_min - box_.y_min);
    const size_t last_line =
        std::min<size_t>(static_cast<size_t>(visible.y_max - box_.y_min + 1),
                         lines_.size());
    for (size_t line = first_line; line < last_line; ++line) {
      const int y = box_.y_min + static_cast<int>(line);
      int x = box_.x_min;
      for (const std::string& glyph : lines_[line]) {
        if (x > visible.x_max) {
          break;
        }
        if (x >= visible.x_min) {
          screen.CellAt(x, y).character = glyph;
        }
        ++x;
      }
    }
  }

 private:
  struct Unit {
    std::string text;
    bool wide;
  };

  void Build(std::string_view text) {
    std::vector<Unit> units;
    auto flush = [&]() {
      lines_.emplace_back();
      std::vector<std::string>& line = lines_.back();
      for (Unit& unit : units) {
        line.push_back(std::move(unit.text));
        if (unit.wide) {
          line.emplace_back();
        }
      }
      max_width_ = std::max(max_width_, static_cast<int>(line.size()));
      units.clear();
    };
    size_t i = 0;
    while (i < text.size()) {
      uint32_t cp = 0;
      size_t len = 0;
      if (!DecodeUtf8(text, i, &cp, &len)) {
        ++i;  // drop an invalid byte, matching Utf8ToGlyphs.
        continue;
      }
      const std::string_view bytes(text.data() + i, len);
      if (cp == '\n') {
        flush();
        i += len;
        continue;
      }
      if (IsControlCodePoint(cp)) {
        i += len;
        continue;
      }
      const int width = ftxui::string_width(bytes);
      if (width == 0) {
        if (!units.empty()) {
          units.back().text.append(bytes);  // combining mark joins its base.
        }
        i += len;
        continue;
      }
      units.push_back(Unit{std::string(bytes), width >= 2});
      i += len;
    }
    flush();
  }

  std::vector<std::vector<std::string>> lines_;
  int max_width_ = 0;
  LayoutSnapshot* snapshot_ = nullptr;
};

// Build a styled text element. When `snapshot` is non-null the element records
// its position during LayoutSnapshot::Build so the document text can be
// recovered without rendering a screen; the combining-unit node is only needed
// for live rendering (a snapshot records the raw base+mark text).
Element MakeText(LayoutSnapshot* snapshot, std::string text,
                 const Decorator& style) {
  Element e;
  if (ContainsCombiningMark(text)) {
    // Keep the combining-unit node for live rendering (it fixes wide-base
    // continuation placement) and let it record the raw base+mark text when a
    // snapshot is active.
    e = std::make_shared<CombiningText>(text, snapshot);
  } else if (snapshot != nullptr) {
    e = SnapshotText(snapshot, std::move(text));
  } else {
    e = ftxui::text(std::move(text));
  }
  if (style) {
    e = style(std::move(e));
  }
  return e;
}

// Horizontal rule / block border helpers that record their positions when a
// snapshot is active, and fall back to the FTXUI primitives otherwise.
Element MakeSeparator(LayoutSnapshot* snapshot) {
  return snapshot != nullptr ? SnapshotSeparator(snapshot) : ftxui::separator();
}

Element MakeBorder(LayoutSnapshot* snapshot, Element child) {
  return snapshot != nullptr ? SnapshotBorder(snapshot, std::move(child))
                             : ftxui::borderLight(std::move(child));
}

// ---------------------------------------------------------------------------
// Renderer: converts md4c SAX events into an FTXUI element tree.
//
// `frames_` is a stack of containers. Inline text is accumulated in the
// topmost *inline-capable* frame (Paragraph, Heading, Item, Cell). Container
// frames accumulate completed child Elements. On leave, an inline-capable
// frame flattens its fragments and attaches to the parent.
// ---------------------------------------------------------------------------

class Renderer {
 public:
  Renderer(const std::string& markdown, const Theme& theme, WrapMode mode)
      : source_(markdown),
        theme_(theme),
        wrap_(mode == WrapMode::Wrap) {}

  explicit Renderer(const std::string& markdown, const Config& config)
      : Renderer(markdown, config.theme, config.horizontal_wrap) {}

  // Record text/rule/border positions into `snapshot` while building. The
  // snapshot must outlive the returned tree.
  void set_snapshot(LayoutSnapshot* snapshot) { snapshot_ = snapshot; }

  // Abort parsing once `cancelled` returns true. The returned tree is partial
  // and `aborted()` is set; callers must discard it.
  void set_cancelled(std::function<bool()> cancelled) {
    cancelled_ = std::move(cancelled);
  }
  bool aborted() const { return aborted_; }

  Element Run() {
    MD_PARSER parser = {};
    parser.abi_version = 0;
    parser.flags =
        MD_DIALECT_GITHUB | MD_FLAG_PERMISSIVEAUTOLINKS;

    frames_.push_back(Frame::Doc());
    parser.enter_block = &Renderer::cb_enter_block;
    parser.leave_block = &Renderer::cb_leave_block;
    parser.enter_span = &Renderer::cb_enter_span;
    parser.leave_span = &Renderer::cb_leave_span;
    parser.text = &Renderer::cb_text;

    const int parse_result =
        md_parse(reinterpret_cast<const MD_CHAR*>(source_.data()),
                 source_.size(), &parser, this);
    if (parse_result != 0) {
      aborted_ = true;
    }

    FlushPendingHtml();  // trailing verbatim HTML coalesced across blocks.
    // A heading at EOF still owes its trailing gap, but there is no next
    // block to take it: drop it (a trailing blank row would be invisible).
    Frame doc = std::move(frames_.back());
    frames_.pop_back();
    if (doc.children.empty()) {
      doc.children.push_back(
          MakeText(snapshot_, "(empty document)", Decorator(nullptr)));
    }
    return ftxui::vbox(std::move(doc.children));
  }

 private:
  enum class Kind {
    Doc,
    Quote,
    List,
    Item,    // inline-capable
    Heading, // inline-capable
    Para,    // inline-capable
    Code,
    Html,
    Table,
    Row,
    Cell,    // inline-capable
  };

  struct Fragment {
    std::string text;
    Decorator style;
    // Identity of the enclosing span stack (e.g. "em", "a:<href>"). Two
    // fragments with equal keys carry the same style even when md4c split
    // the text (soft breaks, entities); Decorator itself is not comparable.
    std::string style_key;
  };

  struct Frame {
    Kind kind;
    unsigned heading_level;
    unsigned col_count;           // table column count
    bool is_header_row;           // row belongs to thead
    bool in_thead = false;        // enclosing Table: within <thead> section
    bool is_task = false;
    char task_mark = ' ';
    char bullet = 0;              // ul marker; 0 means ordered
    unsigned ordered_index = 0;
    char ordered_mark = '.';
    char cell_align = 0;          // MD_ALIGN value for a table cell
    // Trailing gap owed after a heading: the next block in this container
    // inserts one blank row before itself. An HR carries the debt past the
    // rule instead (blank after the rule, none between heading and rule).
    // Only set on Doc/Quote parents; tight Item/Cell content stays compact.
    bool heading_gap = false;
    std::vector<Element> children;
    std::vector<Fragment> inline_;
    std::vector<Element> rows_;      // completed hard-break rows
    std::string text;              // verbatim buffer (code/html)
    std::vector<std::pair<bool, std::vector<Element>>> table_rows;
    std::vector<Element> cells;    // current row cells
    unsigned heading_ordinal = 0;  // markdown heading occurrence (MD_BLOCK_H)

    // Named factories: positional `Frame{Kind::X, ...}` literals depend on
    // field order (the trailing bool is `is_header_row`), so construct
    // frames through these instead.
    static Frame Doc() { return Frame{Kind::Doc, 0, 0, false}; }
    static Frame Para() { return Frame{Kind::Para, 0, 0, false}; }
    static Frame Heading(unsigned level) {
      return Frame{Kind::Heading, level, 0, false};
    }
    static Frame Quote() { return Frame{Kind::Quote, 0, 0, false}; }
    static Frame List() { return Frame{Kind::List, 0, 0, false}; }
    static Frame Item() { return Frame{Kind::Item, 0, 0, false}; }
    static Frame Code() { return Frame{Kind::Code, 0, 0, false}; }
    static Frame Html() { return Frame{Kind::Html, 0, 0, false}; }
    static Frame Table(unsigned cols) {
      return Frame{Kind::Table, 0, cols, false};
    }
    static Frame Row(bool is_header) {
      return Frame{Kind::Row, 0, 0, is_header};
    }
    static Frame Cell(char align, bool is_header) {
      Frame f{Kind::Cell, 0, 0, is_header};
      f.cell_align = align;
      return f;
    }
  };

  // ---- stack helpers ------------------------------------------------------
  Frame& Top() { return frames_.back(); }

  void Push(Frame frame) { frames_.push_back(std::move(frame)); }

  Frame Pop() {
    Frame top = std::move(frames_.back());
    frames_.pop_back();
    return top;
  }

  void Attach(Element e, bool blank_before) {
    Frame& parent = Top();
    if ((parent.kind == Kind::Doc || parent.kind == Kind::Quote) &&
        parent.heading_gap) {
      // A heading owes the next block a blank row. The debt subsumes any
      // leading blank the block would add itself, so H->H gets exactly one.
      parent.heading_gap = false;
      if (!parent.children.empty()) {
        parent.children.push_back(ftxui::text(""));
      }
      parent.children.push_back(std::move(e));
      return;
    }
    if (blank_before && !parent.children.empty()) {
      parent.children.push_back(ftxui::text(""));
    }
    parent.children.push_back(std::move(e));
  }

  // Attach a horizontal rule. When it follows a heading the heading's
  // trailing gap transfers past the rule: no blank between heading and rule,
  // single blank after the rule (consumed by the next block's Attach).
  void AttachHr(Element e) {
    Frame& parent = Top();
    if ((parent.kind == Kind::Doc || parent.kind == Kind::Quote) &&
        parent.heading_gap) {
      parent.children.push_back(std::move(e));
      return;  // keep heading_gap pending for the block after the rule.
    }
    parent.heading_gap = false;
    if (!parent.children.empty()) {
      parent.children.push_back(ftxui::text(""));
    }
    parent.children.push_back(std::move(e));
  }

  // Mark the current container as owing the next block a blank row after a
  // heading. Called after the heading itself attached (which consumed any
  // previous debt), so H->H still yields exactly one gap row.
  void OweHeadingGap() {
    Frame& parent = Top();
    if (parent.kind == Kind::Doc || parent.kind == Kind::Quote) {
      parent.heading_gap = true;
    }
  }

  Decorator ComposedStyle() const {
    Decorator result = nullptr;
    // Decorators are composable; `a | b` yields b(a(e)), so the rightmost
    // (last-entered) decorator is applied outermost, matching span nesting.
    for (const auto& d : span_decorators_) {
      result = (result == nullptr) ? d : (result | d);
    }
    return result;
  }

  const std::string& ComposedStyleKey() const {
    if (!style_key_cache_valid_) {
      style_key_cache_.clear();
      for (const auto& k : span_keys_) {
        if (!style_key_cache_.empty()) {
          style_key_cache_ += '\x1f';
        }
        style_key_cache_ += k;
      }
      style_key_cache_valid_ = true;
    }
    return style_key_cache_;
  }

  void PushSpan(Decorator style, std::string key) {
    span_decorators_.push_back(std::move(style));
    span_keys_.push_back(std::move(key));
    style_key_cache_valid_ = false;
  }

  void PopSpan() {
    if (!span_decorators_.empty()) {
      span_decorators_.pop_back();
    }
    if (!span_keys_.empty()) {
      span_keys_.pop_back();
    }
    style_key_cache_valid_ = false;
  }

  void ResizeSpans(std::size_t size) {
    span_decorators_.resize(size);
    span_keys_.resize(size);
    style_key_cache_valid_ = false;
  }

  void EmitInline(std::string_view text) {
    if (text.empty()) {
      return;
    }
    Frame& top = Top();
    // Only inline-capable frames collect text; a frame-stack desync would
    // otherwise drop text silently, so abort if we hit a container frame.
    if (top.kind != Kind::Item && top.kind != Kind::Heading &&
        top.kind != Kind::Para && top.kind != Kind::Cell) {
      return;
    }
    top.inline_.push_back(
        Fragment{std::string(text), ComposedStyle(), ComposedStyleKey()});
  }

  // Split a row's fragments into word elements for wrap mode. A "word" is a
  // maximal run of non-space text; it may span several styled fragments (e.g.
  // `[a](url)b`), whose pieces are glued into one element so the wrap layout
  // never breaks inside a word. Inter-word whitespace is appended to the
  // *preceding* word so a soft wrap consumes the separator instead of charging
  // its width against the next word (which used to clip a word that otherwise
  // fit). Same-row separator text and styling are preserved: a separator keeps
  // the run's style only when it is internal to one styled run (same key
  // before and after); a gap between different runs stays plain. Source
  // indentation leads the first visual row. Overlong tokens are never split
  // mid-word; they clip. An all-space row keeps one atom so its height
  // survives.
  Element WrapRow(const std::vector<Fragment>& fragments) {
    struct Piece {
      std::string text;
      Decorator style;
      std::string key;
      bool space;
    };
    auto is_space = [](char c) {
      return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };

    // Flatten fragments into alternating whitespace/non-whitespace runs. Tabs
    // and newlines normalize to spaces so no text() element ever contains
    // "\n" (hflow would break the row on it, stranding styled spaces).
    std::vector<Piece> runs;
    for (const auto& frag : fragments) {
      size_t i = 0;
      const size_t n = frag.text.size();
      while (i < n) {
        const bool sp = is_space(frag.text[i]);
        size_t j = i;
        while (j < n && is_space(frag.text[j]) == sp) {
          ++j;
        }
        Piece p;
        p.text = sp ? std::string(j - i, ' ') : frag.text.substr(i, j - i);
        p.style = frag.style;
        p.key = frag.style_key;
        p.space = sp;
        runs.push_back(std::move(p));
        i = j;
      }
    }

    Elements words;
    std::vector<Piece> cur;
    auto finish = [&]() {
      if (cur.empty()) {
        return;
      }
      Elements pieces;
      pieces.reserve(cur.size());
      for (auto& piece : cur) {
        pieces.push_back(MakeText(snapshot_, std::move(piece.text), piece.style));
      }
      words.push_back(pieces.size() == 1 ? std::move(pieces[0])
                                         : ftxui::hbox(std::move(pieces)));
      cur.clear();
    };

    size_t i = 0;
    const size_t m = runs.size();
    // Leading whitespace is source indentation: it stays on the first visual
    // row, glued ahead of the first word.
    if (i < m && runs[i].space) {
      cur.push_back(std::move(runs[i]));
      ++i;
    }
    while (i < m) {
      if (!runs[i].space) {
        cur.push_back(std::move(runs[i]));
        ++i;
        // Append the following separator (if any) to this atom. It keeps the
        // run's style only when it is internal to one styled run.
        if (i < m && runs[i].space) {
          std::string after_key;
          bool has_after = false;
          for (size_t k = i + 1; k < m; ++k) {
            if (!runs[k].space) {
              after_key = runs[k].key;
              has_after = true;
              break;
            }
          }
          const std::string before_key = cur.back().key;
          const std::string space_key = runs[i].key;
          const bool internal =
              has_after && space_key == before_key && space_key == after_key;
          Piece sep = std::move(runs[i]);
          if (!internal) {
            sep.style = Decorator(nullptr);
            sep.key.clear();
          }
          cur.push_back(std::move(sep));
          ++i;
        }
        finish();
      } else {
        // A whitespace run with no preceding word (e.g. after a consumed
        // leading run): keep it as its own atom so height is preserved.
        cur.push_back(std::move(runs[i]));
        ++i;
        finish();
      }
    }
    finish();
    return ftxui::hflow(std::move(words));
  }

  // Rebuild base-plus-combining units across styled-fragment boundaries. md4c
  // can deliver a combining mark (or a decoded combining entity) at the start
  // of a new fragment; FTXUI drops a combining mark that begins its own text
  // node, so the mark must move into the preceding base fragment. The base's
  // style and hyperlink own the whole unit; a differently styled or linked mark
  // never overrides the base. Leading controls move the same way (they render
  // as nothing either way). A mark with no preceding base keeps its current
  // behavior. This is the focused combining-sequence scope, not full
  // extended-grapheme support.
  void CoalesceCombiningUnits(std::vector<Fragment>& fragments) {
    std::vector<Fragment> out;
    out.reserve(fragments.size());
    for (auto& frag : fragments) {
      if (frag.text.empty()) {
        continue;
      }
      const size_t lead = LeadingDroppedGlyphBytes(frag.text);
      if (lead > 0 && !out.empty()) {
        out.back().text.append(frag.text, 0, lead);
        frag.text.erase(0, lead);
      }
      if (!frag.text.empty()) {
        out.push_back(std::move(frag));
      }
    }
    fragments = std::move(out);
  }

  Element FlattenInline(Frame& frame) {
    std::vector<Fragment> fragments = std::move(frame.inline_);
    frame.inline_.clear();
    if (fragments.empty()) {
      return ftxui::text("");
    }
    CoalesceCombiningUnits(fragments);

    // Rows built from fragments: a lone styled span (e.g. an inline code row)
    // must stay wrapped in its own element, otherwise becoming a direct vbox
    // child lets its background decorator paint the full row width instead of
    // just the text. Wrap mode instead word-splits the fragments so the row
    // reflows to the available width (table cells stay single-line
    // regardless).
    if (wrap_ && frame.kind != Kind::Cell) {
      return WrapRow(fragments);
    }
    Elements items;
    items.reserve(fragments.size());
    for (auto& frag : fragments) {
      items.push_back(MakeText(snapshot_, std::move(frag.text), frag.style));
    }
    return ftxui::hbox(std::move(items));
  }

  // End the current inline row (a hard break) and start a new one.
  void FlushRow(Frame& frame) {
    if (frame.inline_.empty()) {
      return;
    }
    frame.rows_.push_back(FlattenInline(frame));
  }

  // Combine completed hard-break rows with any trailing inline fragments into
  // a single block element (vbox of rows, or a lone row when there is one).
  Element InlineBlocks(Frame& frame) {
    if (!frame.inline_.empty()) {
      frame.rows_.push_back(FlattenInline(frame));
    }
    if (frame.rows_.empty()) {
      return ftxui::text("");
    }
    if (frame.rows_.size() == 1) {
      return std::move(frame.rows_[0]);
    }
    return ftxui::vbox(std::move(frame.rows_));
  }

  // ---- HTML rendering ---------------------------------------------------
  // md4c delivers raw HTML as opaque MD_TEXT_HTML chunks (it never parses
  // tags). Inside an HTML block, interpret a small subset of tags and map
  // them onto the same decorators/frames markdown uses; anything else
  // (<pre>, <script>/<style>, unknown tags) stays verbatim in a code box.
  // md4c splits one HTML run into several MD_BLOCK_HTML blocks at blank
  // lines, so pure-verbatim frames coalesce into pending_html_ and attach
  // as a single box instead of one box per block.

  static std::string ToLower(std::string_view input) {
    std::string s(input);
    for (char& c : s) {
      c = static_cast<char>(
          std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
  }

  static bool IsHtmlSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }

  static bool IsAllSpace(std::string_view s) {
    for (char c : s) {
      if (!IsHtmlSpace(c)) {
        return false;
      }
    }
    return true;
  }

  // Collapse HTML whitespace: any run of space/tab/CR/LF becomes one space,
  // like browsers (and like md4c soft breaks, which arrive as " "). Raw
  // newlines must never reach a text() element: hflow treats "\n" as a row
  // break, stranding styled (underlined/hyperlink) spaces on the next visual
  // row as a phantom underlined line carrying the link URL.
  static std::string CollapseHtmlSpace(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool in_space = false;
    for (char c : s) {
      if (IsHtmlSpace(c)) {
        if (!in_space) {
          out += ' ';
          in_space = true;
        }
      } else {
        out += c;
        in_space = false;
      }
    }
    return out;
  }

  static bool IsTagChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' ||
           c == ':' || c == '_';
  }

  // Nearest enclosing Html frame, or nullptr outside an HTML block.
  Frame* HtmlFrame() {
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
      if (it->kind == Kind::Html) {
        return &*it;
      }
    }
    return nullptr;
  }

  // Flush coalesced verbatim HTML (from consecutive HTML blocks split at
  // blank lines) as one box. Called when non-HTML content starts and at the
  // end of the document.
  void FlushPendingHtml() {
    if (pending_html_.empty() || IsAllSpace(pending_html_)) {
      pending_html_.clear();
      return;
    }
    Attach(CodeElement(pending_html_, wrap_), true);
    pending_html_.clear();
  }

  static bool IsInlineCapable(Kind kind) {
    return kind == Kind::Item || kind == Kind::Heading ||
           kind == Kind::Para || kind == Kind::Cell;
  }

  // Open an inline-capable frame for HTML text when tags/text arrive with a
  // container (Html/Quote/List) on top; reuse it when one is already open.
  // Other frames (Table/Row/Code/Doc) cannot occur inside an HTML block, so
  // text there is dropped rather than corrupting the stack.
  void EnsureHtmlPara() {
    Kind kind = Top().kind;
    if (IsInlineCapable(kind)) {
      return;
    }
    if (kind != Kind::Html && kind != Kind::Quote && kind != Kind::List) {
      return;
    }
    Push(Frame::Para());
  }

  void CloseHtmlPara() {
    if (summary_open_) {
      CloseHtmlSummary();  // an open <summary> keeps its marker.
      return;
    }
    if (Top().kind != Kind::Para) {
      html_align_ = 0;
      return;
    }
    Frame top = Pop();
    if (top.inline_.empty() && top.rows_.empty()) {
      html_align_ = 0;  // empty <p></p>: no blank row.
      return;
    }
    Element e = InlineBlocks(top);
    if (html_align_ == 'c') {
      e = ftxui::hcenter(std::move(e));
    }
    html_align_ = 0;
    Attach(std::move(e), true);
  }

  void CloseHtmlHeading() {
    if (Top().kind != Kind::Heading) {
      return;
    }
    Frame top = Pop();
    Element e = InlineBlocks(top);
    e = ftxui::bold(std::move(e)) | ftxui::color(HeadingColor(top.heading_level));
    Attach(std::move(e), true);
    OweHeadingGap();
  }

  // Close a <summary> paragraph: disclosure marker (open-state of the
  // innermost <details>) + bold, attached like any other block. Called
  // directly for </summary> and via CloseHtmlPara for any other boundary.
  void CloseHtmlSummary() {
    if (!summary_open_ || Top().kind != Kind::Para) {
      summary_open_ = false;
      return;
    }
    Frame top = Pop();
    summary_open_ = false;
    if (top.inline_.empty() && top.rows_.empty()) {
      return;  // empty <summary></summary>: no row.
    }
    const bool open = !details_stack_.empty() && details_stack_.back() == 'o';
    top.inline_.insert(top.inline_.begin(),
                       Fragment{open ? "▾ " : "▸ ", Decorator(nullptr), {}});
    Attach(ftxui::bold(InlineBlocks(top)), true);
  }

  // Box the Html frame's verbatim remainder, preserving document order: at
  // the Html top level it joins the frame's children (boxed together with
  // any coalesced neighbours at leave); nested in a quote/list it attaches
  // to the current container so it stays inside the structure.
  void FlushHtmlText() {
    Frame* h = HtmlFrame();
    if (h == nullptr || h->text.empty() || IsAllSpace(h->text)) {
      if (h != nullptr) {
        h->text.clear();
      }
      return;
    }
    Element box = CodeElement(h->text, wrap_);
    h->text.clear();
    if (&Top() == h) {
      h->children.push_back(std::move(box));
    } else {
      Attach(std::move(box), true);
    }
  }

  void HtmlInlineText(std::string_view text) {
    if (text.empty()) {
      return;
    }
    // Interpreted HTML text: decode complete entity references only after the
    // markup boundaries are recognized, so decoded quotes/angle brackets can
    // never become new syntax. Verbatim HTML never reaches here.
    const std::string decoded = DecodeEntityText(text);
    const std::string collapsed = CollapseHtmlSpace(decoded);
    if (collapsed.empty()) {
      return;
    }
    // Formatting indent right after <a> is not link content: skip
    // whitespace-only fragments until the first real content arrives. The key
    // must match (a nested span means real content started elsewhere).
    if (!link_ws_key_.empty()) {
      if (IsAllSpace(collapsed) && ComposedStyleKey() == link_ws_key_) {
        return;
      }
      link_ws_key_.clear();
    }
    // md4c splits one whitespace run into several callbacks (e.g. "\n" then
    // "  "), so consecutive whitespace-only fragments with the same style
    // would accumulate into double spaces. One suffices (HTML collapses).
    if (IsAllSpace(collapsed) && !frames_.empty()) {
      const Frame& top = Top();
      if (!top.inline_.empty()) {
        const Fragment& last = top.inline_.back();
        if (IsAllSpace(last.text) && last.style_key == ComposedStyleKey()) {
          return;
        }
      }
    }
    if (IsInlineCapable(Top().kind)) {
      EmitInline(collapsed);
      return;
    }
    if (IsAllSpace(collapsed)) {
      return;  // indentation/newlines between block tags.
    }
    const size_t before = frames_.size();
    EnsureHtmlPara();
    if (frames_.size() == before) {
      return;  // unexpected frame; drop rather than corrupt the stack.
    }
    EmitInline(collapsed);
  }

  // A <br> break: end the current row, forcing a blank row when the
  // paragraph is empty so consecutive breaks stay visible.
  void HtmlBreak() {
    EnsureHtmlPara();
    if (!IsInlineCapable(Top().kind)) {
      return;
    }
    Frame& para = Top();
    if (para.inline_.empty()) {
      para.rows_.push_back(ftxui::text(""));
    } else {
      FlushRow(para);
    }
  }

  // Verbatim fallback: styled inline in running text, boxed at block level.
  void HtmlVerbatim(std::string_view text) {
    if (text.empty()) {
      return;
    }
    Frame& top = Top();
    if (IsInlineCapable(top.kind)) {
      PushSpan(InlineCodeStyle(), "code");
      EmitInline(ExpandTabs(text));
      PopSpan();
      return;
    }
    if (top.kind == Kind::Html) {
      top.text.append(text.data(), text.size());
      return;
    }
    Attach(CodeElement(std::string(text), wrap_), true);
  }

  // Parse `name="value"` / `name='value'` / `name=value` / bare names from a
  // tag body (everything between `<name` and `>`). Names are lowercased.
  static std::vector<std::pair<std::string, std::string>> ParseHtmlAttrs(
      std::string_view body) {
    std::vector<std::pair<std::string, std::string>> attrs;
    size_t i = 0;
    const size_t n = body.size();
    while (i < n) {
      while (i < n && IsHtmlSpace(body[i])) {
        ++i;
      }
      if (i >= n || body[i] == '/') {
        break;
      }
      size_t j = i;
      while (j < n && IsTagChar(body[j])) {
        ++j;
      }
      if (j == i) {
        ++i;
        continue;
      }
      std::string name = ToLower(body.substr(i, j - i));
      i = j;
      while (i < n && IsHtmlSpace(body[i])) {
        ++i;
      }
      std::string value;
      if (i < n && body[i] == '=') {
        ++i;
        while (i < n && IsHtmlSpace(body[i])) {
          ++i;
        }
        if (i < n && (body[i] == '"' || body[i] == '\'')) {
          const char quote = body[i++];
          const size_t end = body.find(quote, i);
          if (end == std::string::npos) {
            value = body.substr(i);
            i = n;
          } else {
            value = body.substr(i, end - i);
            i = end + 1;
          }
        } else {
          size_t k = i;
          while (k < n && !IsHtmlSpace(body[k]) && body[k] != '>') {
            ++k;
          }
          value = body.substr(i, k - i);
          i = k;
        }
      }
      attrs.emplace_back(std::move(name), std::move(value));
    }
    return attrs;
  }

  static std::string_view HtmlAttr(
      const std::vector<std::pair<std::string, std::string>>& attrs,
      std::string_view name) {
    for (const auto& [key, value] : attrs) {
      if (key == name) {
        return value;
      }
    }
    return {};
  }

  // Attribute presence regardless of value: bare `open`, `open=""` and
  // `open="open"` all count (e.g. <details open> starts expanded).
  static bool HasHtmlAttr(
      const std::vector<std::pair<std::string, std::string>>& attrs,
      std::string_view name) {
    for (const auto& [key, value] : attrs) {
      if (key == name) {
        return true;
      }
    }
    return false;
  }

  static bool IsVoidElement(const std::string& name) {
    return name == "br" || name == "hr" || name == "img" ||
           name == "meta" || name == "link" || name == "input" ||
           name == "source" || name == "wbr";
  }

  // Push an inline style for an opening tag (reusing the markdown span
  // machinery, so wrap-mode style continuity applies to HTML content too).
  void HtmlOpenSpan(Decorator style, std::string_view key) {
    EnsureHtmlPara();
    if (!IsInlineCapable(Top().kind)) {
      return;
    }
    PushSpan(std::move(style), std::string(key));
    ++html_open_count_;
  }

  // Pop for a closing tag. Strict: only the matching entry goes, so a stray
  // close in a markdown paragraph can never pop a markdown span (or vice
  // versa); unclosed entries die with the enclosing block anyway.
  void HtmlCloseSpan(std::string_view key, bool prefix = false) {
    if (html_open_count_ == 0 || span_keys_.empty() ||
        span_decorators_.empty()) {
      return;
    }
    const std::string& top = span_keys_.back();
    const bool match =
        prefix ? (top.size() >= key.size() &&
                  top.compare(0, key.size(), key) == 0)
               : (top == key);
    if (!match) {
      return;
    }
    PopSpan();
    --html_open_count_;
  }

  void HandleHtmlTag(std::string_view inner, std::string_view raw) {
    // Comments, doctypes, processing instructions: always verbatim.
    if (inner.size() >= 3 && inner[0] == '!' && inner[1] == '-' &&
        inner[2] == '-') {
      HtmlVerbatim(raw);
      return;
    }
    if (!inner.empty() && (inner[0] == '!' || inner[0] == '?')) {
      HtmlVerbatim(raw);
      return;
    }
    size_t i = 0;
    bool closing = false;
    if (i < inner.size() && inner[i] == '/') {
      closing = true;
      ++i;
    }
    size_t j = i;
    while (j < inner.size() && IsTagChar(inner[j])) {
      ++j;
    }
    const std::string name = ToLower(inner.substr(i, j - i));
    if (name.empty()) {
      HtmlVerbatim(raw);
      return;
    }
    if (closing && IsVoidElement(name)) {
      return;  // stray `</img>` / `</br>`: nothing to close.
    }
    const auto attrs = ParseHtmlAttrs(inner.substr(j));

    // Inline elements (also interpreted in markdown paragraphs; see the
    // html_inline_only_ gate for block/rawtext below).
    if (name == "b" || name == "strong") {
      if (closing) {
        HtmlCloseSpan("b");
      } else {
        HtmlOpenSpan(ftxui::bold, "b");
      }
      return;
    }
    if (name == "i" || name == "em") {
      if (closing) {
        HtmlCloseSpan("i");
      } else {
        HtmlOpenSpan(ftxui::italic, "i");
      }
      return;
    }
    if (name == "u") {
      if (closing) {
        HtmlCloseSpan("u");
      } else {
        HtmlOpenSpan(ftxui::underlined, "u");
      }
      return;
    }
    if (name == "s" || name == "del" || name == "strike") {
      if (closing) {
        HtmlCloseSpan("s");
      } else {
        HtmlOpenSpan(ftxui::strikethrough, "s");
      }
      return;
    }
    if (name == "code") {
      if (closing) {
        HtmlCloseSpan("code");
      } else {
        HtmlOpenSpan(InlineCodeStyle(), "code");
      }
      return;
    }
    if (name == "a") {
      if (closing) {
        // Drop formatting whitespace before </a> (e.g. the newline+indent
        // around a lone <img/>): it would render as link-styled padding
        // around the image. Genuine label text is never whitespace-only.
        if (IsInlineCapable(Top().kind) && !Top().inline_.empty()) {
          const std::string key = ComposedStyleKey();
          while (!Top().inline_.empty()) {
            const Fragment& back = Top().inline_.back();
            if (!IsAllSpace(back.text) || back.style_key != key) {
              break;
            }
            Top().inline_.pop_back();
          }
        }
        link_ws_key_.clear();
        HtmlCloseSpan("a:", true);
      } else {
        const std::string href_raw(HtmlAttr(attrs, "href"));
        if (href_raw.empty()) {
          HtmlOpenSpan(ftxui::underlined, "a:");
          link_ws_key_ = "a:";
        } else {
          // Decode entities only after the quoted attribute boundary is known,
          // and reject invalid/unsafe decoded destinations (keep the label).
          const DecodedDestination href = DecodeDestination(href_raw);
          const std::string link_key = "a:" + href.text;
          HtmlOpenSpan(LinkStyle(href.text, !href.invalid), link_key);
          // Skip formatting whitespace right after <a> (see the close branch):
          // the first real content clears this.
          link_ws_key_ = link_key;
        }
      }
      return;
    }
    if (name == "img") {
      if (!closing) {
        EnsureHtmlPara();
        if (!IsInlineCapable(Top().kind)) {
          return;
        }
        const std::string_view alt_raw = HtmlAttr(attrs, "alt");
        const std::string alt =
            alt_raw.empty() ? std::string("[img]") : DecodeEntityText(alt_raw);
        PushSpan(ftxui::dim, "img");
        EmitInline(alt);
        PopSpan();
      }
      return;
    }
    if (name == "br") {
      if (!closing) {
        HtmlBreak();
      }
      return;
    }

    // Rawtext elements: buffer everything verbatim until the matching close.
    // <pre> renders as a code box; <script>/<style> contents are never
    // interpreted, only shown code-styled in a single coalesced box.
    // Block-only: inside a markdown paragraph these stay verbatim (and must
    // never swallow the surrounding markdown text into rawtext).
    if (name == "pre" || name == "script" || name == "style") {
      if (html_inline_only_) {
        HtmlVerbatim(raw);
        return;
      }
      if (closing) {
        if (name != raw_tag_) {
          HtmlVerbatim(raw);  // stray close outside rawtext.
          return;
        }
        // The newline right after the opening tag is source formatting, not
        // content (browsers drop it in <pre> too).
        if (!raw_buf_.empty() && raw_buf_[0] == '\n') {
          raw_buf_.erase(0, 1);
        }
        if (IsInlineCapable(Top().kind)) {
          // Invalid nesting (<pre> inside running text): fall back to
          // styled inline rather than corrupting the stack.
          PushSpan(InlineCodeStyle(), "code");
          EmitInline(ExpandTabs(raw_buf_));
          PopSpan();
        } else if (Top().kind == Kind::Html) {
          Top().text += raw_buf_;
        } else {
          Attach(CodeElement(raw_buf_, wrap_), true);
        }
        raw_tag_.clear();
        raw_buf_.clear();
      } else {
        CloseHtmlPara();
        FlushHtmlText();
        raw_tag_ = name;
        raw_buf_.clear();
      }
      return;
    }

    // Block elements. Block-only: inside a markdown paragraph a block tag
    // would pop the paragraph's own frame, so it stays verbatim instead.
    if (html_inline_only_ &&
        (name == "p" || name == "div" || name == "blockquote" ||
         name == "ul" || name == "ol" || name == "li" || name == "hr" ||
         (name.size() == 2 && name[0] == 'h' && name[1] >= '1' &&
          name[1] <= '6'))) {
      HtmlVerbatim(raw);
      return;
    }
    if (name == "p" || name == "div") {
      CloseHtmlPara();
      FlushHtmlText();
      if (!closing) {
        html_align_ =
            (ToLower(HtmlAttr(attrs, "align")) == "center") ? 'c' : 0;
      }
      return;
    }
    if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' &&
        name[1] <= '6') {
      if (closing) {
        CloseHtmlHeading();
      } else {
        CloseHtmlPara();
        FlushHtmlText();
        if (Top().kind == Kind::Heading) {
          CloseHtmlHeading();
        }
        Push(Frame::Heading(name[1] - '0'));
      }
      return;
    }
    if (name == "blockquote") {
      if (closing) {
        CloseHtmlPara();
        FlushHtmlText();
        if (Top().kind == Kind::Quote) {
          LeaveBlockImpl(MD_BLOCK_QUOTE, nullptr);
        }
      } else {
        CloseHtmlPara();
        FlushHtmlText();
        Push(Frame::Quote());
      }
      return;
    }
    if (name == "ul" || name == "ol") {
      if (closing) {
        CloseHtmlPara();
        FlushHtmlText();
        if (Top().kind == Kind::Item) {
          LeaveBlockImpl(MD_BLOCK_LI, nullptr);  // implicit </li>.
        }
        if (Top().kind == Kind::List) {
          // MD_BLOCK_UL/OL leave paths are identical (vbox of children).
          LeaveBlockImpl(MD_BLOCK_UL, nullptr);
        }
      } else {
        CloseHtmlPara();
        FlushHtmlText();
        Frame list = Frame::List();
        if (name == "ul") {
          list.bullet = '-';
        } else {
          list.ordered_index = 1;
          list.ordered_mark = '.';
        }
        Push(std::move(list));
      }
      return;
    }
    if (name == "li") {
      if (closing) {
        CloseHtmlPara();
        FlushHtmlText();
        if (Top().kind == Kind::Item) {
          LeaveBlockImpl(MD_BLOCK_LI, nullptr);
        }
      } else {
        if (Top().kind == Kind::Item) {
          LeaveBlockImpl(MD_BLOCK_LI, nullptr);  // implicit </li>.
        }
        CloseHtmlPara();
        FlushHtmlText();
        if (Top().kind != Kind::List) {
          HtmlVerbatim(raw);  // stray <li> outside a list.
          return;
        }
        // Number/bullet like a markdown item (see MD_BLOCK_LI enter).
        Frame& list = Top();
        Frame item = Frame::Item();
        item.bullet = list.bullet;
        item.ordered_index = list.ordered_index;
        item.ordered_mark = list.ordered_mark;
        if (list.bullet == 0) {
          ++list.ordered_index;
        }
        Push(std::move(item));
      }
      return;
    }
    if (name == "hr") {
      if (!closing) {
        CloseHtmlPara();
        FlushHtmlText();
        AttachHr(MakeSeparator(snapshot_));
      }
      return;
    }

    // <details>/<summary> render statically and always expanded (see plan):
    // the summary gets a disclosure marker, content flows as normal blocks.
    // Block-level, so inside a markdown paragraph they stay verbatim.
    if (name == "details" || name == "summary") {
      if (html_inline_only_) {
        HtmlVerbatim(raw);
        return;
      }
      if (name == "details") {
        CloseHtmlPara();
        FlushHtmlText();
        if (closing) {
          if (!details_stack_.empty()) {
            details_stack_.pop_back();
          }
        } else {
          details_stack_.push_back(HasHtmlAttr(attrs, "open") ? 'o' : 0);
        }
        return;
      }
      if (closing) {
        if (summary_open_ && Top().kind == Kind::Para) {
          CloseHtmlSummary();
        } else {
          summary_open_ = false;
          HtmlVerbatim(raw);  // stray close.
        }
      } else {
        CloseHtmlPara();
        FlushHtmlText();
        Push(Frame::Para());
        summary_open_ = true;
      }
      return;
    }

    // Unknown tags: verbatim, like today's inline-HTML styling.
    HtmlVerbatim(raw);
  }

  // Find `</name>` (case-insensitive, allowing whitespace) at or after
  // `from`; returns the '<' index or npos.
  static size_t FindHtmlCloseTag(const std::string& buf, size_t from,
                                 std::string_view name) {
    for (size_t lt = buf.find('<', from); lt != std::string::npos;
         lt = buf.find('<', lt + 1)) {
      size_t k = lt + 1;
      while (k < buf.size() && IsHtmlSpace(buf[k])) {
        ++k;
      }
      if (k >= buf.size() || buf[k] != '/') {
        continue;
      }
      ++k;
      while (k < buf.size() && IsHtmlSpace(buf[k])) {
        ++k;
      }
      size_t m = k;
      while (m < buf.size() && IsTagChar(buf[m])) {
        ++m;
      }
      if (ToLower(std::string_view(buf).substr(k, m - k)) != name) {
        continue;
      }
      if (m >= buf.size() || buf[m] == '>' || IsHtmlSpace(buf[m]) ||
          buf[m] == '/') {
        return lt;
      }
    }
    return std::string::npos;
  }

  // Interpret one MD_TEXT_HTML chunk. Tags may arrive split across md4c
  // callbacks, so an unterminated tag stays buffered in html_chunk_.
  void HtmlText(std::string_view s) {
    html_chunk_.append(s.data(), s.size());
    size_t pos = 0;
    while (pos < html_chunk_.size()) {
      if (!raw_tag_.empty()) {
        const size_t lt = FindHtmlCloseTag(html_chunk_, pos, raw_tag_);
        if (lt == std::string::npos) {
          raw_buf_.append(html_chunk_.data() + pos, html_chunk_.size() - pos);
          html_chunk_.clear();
          return;
        }
        raw_buf_.append(html_chunk_.data() + pos, lt - pos);
        const size_t gt = html_chunk_.find('>', lt + 1);
        const std::string_view chunk(html_chunk_);
        const std::string_view inner = chunk.substr(lt + 1, gt - lt - 1);
        const std::string_view raw = chunk.substr(lt, gt - lt + 1);
        pos = 0;
        HandleHtmlTag(inner, raw);
        html_chunk_.erase(0, gt + 1);
        continue;
      }
      const size_t lt = html_chunk_.find('<', pos);
      if (lt == std::string::npos) {
        HtmlInlineText(std::string_view(html_chunk_).substr(pos));
        html_chunk_.clear();
        return;
      }
      if (lt > pos) {
        HtmlInlineText(std::string_view(html_chunk_).substr(pos, lt - pos));
      }
      if (html_chunk_.compare(lt, 4, "<!--") == 0) {
        const size_t end = html_chunk_.find("-->", lt + 4);
        if (end == std::string::npos) {
          html_chunk_.erase(0, lt);
          return;
        }
        HtmlVerbatim(
            std::string_view(html_chunk_).substr(lt, end + 3 - lt));
        pos = end + 3;
        continue;
      }
      const size_t gt = html_chunk_.find('>', lt + 1);
      if (gt == std::string::npos) {
        html_chunk_.erase(0, lt);  // partial tag; await more.
        return;
      }
      const std::string_view chunk(html_chunk_);
      HandleHtmlTag(chunk.substr(lt + 1, gt - lt - 1),
                    chunk.substr(lt, gt - lt + 1));
      pos = gt + 1;
    }
    html_chunk_.clear();
  }

  // End inline-HTML interpretation for a markdown block: a tag split
  // across md4c callbacks that never completed shows literally (rather than
  // leaking into the next block), and unclosed tag entries are popped so
  // their decorators/keys cannot leak into later blocks (a bare count reset
  // would leave stale entries behind, corrupting style-key matching for
  // link whitespace, </a> trimming, and wrap continuity). md4c closes every
  // markdown span before the block leave, so remaining owned entries are
  // HTML ones. No-op inside HTML blocks (the HTML leave path handles its
  // own state).
  void EndHtmlParaContext() {
    if (in_html_) {
      return;
    }
    if (!html_chunk_.empty()) {
      PushSpan(InlineCodeStyle(), "code");
      EmitInline(html_chunk_);
      PopSpan();
      html_chunk_.clear();
    }
    while (html_open_count_ > 0 && !span_decorators_.empty() &&
           !span_keys_.empty()) {
      PopSpan();
      --html_open_count_;
    }
    link_ws_key_.clear();
    html_open_count_ = 0;
  }

  // ---- md4c callback dispatchers -----------------------------------------
  static int cb_enter_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto* self = static_cast<Renderer*>(userdata);
    if (self->cancelled_ && self->cancelled_()) {
      self->aborted_ = true;
      return 1;
    }
    return self->EnterBlockImpl(type, detail);
  }
  static int cb_leave_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
    return static_cast<Renderer*>(userdata)->LeaveBlockImpl(type, detail);
  }
  static int cb_enter_span(MD_SPANTYPE type, void* detail, void* userdata) {
    return static_cast<Renderer*>(userdata)->EnterSpanImpl(type, detail);
  }
  static int cb_leave_span(MD_SPANTYPE type, void* detail, void* userdata) {
    return static_cast<Renderer*>(userdata)->LeaveSpanImpl(type, detail);
  }
  static int cb_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size,
                     void* userdata) {
    auto* self = static_cast<Renderer*>(userdata);
    if (self->cancelled_ && self->cancelled_()) {
      self->aborted_ = true;
      return 1;
    }
    return self->TextImpl(type, text, size);
  }

  // ---- md4c callbacks -----------------------------------------------------
  int EnterBlockImpl(MD_BLOCKTYPE type, void* detail) {
    if (type != MD_BLOCK_HTML) {
      // Blank lines produce no callbacks, so consecutive HTML blocks reach
      // here untouched and their verbatim text coalesces into one box.
      FlushPendingHtml();
    }
    switch (type) {
      case MD_BLOCK_DOC:
        break;
      case MD_BLOCK_P:
        Push(Frame::Para());
        break;
      case MD_BLOCK_H: {
        auto* h = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        Push(Frame::Heading(h->level));
        Top().heading_ordinal = heading_ordinal_++;
        break;
      }
      case MD_BLOCK_HR:
        AttachHr(MakeSeparator(snapshot_));
        break;
      case MD_BLOCK_QUOTE:
        Push(Frame::Quote());
        break;
      case MD_BLOCK_UL: {
        auto* ul = static_cast<MD_BLOCK_UL_DETAIL*>(detail);
        Frame f = Frame::List();
        f.bullet = ul->mark;
        Push(std::move(f));
        break;
      }
      case MD_BLOCK_OL: {
        auto* ol = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        Frame f = Frame::List();
        f.ordered_index = ol->start;
        f.ordered_mark = ol->mark_delimiter;
        Push(std::move(f));
        break;
      }
      case MD_BLOCK_LI: {
        auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        Frame& list = Top();  // enclosing list
        Frame f = Frame::Item();
        f.is_task = li->is_task != 0;
        f.task_mark = li->task_mark;
        f.bullet = list.bullet;
        f.ordered_mark = list.ordered_mark;
        if (list.kind == Kind::List && list.bullet == 0) {
          f.ordered_index = list.ordered_index;
          ++list.ordered_index;
        }
        Push(std::move(f));
        break;
      }
      case MD_BLOCK_CODE:
        Push(Frame::Code());
        break;
      case MD_BLOCK_HTML:
        in_html_ = true;
        span_depth_ = span_decorators_.size();
        Push(Frame::Html());
        break;
      case MD_BLOCK_TABLE: {
        auto* t = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail);
        Push(Frame::Table(t->col_count));
        break;
      }
      case MD_BLOCK_THEAD:
      case MD_BLOCK_TBODY: {
        // Record the section on the enclosing Table frame so nested tables
        // don't clobber an outer table's state.
        Frame* t = nullptr;
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
          if (it->kind == Kind::Table) {
            t = &*it;
            break;
          }
        }
        if (t) {
          t->in_thead = (type == MD_BLOCK_THEAD);
        }
        break;
      }
      case MD_BLOCK_TR: {
        Frame& table = Top();
        bool is_header = table.kind == Kind::Table && table.in_thead;
        Push(Frame::Row(is_header));
        break;
      }
      case MD_BLOCK_TH:
      case MD_BLOCK_TD: {
        auto* td = static_cast<MD_BLOCK_TD_DETAIL*>(detail);
        Push(Frame::Cell(static_cast<char>(td->align),
                         type == MD_BLOCK_TH));
        break;
      }
    }
    return 0;
  }

  int LeaveBlockImpl(MD_BLOCKTYPE type, void* /*detail*/) {
    switch (type) {
      case MD_BLOCK_P: {
        EndHtmlParaContext();
        Frame top = Pop();
        Attach(InlineBlocks(top), true);
        break;
      }
      case MD_BLOCK_H: {
        EndHtmlParaContext();
        Frame top = Pop();
        Element e = InlineBlocks(top);
        e = ftxui::bold(e) | ftxui::color(HeadingColor(top.heading_level));
        if (snapshot_ != nullptr) {
          e = SnapshotHeading(snapshot_, static_cast<int>(top.heading_ordinal),
                              std::move(e));
        }
        Attach(std::move(e), true);
        OweHeadingGap();
        break;
      }
      case MD_BLOCK_QUOTE: {
        Frame top = Pop();
        Elements rows;
        for (auto& c : top.children) {
          rows.push_back(ftxui::hbox({
              MakeText(snapshot_, "│ ", ftxui::color(theme_.quote_marker)),
              std::move(c),
          }));
        }
        Attach(ftxui::vbox(std::move(rows)) | ftxui::dim, true);
        break;
      }
      case MD_BLOCK_UL:
      case MD_BLOCK_OL: {
        Frame top = Pop();
        Attach(ftxui::vbox(std::move(top.children)), true);
        break;
      }
      case MD_BLOCK_LI: {
        EndHtmlParaContext();
        Frame top = Pop();
        // In a tight list md4c sends the item text directly into this frame
        // (no wrapping paragraph), so flush any pending inline fragments.
        if (!top.inline_.empty() || !top.rows_.empty()) {
          top.children.insert(top.children.begin(), InlineBlocks(top));
        }
        std::string bullet;
        if (top.is_task) {
          bool checked = (top.task_mark == 'x' || top.task_mark == 'X');
          bullet = checked ? "[x] " : "[ ] ";
        } else if (top.bullet != 0) {
          bullet = std::string(1, top.bullet) + " ";
        } else {
          bullet = std::to_string(top.ordered_index) + top.ordered_mark + " ";
        }
        Elements children = std::move(top.children);
        if (children.empty()) {
          children.push_back(ftxui::text(""));
        }
        Element content = children.size() == 1
                              ? std::move(children[0])
                              : ftxui::vbox(std::move(children));
        Attach(ftxui::hbox({
                   MakeText(snapshot_, bullet, Decorator(nullptr)),
                   std::move(content),
               }),
               false);
        break;
      }
      case MD_BLOCK_CODE:
      case MD_BLOCK_HTML: {
        if (type == MD_BLOCK_HTML) {
          in_html_ = false;
          // A tag split across md4c chunks never closed: show it literally.
          if (!html_chunk_.empty()) {
            HtmlInlineText(html_chunk_);
            html_chunk_.clear();
          }
          // Auto-close unclosed tags (browser-style forgiveness).
          while (Top().kind != Kind::Html) {
            const Kind kind = Top().kind;
            if (kind == Kind::Para) {
              CloseHtmlPara();
            } else if (kind == Kind::Heading) {
              CloseHtmlHeading();
            } else if (kind == Kind::Quote) {
              LeaveBlockImpl(MD_BLOCK_QUOTE, nullptr);
            } else if (kind == Kind::List) {
              // MD_BLOCK_UL/OL leave paths are identical.
              LeaveBlockImpl(MD_BLOCK_UL, nullptr);
            } else if (kind == Kind::Item) {
              LeaveBlockImpl(MD_BLOCK_LI, nullptr);
            } else {
              break;  // Table/Row/Cell/Code cannot occur here; avoid a loop.
            }
          }
          if (!raw_tag_.empty()) {
            // Unterminated <pre>/<script>: keep what was buffered.
            if (Top().kind == Kind::Html) {
              Top().text += raw_buf_;
            }
            raw_tag_.clear();
            raw_buf_.clear();
          }
          if (span_decorators_.size() > span_depth_) {
            ResizeSpans(span_depth_);
          }
          html_open_count_ = 0;
          html_align_ = 0;
          summary_open_ = false;
          details_stack_.clear();
          link_ws_key_.clear();
        }
        Frame top = Pop();
        if (type == MD_BLOCK_HTML && top.children.empty()) {
          // Pure verbatim: coalesce with neighbouring HTML blocks (split at
          // blank lines) instead of one box per block. (The remainder is
          // still in top.text here: boxing happens only via FlushHtmlText
          // at tag boundaries, never at leave, so this check can see it.)
          if (!top.text.empty() && !IsAllSpace(top.text)) {
            if (!pending_html_.empty() && pending_html_.back() != '\n') {
              pending_html_ += '\n';
            }
            pending_html_ += top.text;
          }
          break;  // flushed by later content or at the end of the document.
        }
        if (type == MD_BLOCK_HTML) {
          // Rendered blocks attach as ordinary elements (single or stacked).
          // A verbatim remainder joins them after the rendered children.
          Elements out;
          if (!pending_html_.empty() && !IsAllSpace(pending_html_)) {
            out.push_back(CodeElement(pending_html_, wrap_));
          }
          pending_html_.clear();
          for (auto& c : top.children) {
            out.push_back(std::move(c));
          }
          if (!top.text.empty() && !IsAllSpace(top.text)) {
            out.push_back(CodeElement(top.text, wrap_));
          }
          Attach(out.size() == 1 ? std::move(out[0])
                                 : ftxui::vbox(std::move(out)),
                 true);
          break;
        }
        pending_html_.clear();
        Attach(CodeElement(top.text, wrap_), true);
        break;
      }
      case MD_BLOCK_TABLE: {
        Frame top = Pop();
        Attach(TableElement(top.table_rows), true);
        break;
      }
      case MD_BLOCK_TR: {
        Frame top = Pop();
        bool is_header = top.is_header_row;
        // The parent of a Row is always the enclosing Table: neither THEAD
        // nor TBODY pushes a frame. Do not search up the stack here — a
        // nested block inside a cell leaves extra frames above the Row, and
        // popping them would corrupt the stack for subsequent content.
        Frame& table = Top();
        if (table.kind != Kind::Table) {
          return 1;  // unexpected stack state; abort parsing.
        }
        table.table_rows.push_back({is_header, std::move(top.cells)});
        break;
      }
      case MD_BLOCK_TH:
      case MD_BLOCK_TD: {
        EndHtmlParaContext();
        Frame top = Pop();
        Frame& row = Top();
        if (row.kind != Kind::Row) {
          return 1;  // stack desync; abort.
        }
        // Combine direct inline text (possibly spanning hard-break rows) with
        // any nested block content.
        Elements parts;
        bool has_inline = !top.inline_.empty() || !top.rows_.empty();
        if (has_inline) {
          parts.push_back(InlineBlocks(top));
        }
        for (auto& c : top.children) {
          parts.push_back(std::move(c));
        }
        Element cell = parts.empty()
                           ? ftxui::text("")
                           : ftxui::vbox(std::move(parts));
        if (top.cell_align == 2) {
          cell = ftxui::hcenter(cell);
        } else if (top.cell_align == 3) {
          cell = ftxui::align_right(cell);
        }
        row.cells.push_back(std::move(cell));
        break;
      }
      case MD_BLOCK_THEAD:
      case MD_BLOCK_TBODY:
      case MD_BLOCK_DOC:
        break;
    }
    return 0;
  }

  int EnterSpanImpl(MD_SPANTYPE type, void* detail) {
    switch (type) {
      case MD_SPAN_EM:
        PushSpan(ftxui::italic, "em");
        break;
      case MD_SPAN_STRONG:
        PushSpan(ftxui::bold, "strong");
        break;
      case MD_SPAN_DEL:
        PushSpan(ftxui::strikethrough, "del");
        break;
      case MD_SPAN_CODE:
        PushSpan(InlineCodeStyle(), "code");
        break;
      case MD_SPAN_A: {
        auto* a = static_cast<MD_SPAN_A_DETAIL*>(detail);
        const DecodedDestination href = DecodeMdAttribute(a->href);
        PushSpan(LinkStyle(href.text, !href.invalid), "a:" + href.text);
        break;
      }
      case MD_SPAN_IMG:
        PushSpan(ftxui::dim, "img");
        break;
      case MD_SPAN_U:
        PushSpan(ftxui::underlined, "u");
        break;
      default:
        break;
    }
    return 0;
  }

  int LeaveSpanImpl(MD_SPANTYPE type, void* /*detail*/) {
    switch (type) {
      case MD_SPAN_EM:
      case MD_SPAN_STRONG:
      case MD_SPAN_DEL:
      case MD_SPAN_CODE:
      case MD_SPAN_A:
      case MD_SPAN_U:
      case MD_SPAN_IMG:
        PopSpan();
        break;
      default:
        break;
    }
    return 0;
  }

  int TextImpl(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size) {
    const std::string_view s(reinterpret_cast<const char*>(text), size);
    switch (type) {
      case MD_TEXT_NORMAL:
        // Inside an HTML block plain text joins the tag stream so source
        // newlines collapse and merge like any other HTML whitespace.
        if (in_html_) {
          HtmlInlineText(s);
        } else {
          EmitInline(s);
        }
        break;
      case MD_TEXT_ENTITY:
        // Decode exactly the references md4c typed as entities; ordinary text
        // (including escaped literal '&amp;') is never rescanned.
        if (in_html_) {
          HtmlInlineText(s);
        } else {
          EmitInline(DecodeEntityForDisplay(s));
        }
        break;
      case MD_TEXT_NULLCHAR:
        // A NUL in the source is displayed as U+FFFD, never emitted live.
        EmitInline("\xEF\xBF\xBD");
        break;
      case MD_TEXT_SOFTBR:
        if (in_html_) {
          HtmlInlineText(" ");
        } else {
          EmitInline(" ");
        }
        break;
      case MD_TEXT_BR:
        // A trailing-streak hard break ends the current inline row instead of
        // embedding a "\n" text node (which would inflate the row height and
        // make background decorators bleed onto the line below).
        if (in_html_) {
          HtmlBreak();
        } else {
          FlushRow(Top());
        }
        break;
      case MD_TEXT_CODE: {
        Frame& top = Top();
        if (top.kind == Kind::Code) {
          top.text.append(s.data(), s.size());
        } else if (in_html_) {
          HtmlText(s);
        } else {
          // Inline code: expand tabs to four-column stops (display only).
          EmitInline(ExpandTabs(s));
        }
        break;
      }
      case MD_TEXT_HTML:
        // Fenced code stays literal. Inside an HTML block tags are fully
        // interpreted (HtmlText); inline HTML in running markdown text
        // interprets inline tags only (b/i/code/a/...) so common markup
        // like <i> renders, while block-level tags stay verbatim.
        if (Top().kind == Kind::Code) {
          Top().text += s;
        } else if (in_html_) {
          HtmlText(s);
        } else {
          html_inline_only_ = true;
          HtmlText(s);
          html_inline_only_ = false;
        }
        break;
      default:
        break;
    }
    return 0;
  }

  // ---- finalizers ---------------------------------------------------------

  Element CodeElement(const std::string& code, bool wrap) {
    std::vector<std::string> lines;
    size_t line_start = 0;
    for (size_t i = 0; i < code.size(); ++i) {
      if (code[i] == '\n') {
        lines.emplace_back(code.data() + line_start, i - line_start);
        line_start = i + 1;
      }
    }
    if (line_start < code.size() || code.empty()) {
      lines.emplace_back(code.data() + line_start, code.size() - line_start);
    }
    // Expand tabs to four-column stops before any wrapping so a tab is not
    // treated as a single column or stranded at a wrap edge.
    for (auto& l : lines) {
      l = ExpandTabs(l);
    }

    auto token_text = [this](std::string s) {
      return MakeText(snapshot_, std::move(s),
                      ftxui::color(theme_.code_block_fg));
    };

    Elements rows;
    rows.reserve(lines.size());
    for (auto& l : lines) {
      if (!wrap || l.empty()) {
        rows.push_back(token_text(l));
        continue;
      }
      // Wrap mode: split the raw line into word atoms and append the following
      // inter-word whitespace to the *preceding* atom, so a soft wrap consumes
      // the separator instead of charging its width against the next word.
      // Leading indentation stays on the first atom and trailing whitespace on
      // the last, where it clips rather than wrapping onto an extra row.
      // Tokens longer than the viewport are never split mid-word; they clip.
      Elements toks;
      std::string cur;
      size_t i = 0;
      const size_t n = l.size();
      while (i < n) {
        const bool space = (l[i] == ' ' || l[i] == '\t');
        size_t j = i;
        while (j < n && ((l[j] == ' ' || l[j] == '\t') == space)) {
          ++j;
        }
        if (space) {
          cur.append(l, i, j - i);  // leading indentation or all-space line.
          i = j;
          continue;
        }
        cur.append(l, i, j - i);
        // Attach a following whitespace run to this atom (separator or
        // trailing spaces).
        if (j < n && (l[j] == ' ' || l[j] == '\t')) {
          size_t e = j;
          while (e < n && (l[e] == ' ' || l[e] == '\t')) {
            ++e;
          }
          cur.append(l, j, e - j);
          j = e;
        }
        toks.push_back(token_text(std::move(cur)));
        cur.clear();
        i = j;
      }
      if (!cur.empty()) {
        toks.push_back(token_text(std::move(cur)));  // whitespace-only line.
      }
      if (toks.empty()) {
        toks.push_back(token_text(""));
      }
      rows.push_back(ftxui::hflow(std::move(toks)));
    }

    if (rows.empty()) {
      rows.push_back(ftxui::text(""));
    }
    // Both modes: the box spans the full content width, like tables. In wrap
    // mode hflow reflows each line inside it; in scroll mode the box keeps
    // its natural width and pans with the rest of the content.
    return MakeBorder(
        snapshot_,
        ftxui::vbox(std::move(rows)) | ftxui::bgcolor(theme_.code_block_bg));
  }

  Element TableElement(
      const std::vector<std::pair<bool, std::vector<Element>>>& rows) {
    if (rows.empty()) {
      return ftxui::text("");
    }
    unsigned columns = rows[0].second.size();
    std::vector<unsigned> widths(columns, 0);
    for (const auto& [is_header, cells] : rows) {
      unsigned i = 0;
      for (const auto& cell : cells) {
        if (i < columns) {
          // requirement() is only valid after layout; compute it so column
          // widths reflect content instead of a stale/default requirement.
          cell->ComputeRequirement();
          auto req = cell->requirement();
          widths[i] = std::max(widths[i], static_cast<unsigned>(
                                               std::max<int>(req.min_x, 1)));
        }
        ++i;
      }
    }
    Elements rows_el;
    for (const auto& [is_header, cells] : rows) {
      Elements row_cells;
      unsigned i = 0;
      for (const auto& cell : cells) {
        Element padded = cell;
        if (i < columns) {
          padded = ftxui::size(ftxui::WIDTH, ftxui::EQUAL, widths[i])(cell);
        }
        if (is_header) {
          padded = padded | ftxui::bold;
        }
        row_cells.push_back(std::move(padded));
        if (i + 1 < columns) {  // one-column gutter between cells.
          row_cells.push_back(
              ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 1)(
                  MakeText(snapshot_, " ", Decorator(nullptr))));
        }
        ++i;
      }
      rows_el.push_back(ftxui::hbox(std::move(row_cells)));
    }
    return MakeBorder(snapshot_, ftxui::vbox(std::move(rows_el)));
  }

  // --- Theme-styled helpers --------------------------------------------------

  Decorator LinkStyle(const std::string& href, bool link_ok = true) {
    // Retain the label text and link styling for unsafe destinations, but do
    // not emit them as an OSC 8 hyperlink target: malformed UTF-8 or decoded
    // C0/DEL/C1 controls could otherwise reach the terminal through the URL.
    Decorator style = ftxui::color(theme_.link) | ftxui::underlined;
    if (link_ok && IsSafeLinkDestination(href)) {
      style = style | ftxui::hyperlink(href);
    }
    return style;
  }

  Decorator InlineCodeStyle() {
    return ftxui::color(theme_.inline_code_fg) |
           ftxui::bgcolor(theme_.inline_code_bg);
  }

  Color HeadingColor(unsigned level) {
    switch (level) {
      case 1:
        return theme_.heading_h1;
      case 2:
        return theme_.heading_h2;
      case 3:
        return theme_.heading_h3;
      default:
        return theme_.heading_h4;
    }
  }

  const std::string& source_;
  const Theme& theme_;
  const bool wrap_;
  LayoutSnapshot* snapshot_ = nullptr;
  unsigned heading_ordinal_ = 0;
  std::function<bool()> cancelled_;
  bool aborted_ = false;
  std::vector<Frame> frames_;
  std::vector<Decorator> span_decorators_;
  std::vector<std::string> span_keys_;
  mutable std::string style_key_cache_;
  mutable bool style_key_cache_valid_ = false;
  // HTML interpretation state (see the HTML rendering helpers above).
  bool in_html_ = false;
  // Inside a markdown paragraph only inline tags interpret; block-level
  // tags stay verbatim so they cannot pop the paragraph's own frame.
  bool html_inline_only_ = false;
  size_t html_open_count_ = 0;  // entries HtmlOpenSpan pushed (strict closes).
  size_t span_depth_ = 0;   // span-stack depth at the HTML block entry.
  std::string html_chunk_;  // partial tag carried across TextImpl calls.
  std::string raw_tag_;     // open rawtext element (pre/script/style).
  std::string raw_buf_;     // buffered rawtext content.
  std::string pending_html_;  // verbatim HTML coalesced across blocks.
  char html_align_ = 0;       // 'c' inside <p>/<div align=center>.
  std::vector<char> details_stack_;  // 'o' per open <details open>.
  bool summary_open_ = false;  // a <summary> paragraph collects text.
  // Skip whitespace-only fragments right after an <a> open (formatting
  // indent, not link content); cleared by the first real content, the
  // matching close, or any HTML state reset. Empty when inactive.
  std::string link_ws_key_;
};

}  // namespace

Element RenderMarkdown(const std::string& markdown, const Config& config) {
  return RenderMarkdown(markdown, config.theme, config.horizontal_wrap);
}

Element RenderMarkdown(const std::string& markdown) {
  static const Config kDefaultConfig;
  return RenderMarkdown(markdown, kDefaultConfig);
}

Element RenderMarkdown(const std::string& markdown, const Theme& theme,
                       WrapMode mode) {
  return Renderer(markdown, theme, mode).Run();
}

Element BuildMarkdownSnapshot(const std::string& markdown, const Theme& theme,
                              WrapMode mode, int width,
                              LayoutSnapshot& snapshot,
                              const std::function<bool()>& cancelled,
                              bool* aborted) {
  Element tree = RenderMarkdownRecording(markdown, theme, mode, snapshot,
                                         cancelled);
  if (cancelled && cancelled()) {
    if (aborted != nullptr) {
      *aborted = true;
    }
    return tree;
  }
  snapshot.Build(tree, width, cancelled);
  if (snapshot.cancelled() && aborted != nullptr) {
    *aborted = true;
  }
  return tree;
}

Element RenderMarkdownRecording(const std::string& markdown, const Theme& theme,
                                WrapMode mode, LayoutSnapshot& snapshot,
                                const std::function<bool()>& cancelled) {
  Renderer renderer(markdown, theme, mode);
  renderer.set_snapshot(&snapshot);
  renderer.set_cancelled(cancelled);
  Element tree = renderer.Run();
  return tree;
}

}  // namespace markit
