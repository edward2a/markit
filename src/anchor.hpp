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

#include <vector>  // for vector

#include <ftxui/dom/elements.hpp>  // for Element

#include "chrome.hpp"  // for Heading

namespace markit {

// Map the top-of-view position from one mode's tree to the other's.
// `old_selected` is the pre-toggle offset, `old_is_scroll` the pre-toggle
// mode; widths/heights are the live viewport values. Returns the new offset,
// clamped to the new scroll range (0 when anything is unusable).
int MapTogglePosition(const ftxui::Element& old_tree,
                      const ftxui::Element& new_tree,
                      const std::vector<Heading>& headings, int old_selected,
                      int viewport_width, int viewport_height,
                      bool old_is_scroll);

}  // namespace markit

#endif  // MARKIT_ANCHOR_HPP
