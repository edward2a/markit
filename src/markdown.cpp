// Implementation of markdown rendering for markit. See markdown.hpp.
#include "markdown.hpp"

#include <md4c.h>

#include <algorithm>  // for max
#include <string>     // for string, to_string
#include <utility>    // for move
#include <vector>     // for vector

namespace markit {

namespace {

using ftxui::Color;
using ftxui::Decorator;
using ftxui::Element;
using ftxui::Elements;

std::string Attr(const MD_ATTRIBUTE& attr) {
  // md4c attribute text is not null-terminated and may be absent (NULL).
  return attr.text ? std::string(attr.text, attr.size) : std::string();
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
  explicit Renderer(std::string markdown, const Config& config)
      : source_(std::move(markdown)),
        theme_(config.theme),
        wrap_(config.horizontal_wrap == WrapMode::Wrap) {}

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

    md_parse(reinterpret_cast<const MD_CHAR*>(source_.data()), source_.size(),
             &parser, this);

    Frame doc = std::move(frames_.back());
    frames_.pop_back();
    if (doc.children.empty()) {
      doc.children.push_back(ftxui::text("(empty document)"));
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
    std::vector<Element> children;
    std::vector<Fragment> inline_;
    std::vector<Element> rows_;      // completed hard-break rows
    std::string text;              // verbatim buffer (code/html)
    std::vector<std::pair<bool, std::vector<Element>>> table_rows;
    std::vector<Element> cells;    // current row cells

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
    if (blank_before && !parent.children.empty()) {
      parent.children.push_back(ftxui::text(""));
    }
    parent.children.push_back(std::move(e));
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

  void EmitInline(const std::string& text) {
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
    top.inline_.push_back(Fragment{text, ComposedStyle()});
  }

  // Split a row's fragments into word elements for wrap mode. A "word" is a
  // maximal run of non-space text; it may span several styled fragments (e.g.
  // `[a](url)b`), whose pieces are glued into one element so the wrap layout
  // never breaks inside a word. Inter-word whitespace becomes a plain
  // (unstyled) piece glued ahead of the following word, so the wrapping point
  // lands exactly at the source's whitespace while a link/emphasis/code style
  // no longer decorates the space itself. Overlong tokens (longer than the
  // viewport) are never split mid-word; they clip. Trailing whitespace is
  // kept as a final plain piece so an all-space row keeps its height.
  Element WrapRow(const std::vector<Fragment>& fragments) {
    using Piece = std::pair<std::string, Decorator>;  // text, style
    Elements words;
    std::vector<Piece> cur;
    std::string pending;

    auto finish = [&]() {
      if (cur.empty()) {
        return;
      }
      Elements pieces;
      pieces.reserve(cur.size());
      for (auto& piece : cur) {
        Element e = ftxui::text(std::move(piece.first));
        if (piece.second) {
          e = piece.second(std::move(e));
        }
        pieces.push_back(std::move(e));
      }
      words.push_back(pieces.size() == 1 ? std::move(pieces[0])
                                         : ftxui::hbox(std::move(pieces)));
      cur.clear();
    };

    auto is_space = [](char c) {
      return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };

    for (auto& frag : fragments) {
      size_t i = 0;
      const size_t n = frag.text.size();
      while (i < n) {
        if (is_space(frag.text[i])) {
          size_t j = i;
          while (j < n && is_space(frag.text[j])) {
            ++j;
          }
          if (!cur.empty()) {
            finish();
          }
          pending += frag.text.substr(i, j - i);  // inter-word whitespace
          i = j;
        } else {
          size_t j = i;
          while (j < n && !is_space(frag.text[j])) {
            ++j;
          }
          std::string word = frag.text.substr(i, j - i);
          if (!pending.empty()) {
            // The inter-word whitespace stays a plain (unstyled) piece, glued
            // ahead of the following word so the wrap point still lands on the
            // source whitespace — but a link/emphasis/code style no longer
            // decorates the space itself.
            cur.emplace_back(std::move(pending), nullptr);
            pending.clear();
          }
          cur.emplace_back(std::move(word), frag.style);
          i = j;
        }
      }
    }
    finish();
    if (!pending.empty()) {
      words.push_back(ftxui::text(std::move(pending)));
    }
    return ftxui::hflow(std::move(words));
  }

  Element FlattenInline(Frame& frame) {
    std::vector<Fragment> fragments = std::move(frame.inline_);
    frame.inline_.clear();
    if (fragments.empty()) {
      return ftxui::text("");
    }

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
      Element e = ftxui::text(std::move(frag.text));
      if (frag.style) {
        e = frag.style(std::move(e));
      }
      items.push_back(std::move(e));
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

  // ---- md4c callback dispatchers -----------------------------------------
  static int cb_enter_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
    return static_cast<Renderer*>(userdata)->EnterBlockImpl(type, detail);
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
    return static_cast<Renderer*>(userdata)->TextImpl(type, text, size);
  }

  // ---- md4c callbacks -----------------------------------------------------
  int EnterBlockImpl(MD_BLOCKTYPE type, void* detail) {
    switch (type) {
      case MD_BLOCK_P:
        Push(Frame::Para());
        break;
      case MD_BLOCK_H: {
        auto* h = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        Push(Frame::Heading(h->level));
        break;
      }
      case MD_BLOCK_HR:
        Attach(ftxui::separator(), true);
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
      case MD_BLOCK_DOC:
        break;
    }
    return 0;
  }

  int LeaveBlockImpl(MD_BLOCKTYPE type, void* /*detail*/) {
    switch (type) {
      case MD_BLOCK_P: {
        Frame top = Pop();
        Attach(InlineBlocks(top), false);
        break;
      }
      case MD_BLOCK_H: {
        Frame top = Pop();
        Element e = InlineBlocks(top);
        e = ftxui::bold(e) | ftxui::color(HeadingColor(top.heading_level));
        Attach(std::move(e), true);
        break;
      }
      case MD_BLOCK_QUOTE: {
        Frame top = Pop();
        Elements rows;
        for (auto& c : top.children) {
          rows.push_back(ftxui::hbox({
              ftxui::text("│ ") | ftxui::color(theme_.quote_marker),
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
                   ftxui::text(bullet),
                   std::move(content),
               }),
               false);
        break;
      }
      case MD_BLOCK_CODE:
      case MD_BLOCK_HTML: {
        Frame top = Pop();
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
        span_decorators_.push_back(ftxui::italic);
        break;
      case MD_SPAN_STRONG:
        span_decorators_.push_back(ftxui::bold);
        break;
      case MD_SPAN_DEL:
        span_decorators_.push_back(ftxui::strikethrough);
        break;
      case MD_SPAN_CODE:
        span_decorators_.push_back(InlineCodeStyle());
        break;
      case MD_SPAN_A: {
        auto* a = static_cast<MD_SPAN_A_DETAIL*>(detail);
        span_decorators_.push_back(LinkStyle(Attr(a->href)));
        break;
      }
      case MD_SPAN_IMG:
        span_decorators_.push_back(ftxui::dim);
        break;
      case MD_SPAN_U:
        span_decorators_.push_back(ftxui::underlined);
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
        if (!span_decorators_.empty()) {
          span_decorators_.pop_back();
        }
        break;
      default:
        break;
    }
    return 0;
  }

  int TextImpl(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size) {
    std::string s(reinterpret_cast<const char*>(text), size);
    switch (type) {
      case MD_TEXT_NORMAL:
      case MD_TEXT_ENTITY:
      case MD_TEXT_NULLCHAR:
        EmitInline(s);
        break;
      case MD_TEXT_SOFTBR:
        EmitInline(" ");
        break;
      case MD_TEXT_BR:
        // A trailing-streak hard break ends the current inline row instead of
        // embedding a "\n" text node (which would inflate the row height and
        // make background decorators bleed onto the line below).
        FlushRow(Top());
        break;
      case MD_TEXT_CODE: {
        Frame& top = Top();
        if (top.kind == Kind::Code || top.kind == Kind::Html) {
          top.text += s;
        } else {
          EmitInline(s);
        }
        break;
      }
      case MD_TEXT_HTML:
        // Raw HTML block markup is buffered verbatim inside the current
        // Code/Html frame (like fenced code). Inline HTML inside running
        // text has no verbatim frame, so emit it verbatim with inline-code
        // styling instead of dropping it.
        if (Top().kind == Kind::Code || Top().kind == Kind::Html) {
          Top().text += s;
        } else {
          span_decorators_.push_back(InlineCodeStyle());
          EmitInline(s);
          span_decorators_.pop_back();
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
    std::string cur;
    for (char c : code) {
      if (c == '\n') {
        lines.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty() || code.empty()) {
      lines.push_back(cur);
    }

    auto token_text = [this](std::string s) {
      return ftxui::text(std::move(s)) | ftxui::color(theme_.code_block_fg);
    };

    Elements rows;
    rows.reserve(lines.size());
    for (auto& l : lines) {
      if (!wrap || l.empty()) {
        rows.push_back(token_text(l));
        continue;
      }
      // Wrap mode: split the raw line into word elements (same plain-prefix
      // glue as WrapRow: inter-word whitespace rides along with the following
      // token, leading indentation stays with the first token), then hflow
      // reflows the row at the available width. Tokens longer than the
      // viewport are never split mid-word; they clip.
      Elements toks;
      std::string pending;
      size_t i = 0;
      const size_t n = l.size();
      while (i < n) {
        if (l[i] == ' ' || l[i] == '\t') {
          size_t j = i;
          while (j < n && (l[j] == ' ' || l[j] == '\t')) {
            ++j;
          }
          pending += l.substr(i, j - i);
          i = j;
        } else {
          size_t j = i;
          while (j < n && l[j] != ' ' && l[j] != '\t') {
            ++j;
          }
          toks.push_back(token_text(pending + l.substr(i, j - i)));
          pending.clear();
          i = j;
        }
      }
      if (toks.empty()) {
        toks.push_back(token_text(""));
      }
      rows.push_back(ftxui::hflow(std::move(toks)));
    }

    if (rows.empty()) {
      rows.push_back(ftxui::text(""));
    }
    // Wrap mode: the box spans the full content width and hflow reflows each
    // line inside it. Scroll mode: the hbox wrapper makes the border hug the
    // widest line (natural width) instead of stretching to the window, so the
    // box clips/panes rather than reflowing.
    auto boxed = ftxui::vbox(std::move(rows)) | ftxui::bgcolor(theme_.code_block_bg) |
                 ftxui::borderLight;
    if (wrap) {
      return boxed;
    }
    return ftxui::hbox(boxed);
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
              ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 1)(ftxui::text(" ")));
        }
        ++i;
      }
      rows_el.push_back(ftxui::hbox(std::move(row_cells)));
    }
    return ftxui::vbox(std::move(rows_el)) | ftxui::borderLight;
  }

  // --- Theme-styled helpers --------------------------------------------------

  Decorator LinkStyle(const std::string& href) {
    return ftxui::color(theme_.link) | ftxui::underlined | ftxui::hyperlink(href);
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

  std::string source_;
  const Theme& theme_;
  const bool wrap_;
  std::vector<Frame> frames_;
  std::vector<Decorator> span_decorators_;
};

}  // namespace

Element RenderMarkdown(const std::string& markdown, const Config& config) {
  return Renderer(markdown, config).Run();
}

}  // namespace markit
