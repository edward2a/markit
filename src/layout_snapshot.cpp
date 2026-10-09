// Package E feasibility prototype. See layout_snapshot.hpp.
#include "layout_snapshot.hpp"

#include <algorithm>  // for max
#include <memory>     // for make_shared
#include <utility>    // for move

#include <ftxui/dom/elements.hpp>   // for borderLight, separator
#include <ftxui/dom/node.hpp>       // for Node
#include <ftxui/screen/box.hpp>     // for Box
#include <ftxui/screen/screen.hpp>  // for Screen
#include <ftxui/screen/string.hpp>  // for Utf8ToGlyphs, string_width

namespace markit {

namespace {

std::string Repeat(const char* unit, int count) {
  std::string out;
  for (int i = 0; i < count; ++i) {
    out += unit;
  }
  return out;
}

class SnapshotTextNode final : public ftxui::Node {
 public:
  SnapshotTextNode(LayoutSnapshot* snapshot, std::string text, bool bold)
      : snapshot_(snapshot), text_(std::move(text)), bold_(bold) {}

  void ComputeRequirement() final {
    requirement_.min_x = ftxui::string_width(text_);
    requirement_.min_y = 1;
  }

  void SetBox(ftxui::Box box) final {
    ftxui::Node::SetBox(box);
    if (snapshot_ != nullptr && snapshot_->recording() &&
        box.x_min <= box.x_max && box.y_min <= box.y_max) {
      snapshot_->AddRun(SnapshotRun{box.y_min, box.x_min, text_, bold_});
    }
  }

  void Render(ftxui::Screen& screen) final {
    const ftxui::Box visible = ftxui::Box::Intersection(screen.stencil, box_);
    if (visible.IsEmpty()) {
      return;
    }
    int x = box_.x_min;
    for (const std::string& glyph : ftxui::Utf8ToGlyphs(text_)) {
      if (x > box_.x_max) {
        break;
      }
      if (x >= visible.x_min && x <= visible.x_max) {
        screen.CellAt(x, box_.y_min).character = glyph;
      }
      ++x;
    }
  }

 private:
  LayoutSnapshot* snapshot_;
  std::string text_;
  bool bold_;
};

}  // namespace

ftxui::Element SnapshotText(LayoutSnapshot* snapshot, std::string text,
                            bool bold) {
  return std::make_shared<SnapshotTextNode>(snapshot, std::move(text), bold);
}

namespace {

// Records a light box frame around its child, matching ftxui::borderLight, so
// captured rows include table/code borders exactly like a render.
class SnapshotBorderNode final : public ftxui::Node {
 public:
  SnapshotBorderNode(LayoutSnapshot* snapshot, ftxui::Element child)
      : ftxui::Node(ftxui::Elements{ftxui::borderLight(std::move(child))}),
        snapshot_(snapshot) {}

  void ComputeRequirement() final {
    ftxui::Node::ComputeRequirement();
    requirement_ = children_[0]->requirement();
  }

  void SetBox(ftxui::Box box) final {
    ftxui::Node::SetBox(box);
    children_[0]->SetBox(box);
    Record(box);
  }

  void Render(ftxui::Screen& screen) final { children_[0]->Render(screen); }

 private:
  void Record(ftxui::Box box) const {
    if (snapshot_ == nullptr || !snapshot_->recording()) {
      return;
    }
    const int width = box.x_max - box.x_min + 1;
    const int height = box.y_max - box.y_min + 1;
    if (width < 2 || height < 2) {
      return;
    }
    const std::string inner = Repeat("\u2500", width - 2);  // ─
    snapshot_->AddRun(
        SnapshotRun{box.y_min, box.x_min, "\u250C" + inner + "\u2510", false});
    snapshot_->AddRun(
        SnapshotRun{box.y_max, box.x_min, "\u2514" + inner + "\u2518", false});
    for (int y = box.y_min + 1; y < box.y_max; ++y) {
      snapshot_->AddRun(SnapshotRun{y, box.x_min, "\u2502", false});
      snapshot_->AddRun(SnapshotRun{y, box.x_max, "\u2502", false});
    }
  }

  LayoutSnapshot* snapshot_;
};

// Records a horizontal light rule, matching ftxui::separator.
class SnapshotSeparatorNode final : public ftxui::Node {
 public:
  explicit SnapshotSeparatorNode(LayoutSnapshot* snapshot)
      : ftxui::Node(ftxui::Elements{ftxui::separator()}), snapshot_(snapshot) {}

  void ComputeRequirement() final {
    ftxui::Node::ComputeRequirement();
    requirement_ = children_[0]->requirement();
  }

  void SetBox(ftxui::Box box) final {
    ftxui::Node::SetBox(box);
    children_[0]->SetBox(box);
    if (snapshot_ != nullptr && snapshot_->recording() &&
        box.x_min <= box.x_max) {
      snapshot_->AddRun(SnapshotRun{box.y_min, box.x_min,
                                    Repeat("\u2500", box.x_max - box.x_min + 1),
                                    false});
    }
  }

  void Render(ftxui::Screen& screen) final { children_[0]->Render(screen); }

 private:
  LayoutSnapshot* snapshot_;
};

// Records the content-row span of a heading element under its occurrence
// ordinal, so heading navigation reads a stable row range instead of matching
// text fingerprints.
class SnapshotHeadingNode final : public ftxui::Node {
 public:
  SnapshotHeadingNode(LayoutSnapshot* snapshot, int ordinal, ftxui::Element child)
      : ftxui::Node(ftxui::Elements{std::move(child)}),
        snapshot_(snapshot),
        ordinal_(ordinal) {}

