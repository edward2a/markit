#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

using namespace ftxui;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: markit <file>\n";
    return EXIT_FAILURE;
  }

  std::ifstream file(argv[1]);
  if (!file.is_open()) {
    std::cerr << "error: cannot open '" << argv[1] << "'\n";
    return EXIT_FAILURE;
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) {
    lines.push_back(line);
  }

  if (lines.empty()) {
    lines.push_back("(empty file)");
  }

  int scroll_offset = 0;

  auto content = Renderer([&] {
    Elements elements;
    for (const auto& l : lines) {
      elements.push_back(text(l));
    }
    return vbox(std::move(elements));
  });

  auto scrollable = Renderer(content, [&] {
    return content->Render()
      | focusPosition(0, scroll_offset)
      | yframe
      | vscroll_indicator
      | flex;
  });

  auto screen = App::Fullscreen();
  auto component = CatchEvent(scrollable, [&](Event event) -> bool {
    if (event == Event::q || event == Event::Escape || event == Event::CtrlC) {
      screen.Exit();
      return true;
    }
    if (event == Event::ArrowDown || event == Event::j) {
      scroll_offset = std::min(scroll_offset + 1, static_cast<int>(lines.size()) - 1);
      return true;
    }
    if (event == Event::ArrowUp || event == Event::k) {
      scroll_offset = std::max(scroll_offset - 1, 0);
      return true;
    }
    if (event == Event::PageDown) {
      scroll_offset = std::min(scroll_offset + 20, static_cast<int>(lines.size()) - 1);
      return true;
    }
    if (event == Event::PageUp) {
      scroll_offset = std::max(scroll_offset - 20, 0);
      return true;
    }
    if (event == Event::Home) {
      scroll_offset = 0;
      return true;
    }
    if (event == Event::End) {
      scroll_offset = static_cast<int>(lines.size()) - 1;
      return true;
    }
    return false;
  });

  screen.Loop(component);
  return EXIT_SUCCESS;
}
