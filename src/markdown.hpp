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

#include "display.hpp"  // for WrapMode
#include "theme.hpp"  // for Theme

namespace markit {

struct Config;

/// @brief Parse @p markdown and render it into an FTXUI Element.
/// @param markdown UTF-8 markdown source text.
/// @param config App settings: color theme and display mode.
/// @return A `vbox` Element suitable for display in the scroller.
ftxui::Element RenderMarkdown(const std::string& markdown,
                               const Config& config);

// Render with the default theme and wrap mode.
ftxui::Element RenderMarkdown(const std::string& markdown);

// Narrow render-settings overload for callers that do not need the rest of
// the interactive Config, such as the asynchronous search extractor.
ftxui::Element RenderMarkdown(const std::string& markdown, const Theme& theme,
                              WrapMode mode);

}  // namespace markit

#endif  // MARKIT_MARKDOWN_HPP
