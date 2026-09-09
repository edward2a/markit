// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
//
// Adapted from git-tui: https://github.com/ArthurSonzogni/git-tui
// (src/scroller.cpp / src/scroller.hpp). Reworked into a top-anchored pager.
#ifndef MARKIT_SCROLLER_HPP
#define MARKIT_SCROLLER_HPP

#include <functional>  // for function

#include <ftxui/component/component.hpp>        // for Component
#include <ftxui/util/ref.hpp>                   // for Ref

namespace ftxui {

/// @brief A focusable top-anchored vertical pager over a child component.
/// Navigation: j/k, ArrowUp/ArrowDown, PageUp/PageDown, Home/End.
/// When @p horizontal_scroll is true, ArrowLeft/Right + h/l also pan the
/// content horizontally (used for the "scroll" display mode; inactive while
/// the content wraps to the viewport width).
/// @param child The content to scroll.
/// @param selected Shared scroll offset (index of the topmost visible line).
/// @param viewport_height Shared number of visible rows (terminal height).
/// @param on_change Optional callback fired whenever @p selected changes.
/// @param selected_x Shared horizontal scroll offset (leftmost visible column).
/// @param viewport_width Shared number of visible columns (terminal width).
/// @param horizontal_scroll Enables horizontal panning when true.
/// @param content_height_out Optional out-param receiving the measured content
///        height (scroll or wrapped) on every render, for chrome such as a
///        status bar. Null when unneeded; -1 before the first render.
/// @param wrap_hint_w Optional shared width of a pre-measured wrap height
///        (see wrap_hint_h). When it equals the current viewport width on a
///        wrap-mode render, the stored height is adopted instead of
///        re-measuring; consumed (reset to -1) on adopt. -1 disables.
/// @param wrap_hint_h Optional shared pre-measured wrap height, in rows
///        through the last non-blank one (same semantics as the internal
///        measurement), for the width in wrap_hint_w. -1 disables.
Component Scroller(Component child, Ref<int> selected, Ref<int> viewport_height,
                    std::function<void(int before, int after)> on_change = {},
                    Ref<int> selected_x = Ref<int>(0),
                    Ref<int> viewport_width = Ref<int>(1),
                    Ref<bool> horizontal_scroll = Ref<bool>(false),
                    int* content_height_out = nullptr,
                    Ref<int> wrap_hint_w = Ref<int>(-1),
                    Ref<int> wrap_hint_h = Ref<int>(-1));

}  // namespace ftxui

#endif /* end of include guard: MARKIT_SCROLLER_HPP */
