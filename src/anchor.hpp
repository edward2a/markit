// Section-scoped content anchor for the scroll/wrap mode toggle.
//
// When the user presses `w`, the content tree is rebuilt for the other mode
// and row numbers change meaning (scroll counts unwrapped rows, wrap counts
// reflowed visual rows), so the raw `selected` offset cannot be carried over.
// Instead the top-of-view row is fingerprinted (first words of its normalized
// text) inside its heading-delimited section, and the fingerprint is
// re-located in the new layout's corresponding section.
#ifndef MARKIT_ANCHOR_HPP
#define MARKIT_ANCHOR_HPP

#include <string>   // for string
#include <utility>  // for pair
#include <vector>  // for vector

#include <ftxui/dom/elements.hpp>  // for Element

#include "chrome.hpp"  // for Heading

namespace markit {

// Locate each heading's row in `tree` rendered at `width`: ordered
// (row, heading-index) pairs, heading indices into `headings` (nav rows).
// Same matching as the toggle anchor (bold row containing the heading's
// fingerprint words, occurrence rank for duplicates, empty fingerprints
// skipped), so the nav highlight and the toggle agree on sections.
// `is_scroll` selects the seed the same way MapTogglePosition does
// (scroll trees never reflow). Empty when the tree is unusable.
std::vector<std::pair<int, int>> LocateHeadingRows(
    const ftxui::Element& tree, const std::vector<Heading>& headings,
    int width, int viewport_height, bool is_scroll);

// Render `tree` offscreen at `width`, returning one plain-text row per
// content row (raw cell text, rows through the last non-blank one; interior
// blanks kept so indices align with scroll offsets). `height_hint` seeds the
// render height (the scroller's measured content height when known); a
// grow-and-re-render loop keeps it correct when the hint falls short. Wrap
// trees render at the viewport width; scroll trees render wide (they never
// split rows, so indices stay stable while clipped text becomes searchable)
// — callers choose the width.
std::vector<std::string> RenderTextRows(const ftxui::Element& tree, int width,
                                        int height_hint);

// Map the top-of-view position from one mode's tree to the other's.
// `old_selected` is the pre-toggle offset, `old_is_scroll` the pre-toggle
// mode; widths/heights are the live viewport values. Returns the new offset,
// clamped to the new scroll range (0 when anything is unusable).
//
// Both trees render narrow (viewport width) first: scroll-mode rows never
// split, so structure is identical and only tails clip. A scroll tree is
// re-rendered at its natural width only when clipping makes the narrow
// match ambiguous (truncated fingerprint, or hits solely on clipped rows);
// a width-induced row-count change falls back to proportional. When
// `new_height_out` is set, it receives the new tree's row count at viewport
// width (the displayed height in both modes).
int MapTogglePosition(const ftxui::Element& old_tree,
                      const ftxui::Element& new_tree,
                      const std::vector<Heading>& headings, int old_selected,
                      int viewport_width, int viewport_height,
                      bool old_is_scroll, int* new_height_out = nullptr);

}  // namespace markit

#endif  // MARKIT_ANCHOR_HPP
