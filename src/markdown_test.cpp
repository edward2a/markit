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
                                     const markit::Config& config = {},
                                     int width = 60, int height = 60) {
  ftxui::Screen screen(width, height);
  ftxui::Render(screen, markit::RenderMarkdown(markdown, config));
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
      } else if (i + 1 < out.size() && out[i + 1] == ']') {
        // OSC (hyperlinks): consume through the BEL or ESC '\' terminator.
        i += 2;
        while (i < out.size() && out[i] != '\x07' &&
               !(out[i] == '\x1b' && i + 1 < out.size() &&
                 out[i + 1] == '\\')) {
          ++i;
        }
        if (i < out.size() && out[i] == '\x1b') {
          ++i;  // past the ESC of the ESC '\' terminator (the '\' is dropped)
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

// Approximate rendered cell width: UTF-8 continuation bytes (0x80..0xBF) do
// not add a column.
size_t CellWidth(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) {
    if (c < 0x80 || c >= 0xC0) {
      ++n;
    }
  }
  return n;
}

// Drop trailing padding each Screen row gets (rows are padded to the screen
// width) and discard rows that become blank.
std::vector<std::string> TrimmedLines(const std::vector<std::string>& rows) {
  std::vector<std::string> out;
  for (auto l : rows) {
    while (!l.empty() && l.back() == ' ') {
      l.pop_back();
    }
    if (!l.empty()) {
      out.push_back(l);
    }
  }
  return out;
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
// Raw HTML blocks render their source verbatim (tags included) instead of
// being dropped: the banner text must appear somewhere in the output.
TEST(Markdown, HtmlBlockRendersVerbatimText) {
  const std::string md =
      "<p align=\"center\">\n"
      "  <img src=\"https://example.com/pic.png\" alt=\"Demo\"/>\n"
      "  Project banner text here.\n"
      "</p>\n"
      "\n"
      "After the banner.\n";
  auto lines = RenderLines(md, {}, 40, 60);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_NE(joined.find("<p align=\"center\">"), std::string::npos)
      << "raw HTML tags must be preserved";
  EXPECT_NE(joined.find("Project banner text here."), std::string::npos)
      << "html block text must not be dropped";
  EXPECT_NE(joined.find("After the banner."), std::string::npos);
}

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

// Wrap mode (the default) reflows a long paragraph onto multiple rows that fit
// the viewport width, preserving all the text.
TEST(Markdown, WrapLongParagraph) {
  const std::string md =
      "The quick brown fox jumps over the lazy dog while the sun sets.\n";
  auto lines = TrimmedLines(RenderLines(md, {}, 40, 60));
  ASSERT_GE(lines.size(), 2u);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_TRUE(joined.find("quick brown fox") != std::string::npos);
  EXPECT_TRUE(joined.find("sun sets") != std::string::npos);
  for (const auto& l : lines) {
    EXPECT_LE(l.size(), 40u) << "row exceeds viewport width";
  }
}

// Scroll mode keeps the paragraph on one long line: nothing reflows, so the
// sentence spills past the 40-column viewport instead of wrapping.
TEST(Markdown, ScrollKeepsSingleLine) {
  markit::Config cfg;
  cfg.horizontal_wrap = markit::WrapMode::Scroll;
  const std::string md =
      "The quick brown fox jumps over the lazy dog while the sun sets.\n";
  auto lines = TrimmedLines(RenderLines(md, cfg, 40, 60));
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_TRUE(lines[0].find("The quick") != std::string::npos);
  EXPECT_TRUE(lines[0].find("sun sets") == std::string::npos);  // overflowed
}

// Wrapped rows keep their styled spans: the emphasis text survives the word
// split and the resulting rows stay within the viewport width.
TEST(Markdown, WrapPreservesStyledText) {
  const std::string md =
      "one **two** three *four* five six seven eight nine ten eleven twelve\n";
  auto lines = TrimmedLines(RenderLines(md, {}, 20, 60));
  ASSERT_GE(lines.size(), 2u);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_TRUE(joined.find("two") != std::string::npos);
  EXPECT_TRUE(joined.find("four") != std::string::npos);
  for (const auto& l : lines) {
    EXPECT_LE(l.size(), 20u) << "row exceeds viewport width";
  }
}

// Regression: the inter-word whitespace before a link stays plain in wrap
// mode. Gluing the space into the styled token underlined/colored the space.
TEST(Markdown, WrapLinkLeadingSpaceIsNotUnderlined) {
  const std::string md = "seen [label](https://e.test) tail\n";
  ftxui::Screen screen(40, 4);
  ftxui::Render(screen, markit::RenderMarkdown(md, {}));

  int link_col = -1;
  for (int c = 0; c < 40; ++c) {
    if (screen.CellAt(c, 0).character == "l") {  // start of "label"
      link_col = c;
      break;
    }
  }
  ASSERT_GE(link_col, 1);
  EXPECT_FALSE(screen.CellAt(link_col - 1, 0).underlined)
      << "space before the link must stay plain";
  EXPECT_TRUE(screen.CellAt(link_col, 0).underlined)
      << "link label must be underlined";
}

// A hard break still forces a new rendered row in wrap mode (it closes the
// current inline row regardless of wrapping).
TEST(Markdown, WrapHardBreakForcesNewLine) {
  const std::string md =
      "aaaaaaaaaaaaaaaaaaaa bbbbbbbbbbbbbbbbbbbb  \n"
      "cccccccccccccccccccc dddddddddddddddddddd\n";
  auto lines = TrimmedLines(RenderLines(md, {}, 60, 60));
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_TRUE(lines[0].find("aaaaaaaaaaaaaaaaaaaa") != std::string::npos);
  EXPECT_TRUE(lines[1].find("cccccccccccccccccccc") != std::string::npos);
}

// In wrap mode, long code-block lines reflow at the available width so the
// box stays inside the window margin instead of being clipped at the edge.
TEST(Markdown, CodeWrapsInWrapMode) {
  const std::string md =
      "```\naaaa bbbb cccc dddd eeee ffff gggg\n```\n";
  auto lines = TrimmedLines(RenderLines(md, {}, 20, 60));
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  // Every token survives the reflow...
  EXPECT_TRUE(joined.find("aaaa") != std::string::npos);
  EXPECT_TRUE(joined.find("gggg") != std::string::npos);
  // ...but the line is no longer on a single row.
  bool aaaa_and_gggg_same_row = false;
  for (const auto& l : lines) {
    if (l.find("aaaa") != std::string::npos &&
        l.find("gggg") != std::string::npos) {
      aaaa_and_gggg_same_row = true;
    }
    EXPECT_LE(CellWidth(l), 20u) << "code row exceeds viewport width";
  }
  EXPECT_FALSE(aaaa_and_gggg_same_row);
}

// In scroll mode the same code line keeps its natural single-line width.
TEST(Markdown, CodeStaysSingleLineInScrollMode) {
  markit::Config cfg;
  cfg.horizontal_wrap = markit::WrapMode::Scroll;
  const std::string md =
      "```\naaaa bbbb cccc dddd eeee ffff gggg\n```\n";
  auto lines = TrimmedLines(RenderLines(md, cfg, 60, 60));
  std::vector<std::string> content_rows;
  for (const auto& l : lines) {
    if (l.find("aaaa") != std::string::npos) {
      content_rows.push_back(l);
    }
  }
  ASSERT_EQ(content_rows.size(), 1u);
  EXPECT_TRUE(content_rows[0].find("gggg") != std::string::npos);
}

// Inline raw HTML inside a paragraph renders verbatim with inline-code
// styling instead of being silently dropped.
TEST(Markdown, InlineHtmlInParagraphIsCodeStyled) {
  ftxui::Screen screen(40, 4);
  ftxui::Render(screen, markit::RenderMarkdown("a <kbd>x</kbd> b\n", {}));
  int tag_col = -1;
  for (int c = 0; c < 40; ++c) {
    if (screen.CellAt(c, 0).character == "<") {
      tag_col = c;
      break;
    }
  }
  ASSERT_GE(tag_col, 0) << "inline HTML must render verbatim";
  EXPECT_EQ(screen.CellAt(tag_col, 0).foreground_color, ftxui::Color::Green);
  EXPECT_EQ(screen.CellAt(tag_col, 0).background_color,
            ftxui::Color::GrayDark);
  EXPECT_FALSE(screen.CellAt(tag_col, 0).underlined);
}

// Overlong tokens are never split mid-word in wrap mode: a 40-column token
// in a 20-column viewport clips instead of breaking.
TEST(Markdown, OverlongTokenClipsInWrapMode) {
  const std::string token(40, 'x');
  const std::string md = "```\n" + token + "\n```\n";
  auto lines = TrimmedLines(RenderLines(md, {}, 20, 60));
  for (const auto& l : lines) {
    EXPECT_LE(CellWidth(l), 20u) << "row exceeds viewport width";
  }
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_TRUE(joined.find(std::string(18, 'x')) != std::string::npos)
      << "visible head of the token must render";
  for (const auto& l : lines) {
    EXPECT_EQ(l.find(token), std::string::npos)
        << "full token must not fit on one row";
  }
}
