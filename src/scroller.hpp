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
/// @param child The content to scroll.
/// @param selected Shared scroll offset (index of the topmost visible line).
/// @param viewport_height Shared number of visible rows (terminal height).
/// @param on_change Optional callback fired whenever @p selected changes.
Component Scroller(Component child, Ref<int> selected, Ref<int> viewport_height,
                   std::function<void(int before, int after)> on_change = {});

}  // namespace ftxui

#endif /* end of include guard: MARKIT_SCROLLER_HPP */
