// Functional tests for markdown rendering.
#include <gtest/gtest.h>

#include <string>  // for string

#include <ftxui/dom/elements.hpp>  // for Element, Render
#include <ftxui/dom/node.hpp>      // for Render (into Screen)
#include <ftxui/screen/screen.hpp>  // for Screen

#include "markdown.hpp"

namespace {

// Render markdown to a fixed-size screen and return its text lines.
std::vector<std::string> RenderLines(const std::string& markdown,
                                     int width = 60, int height = 60) {
  ftxui::Screen screen(width, height);
  ftxui::Render(screen, markit::RenderMarkdown(markdown));
  std::string out = screen.ToString();
  // Strip ANSI escape sequences (styling) so plain-text assertions work.
  std::string plain;
  plain.reserve(out.size());
  for (size_t i = 0; i < out.size(); ++i) {
    char c = out[i];
    if (c == '\x1b') {
      if (i + 1 < out.size() && out[i + 1] == '[') {
        i += 2;
        while (i < out.size() && !((out[i] >= 'A' && out[i] <= 'Z') ||
                                   (out[i] >= 'a' && out[i] <= 'z'))) {
          ++i;
        }
      }
      continue;
    }
    plain += c;
  }
  std::vector<std::string> lines;
  std::string cur;
  for (char c : plain) {
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else if (c == '\r') {
      // carriage return in terminals; ignore
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) {
    lines.push_back(cur);
  }
  return lines;
}

bool AnyLineContains(const std::vector<std::string>& lines,
                     const std::string& needle) {
  for (const auto& l : lines) {
    if (l.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

// Headings render with their text.
TEST(Markdown, Heading) {
  auto rows = RenderLines("# Hello\n## World\n");
  EXPECT_TRUE(AnyLineContains(rows, "Hello"));
  EXPECT_TRUE(AnyLineContains(rows, "World"));
}

// Bold and italic text survive rendering.
TEST(Markdown, Emphasis) {
  auto rows = RenderLines("some **bold** and *italic* text\n");
  EXPECT_TRUE(AnyLineContains(rows, "some bold and italic text"));
}

// Inline and fenced code blocks render their contents.
TEST(Markdown, Code) {
  auto rows = RenderLines("`inline` code\n\n```\nint x = 1;\n```\n");
  EXPECT_TRUE(AnyLineContains(rows, "inline"));
  EXPECT_TRUE(AnyLineContains(rows, "int x = 1;"));
}

// Unordered and ordered lists render their sources markers (bullet char /
// number), not the numerically mislabeled markers from the old aggregate-init
// bug that made `* alpha` render as `0. alpha`.
TEST(Markdown, Lists) {
  auto rows = RenderLines("* alpha\n* beta\n\n1. one\n2. two\n");
  EXPECT_TRUE(AnyLineContains(rows, "* alpha"));
  EXPECT_TRUE(AnyLineContains(rows, "* beta"));
  EXPECT_TRUE(AnyLineContains(rows, "1. one"));
  EXPECT_TRUE(AnyLineContains(rows, "2. two"));
  EXPECT_FALSE(AnyLineContains(rows, "0. alpha"));
  EXPECT_FALSE(AnyLineContains(rows, "0. beta"));
}

// Dash-marker lists (the CHANGELOG.md style) keep their bullet character.
TEST(Markdown, DashBulletList) {
  auto rows = RenderLines("- add - First change\n- mod - Second change\n");
  EXPECT_TRUE(AnyLineContains(rows, "- add - First change"));
  EXPECT_TRUE(AnyLineContains(rows, "- mod - Second change"));
}

// Ordered lists honor the starting index from the source.
TEST(Markdown, OrderedListStartIndex) {
  auto rows = RenderLines("5. five\n6. six\n");
  EXPECT_TRUE(AnyLineContains(rows, "5. five"));
  EXPECT_TRUE(AnyLineContains(rows, "6. six"));
}

// Task list items are recognized.
TEST(Markdown, TaskList) {
  auto rows = RenderLines("- [ ] pending\n- [x] done\n");
  EXPECT_TRUE(AnyLineContains(rows, "[ ] pending"));
  EXPECT_TRUE(AnyLineContains(rows, "[x] done"));
}

// Trailing-space hard breaks split a paragraph into one row per source line.
// Regression: the old "\n" text node inflated the paragraph to two rows, so
// inline-code background colors bled into the line below and the lines were
// concatenated horizontally instead of stacked.
TEST(Markdown, HardBreakLines) {
  auto rows = RenderLines("line one  \nline two with `code`  \nline three\n");
  for (auto& line : rows) {
    while (!line.empty() && line.back() == ' ') {
      line.pop_back();
    }
  }
  ASSERT_GE(rows.size(), 3u);
  EXPECT_EQ(rows[0], "line one");
  EXPECT_EQ(rows[1], "line two with code");
  EXPECT_EQ(rows[2], "line three");
}

// A lone inline code span must not paint a full-width background: without an
// hbox wrapper its bgcolor decorator fills the whole row. Check raw cell
// backgrounds just past the code text (they must stay at the default color).
TEST(Markdown, InlineCodeBackgroundStaysInline) {
  ftxui::Screen screen(40, 10);
  ftxui::Render(screen, markit::RenderMarkdown("`code`\n"));
  const auto baseline = screen.CellAt(0, 2).background_color;  // empty row
  EXPECT_NE(screen.CellAt(1, 0).background_color, baseline);    // under `code`
  EXPECT_EQ(screen.CellAt(6, 0).background_color, baseline);    // past `code`
}

// Links render with their label; the href is embedded via the hyperlink
// decorator (not visible as plain text), but the label must appear.
TEST(Markdown, Link) {
  auto rows = RenderLines("[click me](https://example.com)\n");
  EXPECT_TRUE(AnyLineContains(rows, "click me"));
}

// Horizontal rules are emitted as separators (no crash, some content present).
TEST(Markdown, HorizontalRule) {
  auto rows = RenderLines("above\n\n---\n\nbelow\n");
  EXPECT_TRUE(AnyLineContains(rows, "above"));
  EXPECT_TRUE(AnyLineContains(rows, "below"));
}

// GFM tables render all cell text.
TEST(Markdown, Table) {
  auto rows = RenderLines(
      "| a | b |\n| --- | --- |\n| 1 | 2 |\n| 3 | 4 |\n");
  EXPECT_TRUE(AnyLineContains(rows, "a"));
  EXPECT_TRUE(AnyLineContains(rows, "b"));
  EXPECT_TRUE(AnyLineContains(rows, "1"));
  EXPECT_TRUE(AnyLineContains(rows, "2"));
  EXPECT_TRUE(AnyLineContains(rows, "3"));
  EXPECT_TRUE(AnyLineContains(rows, "4"));
}

// Blockquotes render their content.
TEST(Markdown, Blockquote) {
  auto rows = RenderLines("> quoted line\n");
  EXPECT_TRUE(AnyLineContains(rows, "quoted line"));
}

// Regression: a full doc where a blockquote/code block precedes a list must
// still render the list item text. A frame-stack desync used to drop it.
TEST(Markdown, ListTextSurvivesAfterPrecedingBlocks) {
  auto rows = RenderLines(
      "> quote\n\n"
      "```\ncode\n```\n\n"
      "* alpha\n"
      "* beta\n\n"
      "1. one\n"
      "2. two\n\n"
      "- [ ] todo\n"
      "- [x] done\n");
  EXPECT_TRUE(AnyLineContains(rows, "alpha"));
  EXPECT_TRUE(AnyLineContains(rows, "beta"));
  EXPECT_TRUE(AnyLineContains(rows, "one"));
  EXPECT_TRUE(AnyLineContains(rows, "two"));
  EXPECT_TRUE(AnyLineContains(rows, "todo"));
  EXPECT_TRUE(AnyLineContains(rows, "done"));
}

// Two-column table cells must be separated by a gutter, not run together.
TEST(Markdown, TableHasColumnGap) {
  auto rows = RenderLines("| a | b |\n| --- | --- |\n| 1 | 2 |\n");
  EXPECT_TRUE(AnyLineContains(rows, "a b"));
  EXPECT_TRUE(AnyLineContains(rows, "1 2"));
}
