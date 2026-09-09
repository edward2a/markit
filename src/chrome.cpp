// Implementation of the screen chrome builders. See chrome.hpp.
#include "chrome.hpp"

#include <md4c.h>

#include <string>  // for string, to_string

#include <ftxui/screen/color.hpp>  // for Color

namespace markit {

namespace {

// Strip directory components: "/a/b/c.md" -> "c.md".
std::string Basename(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// md4c SAX collector for ATX/setext headings: level from the block detail,
// plain text from every text chunk inside (emphasis/code spans contribute
// their inner text, so "# Hello **bold**" yields "Hello bold").
class HeadingCollector {
 public:
  std::vector<Heading> Run(const std::string& markdown) {
    source_ = &markdown;
    MD_PARSER parser = {};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB;
    parser.enter_block = &HeadingCollector::cb_enter_block;
    parser.leave_block = &HeadingCollector::cb_leave_block;
    parser.enter_span = &HeadingCollector::cb_enter_span;
    parser.leave_span = &HeadingCollector::cb_leave_span;
    parser.text = &HeadingCollector::cb_text;
    md_parse(reinterpret_cast<const MD_CHAR*>(markdown.data()),
             markdown.size(), &parser, this);
    return std::move(headings_);
  }

 private:
  static int cb_enter_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto* self = static_cast<HeadingCollector*>(userdata);
    if (type == MD_BLOCK_H) {
      auto* h = static_cast<MD_BLOCK_H_DETAIL*>(detail);
      self->level_ = h->level;
      self->current_.clear();
    }
    return 0;
  }
  static int cb_leave_block(MD_BLOCKTYPE type, void* /*detail*/,
                            void* userdata) {
    auto* self = static_cast<HeadingCollector*>(userdata);
    if (type == MD_BLOCK_H) {
      self->headings_.push_back(Heading{self->level_, self->current_});
      self->level_ = 0;
    }
    return 0;
  }
  static int cb_enter_span(MD_SPANTYPE /*type*/, void* /*detail*/,
                           void* /*userdata*/) {
    return 0;
  }
  static int cb_leave_span(MD_SPANTYPE /*type*/, void* /*detail*/,
                           void* /*userdata*/) {
    return 0;
  }
  static int cb_text(MD_TEXTTYPE /*type*/, const MD_CHAR* text, MD_SIZE size,
                     void* userdata) {
    auto* self = static_cast<HeadingCollector*>(userdata);
    if (self->level_ > 0) {
      self->current_.append(text, size);
    }
    return 0;
  }

  const std::string* source_ = nullptr;
  std::vector<Heading> headings_;
  int level_ = 0;  // >0 while inside a heading block.
  std::string current_;
};

}  // namespace

std::vector<Heading> ExtractHeadings(const std::string& markdown) {
  HeadingCollector collector;
  return collector.Run(markdown);
}

std::string FormatPosition(int selected, int max_offset, WrapMode mode) {
  if (max_offset <= 0) {
    max_offset = 0;
    selected = 0;
  } else {
    if (selected < 0) {
      selected = 0;
    } else if (selected > max_offset) {
      selected = max_offset;
    }
  }
  const int percent =
      max_offset == 0 ? 100 : (100 * selected + max_offset / 2) / max_offset;
  return std::to_string(selected + 1) + "/" + std::to_string(max_offset + 1) +
         "  " + std::to_string(percent) + "%  " +
         (mode == WrapMode::Scroll ? "scroll" : "wrap");
}

ftxui::Element StatusBar(const std::string& filename, int selected,
                         int max_offset, WrapMode mode) {
  using namespace ftxui;
  return hbox({
             text(Basename(filename)),
             text("") | flex,
             text(FormatPosition(selected, max_offset, mode)),
         }) |
         inverted;
}

ftxui::Element ActionBar() {
  using namespace ftxui;
  return text("q:quit  w:wrap/scroll  n:nav  j/k+arrows:scroll  "
              "PgUp/PgDn:page  Home/End:top/bottom  h/l+arrows:pan (scroll)") |
         dim;
}

ftxui::Element NavBar(const std::vector<Heading>& headings, int current) {
  using namespace ftxui;
  Elements rows;
  rows.push_back(text("Outline") | bold);
  rows.push_back(separator());
  if (headings.empty()) {
    rows.push_back(text("(no headings)") | dim);
  }
  for (std::size_t i = 0; i < headings.size(); ++i) {
    const Heading& h = headings[i];
    Element row = text(std::string((h.level - 1) * 2, ' ') + h.text);
    if (static_cast<int>(i) == current) {
      // Section in view: bold in the accent color (width-neutral, so the
      // 30-column layout never shifts when the highlight moves).
      rows.push_back(row | bold | color(Color::Yellow));
    } else {
      rows.push_back(h.level <= 1 ? row | bold : row | dim);
    }
  }
  return vbox(std::move(rows)) | size(WIDTH, EQUAL, kNavWidth);
}

}  // namespace markit
