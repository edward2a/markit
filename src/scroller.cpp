// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
//
// Top-anchored vertical pager for markit. `selected` is the index of the
// topmost visible line (the view offset). The frame's focus anchor is offset
// so that the requested line appears at the top of the viewport.
#include "scroller.hpp"

#include <algorithm>  // for clamp, max

#include <ftxui/component/component_base.hpp>  // for ComponentBase
#include <ftxui/component/event.hpp>  // for Event, Event::ArrowDown, Event::ArrowUp, Event::End, Event::Home, Event::PageDown, Event::PageUp
#include <ftxui/dom/elements.hpp>  // for operator|, Element, focusPositionRelative, yframe, vscroll_indicator, yflex
#include <ftxui/dom/node.hpp>      // for Node
#include <ftxui/dom/requirement.hpp>  // for Requirement

namespace ftxui {

namespace {

class ScrollerBase : public ComponentBase {
 public:
  ScrollerBase(Component child, Ref<int> selected, Ref<int> viewport_height,
               std::function<void(int, int)> on_change)
      : selected_(std::move(selected)),
        viewport_height_(std::move(viewport_height)),
        on_change_(std::move(on_change)) {
    Add(child);
  }

 private:
  Element OnRender() final {
    Element background = ComponentBase::Render();
    background->ComputeRequirement();
    content_height_ = std::max(1, background->requirement().min_y);
    int viewport_height = std::max(1, *viewport_height_);
    *viewport_height_ = viewport_height;

    // Invert the frame's centering so the top visible line equals `selected`.
    // The frame computes dy = content*y - viewport/2; we want dy = selected,
    // hence y = (selected + viewport/2 - 1) / content. The -1 accounts for the
    // frame's exclusive box bounds. Clamp y to keep the frame within bounds.
    float y = static_cast<float>(*selected_) + viewport_height / 2.f - 1.f;
    y = std::clamp(y / static_cast<float>(content_height_), 0.f, 1.f);

    return std::move(background) | focusPositionRelative(0.f, y) | yframe |
           vscroll_indicator | yflex;
  }

  bool OnEvent(Event event) final {
    if (content_height_ < 0) {
      return false;  // no render yet; have not measured content or viewport.
    }
    int viewport_height = std::max(1, *viewport_height_);
    int before = *selected_;
    int max_offset = std::max(0, content_height_ - viewport_height);

    int after = before;
    if (event == Event::ArrowUp || event == Event::Character('k')) {
      after = before - 1;
    } else if (event == Event::ArrowDown || event == Event::Character('j')) {
      after = before + 1;
    } else if (event == Event::PageUp) {
      after = before - (viewport_height - 1);
    } else if (event == Event::PageDown) {
      after = before + (viewport_height - 1);
    } else if (event == Event::Home) {
      after = 0;
    } else if (event == Event::End) {
      after = max_offset;
    } else {
      return false;
    }

    after = std::clamp(after, 0, max_offset);
    if (after == before) {
      return false;
    }
    set_selected(after);
    return true;
  }

  void set_selected(int value) {
    int before = *selected_;
    *selected_ = value;
    if (on_change_) {
      on_change_(before, value);
    }
  }

  bool Focusable() const final { return true; }

  Ref<int> selected_;
  Ref<int> viewport_height_;
  std::function<void(int, int)> on_change_;
  int content_height_ = -1;
};

}  // namespace

Component Scroller(Component child, Ref<int> selected, Ref<int> viewport_height,
                   std::function<void(int, int)> on_change) {
  return Make<ScrollerBase>(std::move(child), std::move(selected),
                            std::move(viewport_height), std::move(on_change));
}

}  // namespace ftxui
