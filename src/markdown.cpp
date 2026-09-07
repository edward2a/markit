// Implementation of markdown rendering for markit. See markdown.hpp.
#include "markdown.hpp"

#include <md4c.h>

#include <algorithm>  // for max
#include <cctype>     // for tolower/isspace/isalnum (HTML tag parsing)
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

    FlushPendingHtml();  // trailing verbatim HTML coalesced across blocks.
    // A heading at EOF still owes its trailing gap, but there is no next
    // block to take it: drop it (a trailing blank row would be invisible).
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

  std::string ComposedStyleKey() const {
    std::string key;
    for (const auto& k : span_keys_) {
      if (!key.empty()) {
        key += '\x1f';
      }
      key += k;
    }
    return key;
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
    top.inline_.push_back(Fragment{text, ComposedStyle(), ComposedStyleKey()});
  }

  // Split a row's fragments into word elements for wrap mode. A "word" is a
  // maximal run of non-space text; it may span several styled fragments (e.g.
  // `[a](url)b`), whose pieces are glued into one element so the wrap layout
  // never breaks inside a word. Inter-word whitespace is glued ahead of the
  // following word so the wrapping point lands exactly at the source's
  // whitespace, and it keeps the word's style when the preceding piece has
  // the same style key (whitespace *inside* a styled run, e.g. a multi-word
  // link label, stays underlined like scroll mode renders it); at a style
  // boundary it stays a plain piece. Overlong tokens (longer than the
  // viewport) are never split mid-word; they clip. Trailing whitespace is
  // kept as a final plain piece so an all-space row keeps its height.
  Element WrapRow(const std::vector<Fragment>& fragments) {
    struct Piece {
      std::string text;
      Decorator style;
      std::string key;
    };
    Elements words;
    std::vector<Piece> cur;
    std::string pending;
    std::string last_key;  // style key of the previously emitted piece.

    auto finish = [&]() {
      if (cur.empty()) {
        return;
      }
      Elements pieces;
      pieces.reserve(cur.size());
      for (auto& piece : cur) {
        Element e = ftxui::text(std::move(piece.text));
        if (piece.style) {
          e = piece.style(std::move(e));
        }
        pieces.push_back(std::move(e));
      }
      last_key = cur.back().key;
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
            // Same-style run: the space belongs to the styled text (scroll
            // mode renders it as one continuous element), so keep the style.
            // At a style boundary the space stays plain.
            const bool internal = (last_key == frag.style_key);
            cur.push_back({std::move(pending),
                           internal ? frag.style : Decorator(nullptr),
                           internal ? frag.style_key : std::string()});
            pending.clear();
          }
          cur.push_back({std::move(word), frag.style, frag.style_key});
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

  // ---- HTML rendering ---------------------------------------------------
  // md4c delivers raw HTML as opaque MD_TEXT_HTML chunks (it never parses
  // tags). Inside an HTML block, interpret a small subset of tags and map
  // them onto the same decorators/frames markdown uses; anything else
  // (<pre>, <script>/<style>, unknown tags) stays verbatim in a code box.
  // md4c splits one HTML run into several MD_BLOCK_HTML blocks at blank
  // lines, so pure-verbatim frames coalesce into pending_html_ and attach
  // as a single box instead of one box per block.

  static std::string ToLower(std::string s) {
    for (char& c : s) {
      c = static_cast<char>(
          std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
  }

  static bool IsHtmlSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }

  static bool IsAllSpace(const std::string& s) {
    for (char c : s) {
      if (!IsHtmlSpace(c)) {
        return false;
      }
    }
    return true;
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

  void HtmlInlineText(const std::string& text) {
    if (text.empty()) {
      return;
    }
    if (IsInlineCapable(Top().kind)) {
      EmitInline(text);
      return;
    }
    if (IsAllSpace(text)) {
      return;  // indentation/newlines between block tags.
    }
    const size_t before = frames_.size();
    EnsureHtmlPara();
    if (frames_.size() == before) {
      return;  // unexpected frame; drop rather than corrupt the stack.
    }
    EmitInline(text);
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
  void HtmlVerbatim(const std::string& text) {
    if (text.empty()) {
      return;
    }
    Frame& top = Top();
    if (IsInlineCapable(top.kind)) {
      span_decorators_.push_back(InlineCodeStyle());
      span_keys_.push_back("code");
      EmitInline(text);
      span_decorators_.pop_back();
      span_keys_.pop_back();
      return;
    }
    if (top.kind == Kind::Html) {
      top.text += text;
      return;
    }
    Attach(CodeElement(text, wrap_), true);
  }

  // Parse `name="value"` / `name='value'` / `name=value` / bare names from a
  // tag body (everything between `<name` and `>`). Names are lowercased.
  static std::vector<std::pair<std::string, std::string>> ParseHtmlAttrs(
      const std::string& body) {
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

  static std::string HtmlAttr(
      const std::vector<std::pair<std::string, std::string>>& attrs,
      const std::string& name) {
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
      const std::string& name) {
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
  void HtmlOpenSpan(Decorator style, const std::string& key) {
    EnsureHtmlPara();
    if (!IsInlineCapable(Top().kind)) {
      return;
    }
    span_decorators_.push_back(std::move(style));
    span_keys_.push_back(key);
    ++html_open_count_;
  }

  // Pop for a closing tag. Strict: only the matching entry goes, so a stray
  // close in a markdown paragraph can never pop a markdown span (or vice
  // versa); unclosed entries die with the enclosing block anyway.
  void HtmlCloseSpan(const std::string& key, bool prefix = false) {
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
    span_decorators_.pop_back();
    span_keys_.pop_back();
    --html_open_count_;
  }

  void HandleHtmlTag(const std::string& inner, const std::string& raw) {
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
        HtmlCloseSpan("a:", true);
      } else {
        const std::string href = HtmlAttr(attrs, "href");
        HtmlOpenSpan(href.empty() ? Decorator(ftxui::underlined)
                                  : LinkStyle(href),
                     "a:" + href);
      }
      return;
    }
    if (name == "img") {
      if (!closing) {
        EnsureHtmlPara();
        if (!IsInlineCapable(Top().kind)) {
          return;
        }
        const std::string alt = HtmlAttr(attrs, "alt");
        span_decorators_.push_back(ftxui::dim);
        span_keys_.push_back("img");
        EmitInline(alt.empty() ? "[img]" : alt);
        span_decorators_.pop_back();
        span_keys_.pop_back();
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
          span_decorators_.push_back(InlineCodeStyle());
          span_keys_.push_back("code");
          EmitInline(raw_buf_);
          span_decorators_.pop_back();
          span_keys_.pop_back();
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
        AttachHr(ftxui::separator());
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
                                 const std::string& name) {
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
      if (ToLower(buf.substr(k, m - k)) != name) {
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
  void HtmlText(const std::string& s) {
    html_chunk_ += s;
    size_t pos = 0;
    while (pos < html_chunk_.size()) {
      if (!raw_tag_.empty()) {
        const size_t lt = FindHtmlCloseTag(html_chunk_, pos, raw_tag_);
        if (lt == std::string::npos) {
          raw_buf_ += html_chunk_.substr(pos);
          html_chunk_.clear();
          return;
        }
        raw_buf_ += html_chunk_.substr(pos, lt - pos);
        const size_t gt = html_chunk_.find('>', lt + 1);
        // Keep the '/' prefix: HandleHtmlTag detects closes from inner[0].
        const std::string inner =
            "/" + html_chunk_.substr(lt + 2, gt - lt - 2);
        const std::string raw = html_chunk_.substr(lt, gt - lt + 1);
        html_chunk_ = html_chunk_.substr(gt + 1);
        pos = 0;
        HandleHtmlTag(inner, raw);
        continue;
      }
      const size_t lt = html_chunk_.find('<', pos);
      if (lt == std::string::npos) {
        HtmlInlineText(html_chunk_.substr(pos));
        html_chunk_.clear();
        return;
      }
      if (lt > pos) {
        HtmlInlineText(html_chunk_.substr(pos, lt - pos));
      }
      if (html_chunk_.compare(lt, 4, "<!--") == 0) {
        const size_t end = html_chunk_.find("-->", lt + 4);
        if (end == std::string::npos) {
          html_chunk_ = html_chunk_.substr(lt);
          return;
        }
        HtmlVerbatim(html_chunk_.substr(lt, end + 3 - lt));
        pos = end + 3;
        continue;
      }
      const size_t gt = html_chunk_.find('>', lt + 1);
      if (gt == std::string::npos) {
        html_chunk_ = html_chunk_.substr(lt);  // partial tag; await more.
        return;
      }
      HandleHtmlTag(html_chunk_.substr(lt + 1, gt - lt - 1),
                    html_chunk_.substr(lt, gt - lt + 1));
      pos = gt + 1;
    }
    html_chunk_.clear();
  }

  // End inline-HTML interpretation for a markdown block: a tag split
  // across md4c callbacks that never completed shows literally (rather than
  // leaking into the next block), and unclosed tag entries reset so later
  // stray closes have nothing to pop. No-op inside HTML blocks (the HTML
  // leave path handles its own state).
  void EndHtmlParaContext() {
    if (in_html_) {
      return;
    }
    if (!html_chunk_.empty()) {
      span_decorators_.push_back(InlineCodeStyle());
      span_keys_.push_back("code");
      EmitInline(html_chunk_);
      span_decorators_.pop_back();
      span_keys_.pop_back();
      html_chunk_.clear();
    }
    html_open_count_ = 0;
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
        break;
      }
      case MD_BLOCK_HR:
        AttachHr(ftxui::separator());
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
        Attach(InlineBlocks(top), false);
        break;
      }
      case MD_BLOCK_H: {
        EndHtmlParaContext();
        Frame top = Pop();
        Element e = InlineBlocks(top);
        e = ftxui::bold(e) | ftxui::color(HeadingColor(top.heading_level));
        Attach(std::move(e), true);
        OweHeadingGap();
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
                   ftxui::text(bullet),
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
            span_decorators_.resize(span_depth_);
            span_keys_.resize(span_depth_);
          }
          html_open_count_ = 0;
          html_align_ = 0;
          summary_open_ = false;
          details_stack_.clear();
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
        span_decorators_.push_back(ftxui::italic);
        span_keys_.push_back("em");
        break;
      case MD_SPAN_STRONG:
        span_decorators_.push_back(ftxui::bold);
        span_keys_.push_back("strong");
        break;
      case MD_SPAN_DEL:
        span_decorators_.push_back(ftxui::strikethrough);
        span_keys_.push_back("del");
        break;
      case MD_SPAN_CODE:
        span_decorators_.push_back(InlineCodeStyle());
        span_keys_.push_back("code");
        break;
      case MD_SPAN_A: {
        auto* a = static_cast<MD_SPAN_A_DETAIL*>(detail);
        const std::string href = Attr(a->href);
        span_decorators_.push_back(LinkStyle(href));
        span_keys_.push_back("a:" + href);
        break;
      }
      case MD_SPAN_IMG:
        span_decorators_.push_back(ftxui::dim);
        span_keys_.push_back("img");
        break;
      case MD_SPAN_U:
        span_decorators_.push_back(ftxui::underlined);
        span_keys_.push_back("u");
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
        if (!span_keys_.empty()) {
          span_keys_.pop_back();
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
        if (in_html_) {
          HtmlBreak();
        } else {
          FlushRow(Top());
        }
        break;
      case MD_TEXT_CODE: {
        Frame& top = Top();
        if (top.kind == Kind::Code) {
          top.text += s;
        } else if (in_html_) {
          HtmlText(s);
        } else {
          EmitInline(s);
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
    // Both modes: the box spans the full content width, like tables. In wrap
    // mode hflow reflows each line inside it; in scroll mode the box keeps
    // its natural width and pans with the rest of the content.
    return ftxui::vbox(std::move(rows)) | ftxui::bgcolor(theme_.code_block_bg) |
           ftxui::borderLight;
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
  std::vector<std::string> span_keys_;
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
};

}  // namespace

Element RenderMarkdown(const std::string& markdown, const Config& config) {
  return Renderer(markdown, config).Run();
}

}  // namespace markit
