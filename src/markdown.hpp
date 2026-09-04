// Markdown parsing and rendering for markit.
//
// Renders markdown source into an FTXUI Element tree using the md4c SAX
// parser. Emphasis, strong, code, strikethrough, links, headings, lists
// (including task lists), blockquotes, code blocks, horizontal rules, and
// GFM tables are supported.
#ifndef MARKIT_MARKDOWN_HPP
#define MARKIT_MARKDOWN_HPP

#include <string>  // for string

#include <ftxui/dom/elements.hpp>  // for Element

namespace markit {

/// @brief Parse @p markdown and render it into an FTXUI Element.
/// @param markdown UTF-8 markdown source text.
/// @return A `vbox` Element suitable for display in the scroller.
ftxui::Element RenderMarkdown(const std::string& markdown);

}  // namespace markit

#endif  // MARKIT_MARKDOWN_HPP
