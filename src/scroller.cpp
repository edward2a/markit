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
#include <ftxui/dom/elements.hpp>  // for operator|, Element, focusPositionRelative, yframe, yflex
#include <ftxui/dom/node.hpp>      // for Node
#include <ftxui/dom/requirement.hpp>  // for Requirement

#include "keybindings.hpp"  // for markit::KeyBindings, MatchesKey

namespace ftxui {

namespace {

// Compiled-in defaults for callers that pass no keybindings (unit tests
// exercising the default map).
const markit::KeyBindings& DefaultKeyBindings() {
  static const markit::KeyBindings kDefaults;
  return kDefaults;
}

class ScrollerBase : public ComponentBase {
 public:
  ScrollerBase(Component child, Ref<int> selected, Ref<int> viewport_height,
                std::function<void(int, int)> on_change, Ref<int> selected_x,
                Ref<int> viewport_width, Ref<bool> horizontal_scroll,
                int* content_height_out, Ref<int> wrap_height,
                const markit::KeyBindings* keybindings)
      : selected_(std::move(selected)),
        viewport_height_(std::move(viewport_height)),
        on_change_(std::move(on_change)),
        selected_x_(std::move(selected_x)),
        viewport_width_(std::move(viewport_width)),
        horizontal_scroll_(std::move(horizontal_scroll)),
        content_height_out_(content_height_out),
        wrap_height_(std::move(wrap_height)),
        keybindings_(keybindings) {
    Add(child);
  }

 private:
  Element OnRender() final {
    Element background = ComponentBase::Render();
    // The child tree is often pointer-identical across frames (a pure
    // content renderer returns its cached tree), and its requirement is
    // then stable too, so skip the redundant walk. The shared_ptr is
    // retained to keep the identity key alive: a bare raw pointer could be
    // recycled by a newly built tree and cause a false cache hit.
    if (background.get() != rendered_tree_.get()) {
      background->ComputeRequirement();
      rendered_tree_ = background;
      natural_height_ = std::max(1, background->requirement().min_y);
      natural_width_ = std::max(1, background->requirement().min_x);
    }
    int natural_height = natural_height_;
    int natural_width = natural_width_;
    int viewport_height = std::max(1, *viewport_height_);
    *viewport_height_ = viewport_height;
    int viewport_width = std::max(1, *viewport_width_);
    *viewport_width_ = viewport_width;

    // Invert the frame's centering so the top visible line equals `selected`.
    // The frame works on inclusive boxes (max - min = size - 1): it offsets
    // with dy = focus - external/2 where external = viewport - 1 is halved
    // with integer division, then truncates the focus product with int().
    // Mirror that here and add +0.5 so the truncation lands on the intended
    // row. (The old `- 1` assumed exclusive bounds: every position showed
    // one row too early and the last row stayed unreachable at End.) This is
    // re-clamped after the content height is known.
    float y = static_cast<float>(*selected_) +
              static_cast<float>((viewport_height - 1) / 2) + 0.5f;

    // Scroll mode: content keeps its natural (full) width so it can be panned.
    if (*horizontal_scroll_) {
      content_height_ = natural_height;
      content_width_ = natural_width;
      y = std::clamp(y / static_cast<float>(content_height_), 0.f, 1.f);
      float x = 0.f;
      if (content_width_ > viewport_width) {
        // Same inclusive-box mirroring as the vertical axis above.
        x = static_cast<float>(*selected_x_) +
            static_cast<float>((viewport_width - 1) / 2) + 0.5f;
        x = std::clamp(x / static_cast<float>(content_width_), 0.f, 1.f);
      }
      PublishContentHeight();
      return std::move(background) | focusPositionRelative(x, y) | xframe |
             yframe | yflex;
    }

    // Wrap mode. A frame (xframe) would lay the content out at its natural
    // width and then clip it, which defeats reflow; without it the content
    // fills the viewport width and hflow wraps there. The caller supplies the
    // wrapped height from the document layout snapshot; when it is absent
    // (content that does not reflow) the unwrapped requirement is used.
    content_height_ = (*wrap_height_ >= 0)
                          ? std::max(1, *wrap_height_)
                          : natural_height;
    content_width_ = viewport_width;
    y = std::clamp(y / static_cast<float>(content_height_), 0.f, 1.f);
    PublishContentHeight();
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
    // Navigation keys are configurable (markit::KeyBindings); the shape of
    // the handling — clamp, no-op reporting, pan gating on scroll mode —
    // is unchanged.
    const markit::KeyBindings& kb =
        keybindings_ != nullptr ? *keybindings_ : DefaultKeyBindings();
    if (markit::MatchesKey(event, kb.scroll_up)) {
      after = before - 1;
    } else if (markit::MatchesKey(event, kb.scroll_down)) {
      after = before + 1;
    } else if (markit::MatchesKey(event, kb.page_up)) {
      after = before - (viewport_height - 1);
    } else if (markit::MatchesKey(event, kb.page_down)) {
      after = before + (viewport_height - 1);
    } else if (markit::MatchesKey(event, kb.goto_top)) {
      after = 0;
    } else if (markit::MatchesKey(event, kb.goto_bottom)) {
      after = max_offset;
    } else if (*horizontal_scroll_ &&
               markit::MatchesKey(event, kb.pan_left)) {
      int after_x = std::max(0, *selected_x_ - 1);
      if (after_x == *selected_x_) {
        return false;
      }
      *selected_x_ = after_x;
      return true;
    } else if (*horizontal_scroll_ &&
               markit::MatchesKey(event, kb.pan_right)) {
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

  void PublishContentHeight() {
    if (content_height_out_ != nullptr) {
      *content_height_out_ = content_height_;
    }
  }

  bool Focusable() const final { return true; }

  Ref<int> selected_;
  Ref<int> viewport_height_;
  std::function<void(int, int)> on_change_;
  Ref<int> selected_x_;
  Ref<int> viewport_width_;
  Ref<bool> horizontal_scroll_;
  int* content_height_out_;
  Ref<int> wrap_height_;
  const markit::KeyBindings* keybindings_;
   int content_height_ = -1;
   int content_width_ = -1;
   Element rendered_tree_;
   int natural_height_ = 1;
   int natural_width_ = 1;
};

}  // namespace

Component Scroller(Component child, Ref<int> selected, Ref<int> viewport_height,
                    std::function<void(int before, int after)> on_change,
                    Ref<int> selected_x, Ref<int> viewport_width,
                    Ref<bool> horizontal_scroll, int* content_height_out,
                    Ref<int> wrap_height,
                    const markit::KeyBindings* keybindings) {
  return Make<ScrollerBase>(std::move(child), std::move(selected),
                            std::move(viewport_height), std::move(on_change),
                            std::move(selected_x), std::move(viewport_width),
                            std::move(horizontal_scroll), content_height_out,
                            std::move(wrap_height), keybindings);
}

}  // namespace ftxui
