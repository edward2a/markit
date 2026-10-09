// Markdown parsing and rendering for markit.
//
// Renders markdown source into an FTXUI Element tree using the md4c SAX
// parser. Emphasis, strong, code, strikethrough, links, headings, lists
// (including task lists), blockquotes, code blocks, horizontal rules, and
// GFM tables are supported.
#ifndef MARKIT_MARKDOWN_HPP
#define MARKIT_MARKDOWN_HPP

#include <functional>  // for function
#include <string>  // for string

#include <ftxui/dom/elements.hpp>  // for Element

#include "display.hpp"  // for WrapMode
#include "theme.hpp"  // for Theme

namespace markit {

struct Config;
class LayoutSnapshot;

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

// Build an immutable LayoutSnapshot for @p markdown at @p width and return the
// tree that produced it. The tree is rendered with snapshot-recording
// primitives, so captured rows include text, list markers, quote markers,
// code/table borders and rules. `snapshot` must outlive the returned tree. The
// complete layout has no height cap and allocates no screen. `cancelled` aborts
// parsing/layout early; when it fires, `*aborted` is set and `snapshot` is
// incomplete (do not use it).
ftxui::Element BuildMarkdownSnapshot(
    const std::string& markdown, const Theme& theme, WrapMode mode, int width,
    LayoutSnapshot& snapshot, const std::function<bool()>& cancelled = {},
    bool* aborted = nullptr);

// Build the tree bound to @p snapshot without laying it out. Call
// LayoutSnapshot::Build(tree, width) later to capture at the current width
// (the tree itself is width-independent). `snapshot` must outlive the tree.
// `cancelled` aborts parsing early; the returned tree is then partial.
ftxui::Element RenderMarkdownRecording(
    const std::string& markdown, const Theme& theme, WrapMode mode,
    LayoutSnapshot& snapshot, const std::function<bool()>& cancelled = {});

}  // namespace markit

#endif  // MARKIT_MARKDOWN_HPP
