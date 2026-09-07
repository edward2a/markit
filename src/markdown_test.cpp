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

// A heading directly followed by paragraph text owes the paragraph a blank
// row, at every level (ATX headings end at the newline; the blank comes from
// the renderer, not the source).
TEST(Markdown, HeadingFollowedByParagraphHasBlank) {
  for (const char* md : {"# Hello\nworld\n", "## Hello\nworld\n"}) {
    auto rows = RenderLines(md, {}, 60, 10);
    for (auto& line : rows) {
      while (!line.empty() && line.back() == ' ') {
        line.pop_back();
      }
    }
    ASSERT_GE(rows.size(), 3u) << md;
    EXPECT_EQ(rows[0], "Hello") << md;
    EXPECT_EQ(rows[1], "") << md;
    EXPECT_EQ(rows[2], "world") << md;
  }
}

// When a --- rule follows a heading, the heading's trailing blank transfers
// past the rule: no blank between heading and rule, single blank after it.
TEST(Markdown, HeadingHrParagraphBlankAfterRule) {
  auto rows = RenderLines("# Hello\n---\nworld\n", {}, 60, 10);
  for (auto& line : rows) {
    while (!line.empty() && line.back() == ' ') {
      line.pop_back();
    }
  }
  ASSERT_GE(rows.size(), 4u);
  EXPECT_EQ(rows[0], "Hello");
  EXPECT_NE(rows[1].find("─"), std::string::npos)
      << "row 1 should be the rule, got: " << rows[1];
  EXPECT_EQ(rows[2], "");
  EXPECT_EQ(rows[3], "world");
}

