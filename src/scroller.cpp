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
#include <ftxui/screen/screen.hpp>    // for Screen, Dimension

namespace ftxui {

namespace {

// Renders a width-constrained element into a tall screen and returns the last
// row index with visible content (querying the full width, so trailing padding
// and vscroll glyphs don't count). Used in wrap mode where the wrapped height
// is only known after a constrained layout.
int MeasureWrapHeight(const Element& element, int viewport_width) {
  int cap = 256;
  for (;;) {
    Screen screen = Screen::Create(Dimension::Fixed(viewport_width),
                                   Dimension::Fixed(cap));
    Render(screen, element);
    int last = -1;
    for (int row = 0; row < cap; ++row) {
      for (int col = 0; col < viewport_width; ++col) {
        const Cell& cell = screen.CellAt(col, row);
        if (cell.character != " " && cell.character != "" ||
            cell.background_color != Color::Default) {
          last = row;
          break;
        }
      }
    }
    if (last < cap - 1 || cap >= 65536) {
      return last + 1;
    }
    cap *= 4;
  }
}

class ScrollerBase : public ComponentBase {
 public:
  ScrollerBase(Component child, Ref<int> selected, Ref<int> viewport_height,
               std::function<void(int, int)> on_change, Ref<int> selected_x,
               Ref<int> viewport_width, Ref<bool> horizontal_scroll)
      : selected_(std::move(selected)),
        viewport_height_(std::move(viewport_height)),
        on_change_(std::move(on_change)),
        selected_x_(std::move(selected_x)),
        viewport_width_(std::move(viewport_width)),
        horizontal_scroll_(std::move(horizontal_scroll)) {
    Add(child);
  }

 private:
  Element OnRender() final {
    Element background = ComponentBase::Render();
    background->ComputeRequirement();
    int natural_height = std::max(1, background->requirement().min_y);
    int natural_width = std::max(1, background->requirement().min_x);
    content_height_ = natural_height;
    content_width_ = natural_width;
    int viewport_height = std::max(1, *viewport_height_);
    *viewport_height_ = viewport_height;
    int viewport_width = std::max(1, *viewport_width_);
    *viewport_width_ = viewport_width;

    // Invert the frame's centering so the top visible line equals `selected`.
    // The frame computes dy = content*y - viewport/2; we want dy = selected,
    // hence y = (selected + viewport/2 - 1) / content. The -1 is for the
    // frame's exclusive box bounds. This is re-clamped in wrap mode after the
    // wrapped height is measured.
    float y = static_cast<float>(*selected_) + viewport_height / 2.f - 1.f;

    // Scroll mode: content keeps its natural (full) width so it can be
    // panned.
    if (*horizontal_scroll_) {
      y = std::clamp(y / static_cast<float>(content_height_), 0.f, 1.f);
      float x = 0.f;
      if (content_width_ > viewport_width) {
        x = static_cast<float>(*selected_x_) + viewport_width / 2.f - 1.f;
        x = std::clamp(x / static_cast<float>(content_width_), 0.f, 1.f);
      }
      return std::move(background) | focusPositionRelative(x, y) | xframe |
             yframe | vscroll_indicator | yflex;
    }

    // Wrap mode. A frame (xframe) would lay the content out at its natural
    // width and then clip it, which defeats reflow; without it the content
    // fills the viewport width and hflow wraps there. Measure the wrapped
    // height once per viewport width for the vertical scroll math.
    if (measured_wrap_width_ != viewport_width) {
      measured_wrap_width_ = viewport_width;
      content_height_ = std::max(1, MeasureWrapHeight(background, viewport_width));
      content_width_ = viewport_width;
    }
    y = std::clamp(y / static_cast<float>(content_height_), 0.f, 1.f);
    return std::move(background) | focusPositionRelative(0.f, y) | yframe |
           yflex;
  }

  bool OnEvent(Event event) final {
    if (content_height_ < 0) {
      return false;  // no render yet; have not measured content or viewport.
    }
    int viewport_height = std::max(1, *viewport_height_);
    int before = *selected_;
    int max_offset = std::max(0, content_height_ - viewport_height);

    int after = before;
    bool handled = true;
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
    } else if (*horizontal_scroll_ &&
               (event == Event::ArrowLeft || event == Event::Character('h'))) {
      int after_x = std::max(0, *selected_x_ - 1);
      if (after_x == *selected_x_) {
        return false;
      }
      *selected_x_ = after_x;
      return true;
    } else if (*horizontal_scroll_ &&
               (event == Event::ArrowRight || event == Event::Character('l'))) {
      int viewport_width = std::max(1, *viewport_width_);
      int max_x_offset = std::max(0, content_width_ - viewport_width);
      int after_x = std::min(*selected_x_ + 1, max_x_offset);
      if (after_x == *selected_x_) {
        return false;
      }
      *selected_x_ = after_x;
      return true;
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
  Ref<int> selected_x_;
  Ref<int> viewport_width_;
  Ref<bool> horizontal_scroll_;
  int content_height_ = -1;
  int content_width_ = -1;
  int measured_wrap_width_ = -1;
};

}  // namespace

Component Scroller(Component child, Ref<int> selected, Ref<int> viewport_height,
                   std::function<void(int, int)> on_change, Ref<int> selected_x,
                   Ref<int> viewport_width, Ref<bool> horizontal_scroll) {
  return Make<ScrollerBase>(std::move(child), std::move(selected),
                            std::move(viewport_height), std::move(on_change),
                            std::move(selected_x), std::move(viewport_width),
                            std::move(horizontal_scroll));
}

}  // namespace ftxui