  void ComputeRequirement() final {
    ftxui::Node::ComputeRequirement();
    requirement_ = children_[0]->requirement();
  }

  void SetBox(ftxui::Box box) final {
    ftxui::Node::SetBox(box);
    children_[0]->SetBox(box);
    if (snapshot_ != nullptr && snapshot_->recording() &&
        box.y_min <= box.y_max) {
      snapshot_->AddHeadingSpan(ordinal_, box.y_min, box.y_max);
    }
  }

  void Render(ftxui::Screen& screen) final { children_[0]->Render(screen); }

 private:
  LayoutSnapshot* snapshot_;
  int ordinal_;
};

}  // namespace

ftxui::Element SnapshotBorder(LayoutSnapshot* snapshot, ftxui::Element child) {
  return std::make_shared<SnapshotBorderNode>(snapshot, std::move(child));
}

ftxui::Element SnapshotSeparator(LayoutSnapshot* snapshot) {
  return std::make_shared<SnapshotSeparatorNode>(snapshot);
}

ftxui::Element SnapshotHeading(LayoutSnapshot* snapshot, int ordinal,
                               ftxui::Element child) {
  return std::make_shared<SnapshotHeadingNode>(snapshot, ordinal,
                                               std::move(child));
}

void LayoutSnapshot::Build(ftxui::Element element, int width,
                           const std::function<bool()>& cancelled) {
  runs_.clear();
  row_runs_.clear();
  heading_spans_.clear();
  width_ = std::max(0, width);
  height_ = 0;
  layout_passes_ = 0;
  cancelled_ = false;
  if (!element) {
    return;
  }

  recording_ = true;
  ftxui::Node::Status status;
  element->Check(&status);
  int iterations = 0;
  do {
    if (cancelled && cancelled()) {
      cancelled_ = true;
      break;
    }
    runs_.clear();
    heading_spans_.clear();
    element->ComputeRequirement();
    const int h = std::max(1, element->requirement().min_y);
    height_ = h;
    ftxui::Box box;
    box.x_min = 0;
    box.x_max = std::max(0, width_ - 1);
    box.y_min = 0;
    box.y_max = h - 1;
    element->SetBox(box);
    status.need_iteration = false;
    status.iteration++;
    element->Check(&status);
    ++layout_passes_;
    ++iterations;
  } while (status.need_iteration && iterations < 20);
  recording_ = false;

  row_runs_.resize(static_cast<size_t>(std::max(0, height_)));
  for (size_t i = 0; i < runs_.size(); ++i) {
    const int row = runs_[i].row;
    if (row >= 0 && row < height_) {
      row_runs_[static_cast<size_t>(row)].push_back(static_cast<int>(i));
    }
  }
  for (std::vector<int>& row : row_runs_) {
    std::sort(row.begin(), row.end(), [&](int a, int b) {
      return runs_[static_cast<size_t>(a)].col <
             runs_[static_cast<size_t>(b)].col;
    });
  }
}

void LayoutSnapshot::AddRun(const SnapshotRun& run) { runs_.push_back(run); }

void LayoutSnapshot::AddHeadingSpan(int ordinal, int first_row, int last_row) {
  if (ordinal < 0) {
    return;
  }
  if (static_cast<size_t>(ordinal) >= heading_spans_.size()) {
    heading_spans_.resize(static_cast<size_t>(ordinal) + 1, {-1, -1});
  }
  heading_spans_[static_cast<size_t>(ordinal)] = {first_row, last_row};
}

std::string LayoutSnapshot::RowText(int row) const {
  if (row < 0 || row >= height_ || width_ <= 0) {
    return {};
  }
  std::vector<std::string> cells(static_cast<size_t>(width_), " ");
  for (const int index : row_runs_[static_cast<size_t>(row)]) {
    const SnapshotRun& run = runs_[static_cast<size_t>(index)];
    int col = run.col;
    for (const std::string& glyph : ftxui::Utf8ToGlyphs(run.text)) {
      if (col >= 0 && col < width_) {
        cells[static_cast<size_t>(col)] = glyph;
      }
      col += std::max(1, ftxui::string_width(glyph));
    }
  }
  std::string out;
  out.reserve(static_cast<size_t>(width_));
  for (const std::string& cell : cells) {
    out += cell;
  }
  return out;
}

std::string LayoutSnapshot::RowSparseText(int row) const {
  if (row < 0 || row >= height_) {
    return {};
  }
  std::string out;
  for (const int index : row_runs_[static_cast<size_t>(row)]) {
    out += runs_[static_cast<size_t>(index)].text;
  }
  return out;
}

std::vector<std::string> LayoutSnapshot::TextRows() const {
  std::vector<std::string> out;
  out.reserve(static_cast<size_t>(std::max(0, height_)));
  int last = -1;
  for (int row = 0; row < height_; ++row) {
    std::string text = RowSparseText(row);
    for (char c : text) {
      if (c != ' ') {
        last = row;
        break;
      }
    }
    out.push_back(std::move(text));
  }
  out.resize(static_cast<size_t>(last + 1));
  return out;
}

std::vector<std::string> LayoutSnapshot::Rows(int first, int last) const {
  std::vector<std::string> out;
  if (first < 0) {
    first = 0;
  }
  if (last > height_) {
    last = height_;
  }
  out.reserve(static_cast<size_t>(std::max(0, last - first)));
  for (int row = first; row < last; ++row) {
    out.push_back(RowText(row));
  }
  return out;
}

}  // namespace markit