// A standalone --- (no heading before it) keeps its leading-blank-only
// shape: no new trailing blank is introduced after the rule.
TEST(Markdown, StandaloneHrKeepsLeadingBlankOnly) {
  auto rows = RenderLines("para\n\n---\n\nmore\n", {}, 60, 10);
  for (auto& line : rows) {
    while (!line.empty() && line.back() == ' ') {
      line.pop_back();
    }
  }
  ASSERT_GE(rows.size(), 4u);
  EXPECT_EQ(rows[0], "para");
  EXPECT_EQ(rows[1], "");
  EXPECT_NE(rows[2].find("─"), std::string::npos)
      << "row 2 should be the rule, got: " << rows[2];
  EXPECT_EQ(rows[3], "more");
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

// HTML blocks render instead of boxing: tags are interpreted (the img alt
// text shows, banner text flows as a paragraph) and no bordered box appears.
TEST(Markdown, HtmlBlockRendersInsteadOfBoxing) {
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
  EXPECT_EQ(joined.find("<p align=\"center\">"), std::string::npos)
      << "tags must render, not show literally";
  EXPECT_EQ(joined.find("┌"), std::string::npos)
      << "rendered HTML must not be boxed";
  EXPECT_NE(joined.find("Demo"), std::string::npos)
      << "img alt text must show";
  EXPECT_NE(joined.find("Project banner text here."), std::string::npos)
      << "html block text must not be dropped";
  EXPECT_NE(joined.find("After the banner."), std::string::npos);
}

// Inline HTML tags render with markdown-equivalent styling: bold/italic
// spans inside a block, no box around them.
TEST(Markdown, HtmlInlineTagsAreStyled) {
  const std::string md = "<div>\n<b>bo</b> <i>it</i>\n</div>\n";
  ftxui::Screen screen(40, 6);
  ftxui::Render(screen, markit::RenderMarkdown(md, {}));
  EXPECT_EQ(screen.CellAt(0, 0).character, "b");
  EXPECT_TRUE(screen.CellAt(0, 0).bold) << "b tag must be bold";
  EXPECT_TRUE(screen.CellAt(1, 0).bold) << "b tag must be bold";
  EXPECT_FALSE(screen.CellAt(0, 0).italic);
  int icol = -1;
  for (int c = 0; c < 40; ++c) {
    if (screen.CellAt(c, 0).character == "i") {
      icol = c;
      break;
    }
  }
  ASSERT_GE(icol, 0) << "i tag content must render";
  EXPECT_TRUE(screen.CellAt(icol, 0).italic) << "i tag must be italic";
  EXPECT_FALSE(screen.CellAt(icol, 0).bold);
  for (int c = 0; c < 40; ++c) {
    EXPECT_NE(screen.CellAt(c, 0).character, "┌") << "no box expected";
  }
}

// Anchors render as underlined links, like markdown links.
TEST(Markdown, HtmlAnchorRendersLink) {
  const std::string md =
      "<div>\n<a href=\"https://example.com\">click</a>\n</div>\n";
  ftxui::Screen screen(40, 6);
  ftxui::Render(screen, markit::RenderMarkdown(md, {}));
  int col = -1;
  for (int c = 0; c < 40; ++c) {
    if (screen.CellAt(c, 0).character == "c") {
      col = c;
      break;
    }
  }
  ASSERT_GE(col, 0) << "link text must render";
  EXPECT_TRUE(screen.CellAt(col, 0).underlined) << "a tag must be underlined";
}

// A <br/> inside HTML ends the current row.
TEST(Markdown, HtmlBrSplitsRows) {
  auto rows =
      TrimmedLines(RenderLines("<div>\naaa<br/>bbb\n</div>\n", {}, 40, 10));
  ASSERT_GE(rows.size(), 2u);
  EXPECT_EQ(rows[0], "aaa");
  EXPECT_EQ(rows[1], "bbb");
}

// <pre> keeps its verbatim box.
TEST(Markdown, HtmlPreStaysBoxed) {
  const std::string md = "<pre>\nint x = 1;\n</pre>\n";
  auto lines = RenderLines(md, {}, 40, 10);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_NE(joined.find("┌"), std::string::npos) << "pre must stay boxed";
  EXPECT_NE(joined.find("int x = 1;"), std::string::npos);
}

// Unknown tags fall back to verbatim: the literal tag stays visible.
TEST(Markdown, HtmlUnknownTagStaysVerbatim) {
  const std::string md = "<marquee>\nhello\n</marquee>\n";
  auto lines = RenderLines(md, {}, 40, 10);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_NE(joined.find("<marquee>"), std::string::npos)
      << "unknown tags must stay literal";
  EXPECT_NE(joined.find("hello"), std::string::npos);
}

// md4c splits one HTML run into several blocks at blank lines; verbatim
// content coalesces into a single box instead of one box per block.
TEST(Markdown, HtmlConsecutiveBlocksShareOneBox) {
  const std::string md = "<!-- one -->\n\n<!-- two -->\n";
  auto lines = RenderLines(md, {}, 40, 10);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_NE(joined.find("one"), std::string::npos);
  EXPECT_NE(joined.find("two"), std::string::npos);
  size_t boxes = 0;
  for (size_t pos = joined.find("┌"); pos != std::string::npos;
       pos = joined.find("┌", pos + 1)) {
    ++boxes;
  }
  EXPECT_EQ(boxes, 1u) << "consecutive HTML must share one box";
}

// <script> contents are never interpreted and stay in a single box.
TEST(Markdown, HtmlScriptStaysInOneBox) {
  const std::string md = "<script>\nvar x = 1;\n</script>\n";
  auto lines = RenderLines(md, {}, 40, 10);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_NE(joined.find("var x = 1;"), std::string::npos);
  size_t boxes = 0;
  for (size_t pos = joined.find("┌"); pos != std::string::npos;
       pos = joined.find("┌", pos + 1)) {
    ++boxes;
  }
  EXPECT_EQ(boxes, 1u) << "script must stay in a single box";
}

// Inline tags also render inside markdown paragraphs (which is where md4c
// reports single-line markup like <i>...</i>): the tags disappear and the
// text carries the style.
TEST(Markdown, HtmlItalicParagraphRenders) {
  const std::string md = "<i>Functional Terminal (X) User interface</i>\n";
  auto lines = RenderLines(md, {}, 40, 10);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_EQ(joined.find("<i>"), std::string::npos)
      << "i tag must render, not show literally";
  EXPECT_NE(joined.find("Functional"), std::string::npos);
  ftxui::Screen screen(40, 10);
  ftxui::Render(screen, markit::RenderMarkdown(md, {}));
  EXPECT_EQ(screen.CellAt(0, 0).character, "F");
  EXPECT_TRUE(screen.CellAt(0, 0).italic) << "i tag must be italic";
}

// Bold inside a mixed markdown paragraph renders; surrounding text is kept.
TEST(Markdown, HtmlBoldInParagraphRenders) {
  const std::string md = "a <b>bo</b> c\n";
  auto lines = RenderLines(md, {}, 40, 4);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_EQ(joined.find("<b>"), std::string::npos);
  EXPECT_NE(joined.find("a bo c"), std::string::npos);
  ftxui::Screen screen(40, 4);
  ftxui::Render(screen, markit::RenderMarkdown(md, {}));
  EXPECT_TRUE(screen.CellAt(2, 0).bold) << "b tag must be bold";
  EXPECT_FALSE(screen.CellAt(0, 0).bold) << "surrounding text stays plain";
}

// A stray closing tag in a paragraph is ignored and must not pop a
// markdown span: emphasis around it keeps working.
TEST(Markdown, HtmlStrayCloseInParagraphIgnored) {
  const std::string md = "*e* </b> f\n";
  ftxui::Screen screen(40, 4);
  ftxui::Render(screen, markit::RenderMarkdown(md, {}));
  EXPECT_EQ(screen.CellAt(0, 0).character, "e");
  EXPECT_TRUE(screen.CellAt(0, 0).italic)
      << "markdown emphasis must survive a stray HTML close";
  auto lines = RenderLines(md, {}, 40, 4);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_NE(joined.find("e"), std::string::npos);
  EXPECT_NE(joined.find("f"), std::string::npos);
}

// Anchors in paragraphs render as underlined links.
TEST(Markdown, HtmlAnchorInParagraphRendersLink) {
  const std::string md =
      "a <a href=\"https://example.com\">click</a> b\n";
  ftxui::Screen screen(40, 4);
  ftxui::Render(screen, markit::RenderMarkdown(md, {}));
  int col = -1;
  for (int c = 0; c < 40; ++c) {
    if (screen.CellAt(c, 0).character == "c") {
      col = c;
      break;
    }
  }
  ASSERT_GE(col, 0) << "link text must render";
  EXPECT_TRUE(screen.CellAt(col, 0).underlined)
      << "a tag in a paragraph must be underlined";
  EXPECT_FALSE(screen.CellAt(0, 0).underlined)
      << "surrounding text stays plain";
}

// Regression (FTXUI README link row): newlines/indentation between HTML
// anchors must collapse to single spaces. Raw "\n" in a text() element made
// hflow break the row, stranding styled (underlined/hyperlink) spaces on the
// next visual row: an empty-looking line below the links showing underlines
// with the same link URLs.
TEST(Markdown, HtmlLinksAcrossLinesLeaveNoPhantomUnderline) {
  const std::string md =
      "<div>\n"
      "<a href=\"https://a.test/1\">Documentation</a> \u00b7\n"
      "<a href=\"https://b.test/2\">Report a Bug</a> \u00b7\n"
      "<a href=\"https://c.test/3\">Examples</a>\n"
      "</div>\n";
  ftxui::Screen screen(80, 6);
  ftxui::Render(screen, markit::RenderMarkdown(md, {}));
  std::string row0;
  for (int c = 0; c < 80; ++c) {
    row0 += screen.CellAt(c, 0).character;
  }
  EXPECT_NE(row0.find("Documentation"), std::string::npos);
  EXPECT_NE(row0.find("Report a Bug"), std::string::npos);
  EXPECT_NE(row0.find("Examples"), std::string::npos);
  for (int r = 1; r < 6; ++r) {
    for (int c = 0; c < 80; ++c) {
      EXPECT_FALSE(screen.CellAt(c, r).underlined)
          << "phantom underline at row " << r << " col " << c;
    }
  }
}

// Regression (FTXUI README link row): md4c splits the whitespace between HTML
// anchors into several callbacks ("\n" then "  "), which accumulated into
// double spaces. Separators render once, in both wrap and scroll modes.
TEST(Markdown, HtmlLinksAcrossLinesCollapseToSingleSpaces) {
  const std::string md =
      "<div>\n"
      "  <a href=\"https://a.test/1\">Documentation</a> \u00b7\n"
      "  <a href=\"https://b.test/2\">Report a Bug</a>\n"
      "</div>\n";
  for (markit::WrapMode mode :
       {markit::WrapMode::Wrap, markit::WrapMode::Scroll}) {
    markit::Config cfg;
    cfg.horizontal_wrap = mode;
    ftxui::Screen screen(80, 4);
    ftxui::Render(screen, markit::RenderMarkdown(md, cfg));
    std::string row0;
    for (int c = 0; c < 80; ++c) {
      row0 += screen.CellAt(c, 0).character;
    }
    EXPECT_NE(row0.find("Documentation \u00b7 Report a Bug"),
              std::string::npos)
        << "single spaces in mode " << static_cast<int>(mode) << ": [" << row0
        << "]";
  }
}

// Regression (FTXUI README badges): the gap between two adjacent links with
// the same URL (e.g. href="#" badges) must stay plain in wrap mode. WrapRow
// used to treat cross-fragment whitespace as label-internal whenever the keys
// matched, underlining the gap.
TEST(Markdown, HtmlSameUrlLinkGapStaysPlainInWrap) {
  const std::string md =
      "<div>\n"
      "<a href=\"#\"><img alt=\"one\" src=\"x\"></a>\n"
      "<a href=\"#\"><img alt=\"two\" src=\"y\"></a>\n"
      "</div>\n";
  ftxui::Screen screen(40, 4);
  ftxui::Render(screen, markit::RenderMarkdown(md, {}));
  int one_end = -1;
  int two_start = -1;
  for (int c = 0; c < 40; ++c) {
    const std::string ch = screen.CellAt(c, 0).character;
    if (ch == "e" && one_end < 0) {
      one_end = c;  // end of "one"
    }
    if (ch == "t") {
      two_start = c;  // start of "two"
      break;
    }
  }
  ASSERT_GE(one_end, 0);
  ASSERT_GE(two_start, 0);
  EXPECT_EQ(two_start, one_end + 2) << "exactly one space between badges";
  EXPECT_FALSE(screen.CellAt(one_end + 1, 0).underlined)
      << "gap between same-URL links must stay plain";
}

// Regression (FTXUI README codecov badge): newlines/indentation *inside* an
// anchor around a lone image are source formatting, not link content. They
// used to render as link-styled padding around the image (visible as
// surrounding underlines in scroll mode). Both modes show just the image.
TEST(Markdown, HtmlImageLinkEdgeWhitespaceHasNoSurroundingUnderline) {
  const std::string md =
      "<div>\n"
      "  <a href=\"https://c.test/9\">\n"
      "    <img src=\"y.png\">\n"
      "  </a>\n"
      "</div>\n";
  for (markit::WrapMode mode :
       {markit::WrapMode::Wrap, markit::WrapMode::Scroll}) {
    markit::Config cfg;
    cfg.horizontal_wrap = mode;
    ftxui::Screen screen(40, 4);
    ftxui::Render(screen, markit::RenderMarkdown(md, cfg));
    int img_col = -1;
    for (int c = 0; c < 40; ++c) {
      if (screen.CellAt(c, 0).character == "[") {
        img_col = c;
        break;
      }
    }
    ASSERT_GE(img_col, 0) << "image must render in mode "
                          << static_cast<int>(mode);
    for (int c = img_col; c < img_col + 5; ++c) {
      EXPECT_TRUE(screen.CellAt(c, 0).underlined)
          << "image itself stays underlined in mode " << static_cast<int>(mode)
          << " col " << c;
    }
    if (img_col > 0) {
      EXPECT_FALSE(screen.CellAt(img_col - 1, 0).underlined)
          << "no leading underline in mode " << static_cast<int>(mode);
    }
    for (int c = img_col + 5; c < 40; ++c) {
      EXPECT_FALSE(screen.CellAt(c, 0).underlined)
          << "no trailing underline in mode " << static_cast<int>(mode)
          << " col " << c;
    }
  }
}

// <details>/<summary> render statically and always expanded: the summary
// gets a disclosure marker, content flows as normal blocks, nothing boxed.
TEST(Markdown, HtmlDetailsRendersExpanded) {
  const std::string md =
      "<details>\n<summary>Click me</summary>\nHidden content here.\n</details>\n";
  auto lines = RenderLines(md, {}, 40, 10);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_EQ(joined.find("<details>"), std::string::npos)
      << "tags must render, not show literally";
  EXPECT_EQ(joined.find("┌"), std::string::npos)
      << "details must not be boxed";
  EXPECT_NE(joined.find("Click me"), std::string::npos);
  EXPECT_NE(joined.find("Hidden content here."), std::string::npos);
  EXPECT_NE(joined.find("▸"), std::string::npos)
      << "summary needs a disclosure marker";
  ftxui::Screen screen(40, 10);
  ftxui::Render(screen, markit::RenderMarkdown(md, {}));
  bool summary_bold = false;
  for (int r = 0; r < 10 && !summary_bold; ++r) {
    for (int c = 0; c < 40; ++c) {
      if (screen.CellAt(c, r).character == "C") {
        summary_bold = screen.CellAt(c, r).bold;
        break;
      }
    }
  }
  EXPECT_TRUE(summary_bold) << "summary text must be bold";
}

// <details open> shows the expanded marker.
TEST(Markdown, HtmlDetailsOpenMarker) {
  const std::string md =
      "<details open>\n<summary>Shown</summary>\nBody.\n</details>\n";
  auto lines = RenderLines(md, {}, 40, 10);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_NE(joined.find("▾"), std::string::npos)
      << "open details needs the expanded marker";
  EXPECT_NE(joined.find("Shown"), std::string::npos);
  EXPECT_NE(joined.find("Body."), std::string::npos);
}

// A stray </summary> stays verbatim instead of corrupting the stack.
TEST(Markdown, HtmlDetailsStrayCloseVerbatim) {
  const std::string md = "Before.\n\n</summary>\n\nAfter.\n";
  auto lines = RenderLines(md, {}, 40, 10);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_NE(joined.find("</summary>"), std::string::npos)
      << "stray close must stay literal";
  EXPECT_NE(joined.find("Before."), std::string::npos);
  EXPECT_NE(joined.find("After."), std::string::npos);
}

// An unclosed <summary> still gets its marker at the block boundary.
TEST(Markdown, HtmlDetailsUnclosedSummary) {
  const std::string md =
      "<details>\n<summary>Unclosed\nContent.\n</details>\n";
  auto lines = RenderLines(md, {}, 40, 10);
  std::string joined;
  for (const auto& l : lines) {
    joined += l;
  }
  EXPECT_NE(joined.find("▸"), std::string::npos)
      << "unclosed summary still gets a marker";
  EXPECT_NE(joined.find("Unclosed"), std::string::npos);
  EXPECT_NE(joined.find("Content."), std::string::npos);
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

// Whitespace *inside* a styled run keeps the style in wrap mode (matching
// scroll mode, which renders the run as one continuous element), while the
// boundary spaces around it stay plain.
TEST(Markdown, WrapLinkInternalSpaceIsUnderlined) {
  const std::string md = "seen [label one](https://e.test) tail\n";
  ftxui::Screen screen(40, 4);
  ftxui::Render(screen, markit::RenderMarkdown(md, {}));
  int label_col = -1;
  for (int c = 0; c < 40; ++c) {
    if (screen.CellAt(c, 0).character == "l") {  // start of "label"
      label_col = c;
      break;
    }
  }
  ASSERT_GE(label_col, 1);
  // Boundary space before the link stays plain (see
  // WrapLinkLeadingSpaceIsNotUnderlined)...
  EXPECT_FALSE(screen.CellAt(label_col - 1, 0).underlined);
  // ...but the space inside the label is underlined like the words.
  int inner_space = -1;
  for (int c = label_col; c < 40; ++c) {
    if (screen.CellAt(c, 0).character == " ") {
      inner_space = c;
      break;
    }
  }
  ASSERT_GE(inner_space, 0);
  EXPECT_TRUE(screen.CellAt(inner_space, 0).underlined)
      << "space inside the link label must stay underlined";
  EXPECT_TRUE(screen.CellAt(inner_space + 1, 0).underlined)
      << "link continuation must stay underlined";
}

// Same rule for inline code: the space inside `code span` keeps the
// background instead of punching a plain gap.
TEST(Markdown, WrapInlineCodeInternalSpaceKeepsBackground) {
  const std::string md = "a `code span` b\n";
  ftxui::Screen screen(40, 4);
  ftxui::Render(screen, markit::RenderMarkdown(md, {}));
  int code_col = -1;
  for (int c = 0; c < 40; ++c) {
    if (screen.CellAt(c, 0).character == "c") {  // start of "code"
      code_col = c;
      break;
    }
  }
  ASSERT_GE(code_col, 1);
  EXPECT_NE(screen.CellAt(code_col - 1, 0).background_color,
            ftxui::Color::GrayDark);
  int inner_space = -1;
  for (int c = code_col; c < 40; ++c) {
    if (screen.CellAt(c, 0).character == " ") {
      inner_space = c;
      break;
    }
  }
  ASSERT_GE(inner_space, 0);
  EXPECT_EQ(screen.CellAt(inner_space, 0).background_color,
            ftxui::Color::GrayDark)
      << "space inside inline code must keep the background";
}

// In scroll mode the code box fills the available width (like tables and
// like wrap mode) instead of hugging its own widest line: the border runs
// edge to edge rather than closing right after the short content.
TEST(Markdown, CodeBoxFillsWidthInScrollMode) {
  markit::Config cfg;
  cfg.horizontal_wrap = markit::WrapMode::Scroll;
  const std::string md = "```\nhi\n```\n";
  ftxui::Screen screen(60, 6);
  ftxui::Render(screen, markit::RenderMarkdown(md, cfg));
  int border_right = -1;
  int border_row = -1;
  for (int r = 0; r < 6 && border_right < 0; ++r) {
    for (int c = 0; c < 60; ++c) {
      if (screen.CellAt(c, r).character == "┐") {
        border_row = r;
        border_right = c;
        break;
      }
    }
  }
  ASSERT_GE(border_right, 0) << "code box border must render";
  EXPECT_EQ(border_right, 59) << "border must run to the window edge";
  EXPECT_EQ(screen.CellAt(0, border_row).character, "┌");
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
