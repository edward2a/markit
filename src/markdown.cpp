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
  return std::string(attr.text, attr.size);
}

Decorator LinkStyle(const std::string& href) {
  return ftxui::color(Color::CyanLight) | ftxui::underlined |
         ftxui::hyperlink(href);
}

Decorator InlineCodeStyle() {
  return ftxui::color(Color::Green) | ftxui::bgcolor(Color::GrayDark);
}

Color HeadingColor(unsigned level) {
  switch (level) {
    case 1:
      return Color::Red;
    case 2:
      return Color::Yellow;
    case 3:
      return Color::Green;
    default:
      return Color::CyanLight;
  }
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
  explicit Renderer(std::string markdown) : source_(std::move(markdown)) {}

  Element Run() {
    MD_PARSER parser = {};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB;

    frames_.push_back(Frame{Kind::Doc, 0, 0, false});
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
    bool is_task = false;
    char task_mark = ' ';
    char bullet = 0;              // ul marker; 0 means ordered
    unsigned ordered_index = 0;
    char ordered_mark = '.';
    std::vector<Element> children;
    std::vector<Fragment> inline_;
    std::string text;              // verbatim buffer (code/html)
    std::vector<std::pair<bool, std::vector<Element>>> table_rows;
    std::vector<Element> cells;    // current row cells
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
    for (const auto& d : span_decorators_) {
      if (result == nullptr) {
        result = d;
      } else {
        Decorator outer = result;
        result = [outer, d](Element e) { return d(outer(std::move(e))); };
      }
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

  Element FlattenInline(Frame& frame) {
    Element prev = nullptr;
    for (auto& frag : frame.inline_) {
      Element e = ftxui::text(std::move(frag.text));
      if (frag.style) {
        e = frag.style(std::move(e));
      }
      prev = (prev == nullptr) ? std::move(e)
                               : ftxui::hbox({std::move(prev), std::move(e)});
    }
    frame.inline_.clear();
    return (prev == nullptr) ? ftxui::text("") : std::move(prev);
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
        Push(Frame{Kind::Para, 0, 0, false});
        break;
      case MD_BLOCK_H: {
        auto* h = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        Push(Frame{Kind::Heading, h->level, 0, false});
        break;
      }
      case MD_BLOCK_HR:
        Attach(ftxui::separator(), true);
        break;
      case MD_BLOCK_QUOTE:
        Push(Frame{Kind::Quote, 0, 0, false});
        break;
      case MD_BLOCK_UL: {
        auto* ul = static_cast<MD_BLOCK_UL_DETAIL*>(detail);
        Push(Frame{Kind::List, 0, 0, false, 0, 0, ul->mark});
        break;
      }
      case MD_BLOCK_OL: {
        auto* ol = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        Push(Frame{Kind::List, 0, 0, false, 0, 0, 0, ol->start,
                   ol->mark_delimiter});
        break;
      }
      case MD_BLOCK_LI: {
        auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        Frame& list = Top();  // enclosing list
        Frame f{Kind::Item, 0, 0, false};
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
        Push(Frame{Kind::Code, 0, 0, false});
        break;
      case MD_BLOCK_HTML:
        Push(Frame{Kind::Html, 0, 0, false});
        break;
      case MD_BLOCK_TABLE: {
        auto* t = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail);
        Push(Frame{Kind::Table, 0, t->col_count, false});
        break;
      }
      case MD_BLOCK_THEAD:
      case MD_BLOCK_TBODY:
        break;
      case MD_BLOCK_TR:
        Push(Frame{Kind::Row, 0, 0, true});
        break;
      case MD_BLOCK_TH:
      case MD_BLOCK_TD: {
        auto* td = static_cast<MD_BLOCK_TD_DETAIL*>(detail);
        Frame f{Kind::Cell, 0, 0, type == MD_BLOCK_TH};
        f.ordered_mark = static_cast<char>(td->align);
        Push(std::move(f));
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
        Attach(FlattenInline(top), false);
        break;
      }
      case MD_BLOCK_H: {
        Frame top = Pop();
        Element e = FlattenInline(top);
        e = ftxui::bold(e) | ftxui::color(HeadingColor(top.heading_level));
        Attach(std::move(e), true);
        break;
      }
      case MD_BLOCK_QUOTE: {
        Frame top = Pop();
        Elements rows;
        for (auto& c : top.children) {
          rows.push_back(ftxui::hbox({
              ftxui::text("│ ") | ftxui::color(Color::GrayDark),
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
        if (!top.inline_.empty()) {
          top.children.insert(top.children.begin(), FlattenInline(top));
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
        Attach(CodeElement(top.text), true);
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
        if (row.kind == Kind::Row) {
          row.cells.push_back(FlattenInline(top));
        }
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
        EmitInline("\n");
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
      default:
        break;
    }
    return 0;
  }

  // ---- finalizers ---------------------------------------------------------

  static Element CodeElement(const std::string& code) {
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
    Elements rows;
    for (auto& l : lines) {
      rows.push_back(ftxui::text(l) | ftxui::color(Color::GrayLight));
    }
    if (rows.empty()) {
      rows.push_back(ftxui::text(""));
    }
    return ftxui::vbox(std::move(rows)) | ftxui::bgcolor(Color::GrayDark) |
           ftxui::borderLight;
  }

  static Element TableElement(
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
          auto req = cell->requirement();
          widths[i] =
              std::max(widths[i], static_cast<unsigned>(req.min_x));
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

  std::string source_;
  std::vector<Frame> frames_;
  std::vector<Decorator> span_decorators_;
};

}  // namespace

Element RenderMarkdown(const std::string& markdown) {
  return Renderer(markdown).Run();
}

}  // namespace markit
