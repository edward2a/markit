// Implementation of the screen chrome builders. See chrome.hpp.
#include "chrome.hpp"

#include <md4c.h>

#include <algorithm>  // for clamp, max
#include <string>  // for string, to_string
#include <utility>  // for pair

#include <ftxui/dom/node.hpp>  // for Node, Render
#include <ftxui/screen/color.hpp>  // for Color
#include <ftxui/screen/screen.hpp>  // for Screen, Cell

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
                         int max_offset, WrapMode mode,
                         const std::string& search_suffix) {
  using namespace ftxui;
  std::string right = FormatPosition(selected, max_offset, mode);
  if (!search_suffix.empty()) {
    right += "  " + search_suffix;
  }
  return hbox({
             text(Basename(filename)),
             text("") | flex,
             text(right),
         }) |
         inverted;
}

std::string FormatSearchStatus(int current, int total, bool invalid) {
  if (invalid) {
    return "invalid pattern";
  }
  if (total <= 0) {
    return "no matches";
  }
  if (current < 0 || current >= total) {
    return std::to_string(total) + " matches";
  }
  return std::to_string(current + 1) + "/" + std::to_string(total);
}

ftxui::Element ActionBar(bool nav_focused) {
  using namespace ftxui;
  return text(nav_focused
                  ? "Tab:main  Up/Down:move  PgUp/PgDn:page  Home/End:first/"
                    "last  Enter:goto section  /:search  n/N:match"
                  : "q:quit  w:wrap/scroll  Ctrl+N:nav  Tab:focus  /:search  "
                    "n/N:match  j/k+arrows:scroll  PgUp/PgDn:page  Home/End:"
                    "top/bottom  h/l+arrows:pan (scroll)") |
         dim;
}

ftxui::Element NavBar(const std::vector<Heading>& headings, int current,
                      int cursor, bool focused) {
  using namespace ftxui;
  Elements rows;
  Element title = text("Outline") | bold;
  if (focused) {
    title = title | inverted;
  }
  rows.push_back(title);
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
      row = row | bold | color(Color::Yellow);
    } else if (h.level <= 1) {
      row = row | bold;
    } else {
      row = row | dim;
    }
    if (focused && static_cast<int>(i) == cursor) {
      // Keyboard cursor: inverted, independent of the view highlight
      // (also width-neutral). Combines with the accent when both land on
      // the same row.
      row = row | inverted;
    }
    rows.push_back(row);
  }
  return vbox(std::move(rows)) | size(WIDTH, EQUAL, kNavWidth);
}

int ClampNavOffset(int offset, int count, int visible) {
  const int max_offset = std::max(0, count - std::max(1, visible));
  return std::clamp(offset, 0, max_offset);
}

int FollowNavOffset(int offset, int cursor, int count, int visible) {
  const int window = std::max(1, visible);
  if (cursor < offset) {
    offset = cursor;
  } else if (cursor >= offset + window) {
    offset = cursor - window + 1;
  }
  return ClampNavOffset(offset, count, visible);
}

namespace {

// Post-render mark for the current search match: the child renders normally,
// then the cells overlapping the match byte spans flip `inverted`. Bytes map
// to cells by walking the row's graphemes (a cell's `character` carries its
// own byte length), so wide/combining characters land correctly. Out-of-view
// writes are swallowed by the screen stencil; the walk stops past the last
// span's bytes, so an oversized box costs nothing.
class SearchHighlightNode : public ftxui::Node {
 public:
  SearchHighlightNode(ftxui::Element child, const int* row,
                      const std::vector<std::pair<int, int>>* spans)
      : Node({std::move(child)}), row_(row), spans_(spans) {}

  void SetBox(ftxui::Box box) override {
    Node::SetBox(box);
    children_[0]->SetBox(box);
  }

  void Render(ftxui::Screen& screen) override {
    children_[0]->Render(screen);
    if (row_ == nullptr || spans_ == nullptr || spans_->empty()) {
      return;
    }
    const int row = *row_;
    // requirement_ is the child's (Node::ComputeRequirement forwards it):
    // a row outside the content needs no walk at all.
    if (row < 0 || row >= requirement_.min_y) {
      return;
    }
    int last_end = 0;
    for (const auto& [s, e] : *spans_) {
      last_end = std::max(last_end, e);
    }
    const int y = box_.y_min + row;
    int consumed = 0;  // row-text bytes in the cells passed so far.
    for (int x = box_.x_min; x <= box_.x_max; ++x) {
      ftxui::Cell& cell = screen.CellAt(x, y);
      const int start = consumed;
      consumed += static_cast<int>(cell.character.size());
      for (const auto& [s, e] : *spans_) {
        if (start < e && consumed > s) {
          cell.inverted ^= true;
          break;
        }
      }
      if (consumed >= last_end) {
        break;  // past every span: later cells cannot overlap (bytes only
                // grow; a zero-byte cell here starts at/after every end).
      }
    }
  }

 private:
  const int* row_;
  const std::vector<std::pair<int, int>>* spans_;
};

}  // namespace

ftxui::Element SearchHighlight(
    ftxui::Element child, const int* row,
    const std::vector<std::pair<int, int>>* spans) {
  return std::make_shared<SearchHighlightNode>(std::move(child), row, spans);
}

}  // namespace markit
