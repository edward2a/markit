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
  std::string debug_file;
  const char* input_file = nullptr;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--debug" && i + 1 < argc) {
      debug_file = argv[++i];
    } else if (input_file == nullptr) {
      input_file = argv[i];
    } else {
      std::cerr << "error: unexpected argument '" << arg << "'\n";
      return EXIT_FAILURE;
    }
  }

  if (input_file == nullptr) {
    std::cerr << "usage: markit [--debug <file>] <file>\n";
    return EXIT_FAILURE;
  }

  std::ifstream file(input_file);
  if (!file.is_open()) {
    std::cerr << "error: cannot open '" << input_file << "'\n";
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

  std::ofstream debug;
  if (!debug_file.empty()) {
    debug.open(debug_file);
    if (!debug.is_open()) {
      std::cerr << "error: cannot open debug file '" << debug_file << "'\n";
      return EXIT_FAILURE;
    }
  }

  auto log = [&](const std::string& what, int before, int after) {
    if (debug.is_open()) {
      debug << what << ": " << before << " -> " << after << "\n";
      debug.flush();
    }
  };

  auto content = Renderer([&] {
    Elements elements;
    for (const auto& l : lines) {
      elements.push_back(text(l));
    }
    return vbox(std::move(elements));
  });

  auto scrollable = Renderer(content, [&] {
    int max_offset = static_cast<int>(lines.size()) - 1;
    float scroll_y = max_offset > 0
                         ? static_cast<float>(scroll_offset) / max_offset
                         : 0.f;
    return content->Render()
      | focusPositionRelative(0.f, scroll_y)
      | yframe
      | vscroll_indicator
      | flex;
  });

  auto screen = App::Fullscreen();
  auto component = CatchEvent(scrollable, [&](Event event) -> bool {
    if (debug.is_open()) {
      debug << "input: " << event.DebugString()
            << " (character: '" << event.character() << "')\n";
      debug.flush();
    }

    if (event == Event::q || event == Event::Escape || event == Event::CtrlC) {
      screen.Exit();
      return true;
    }
    if (event == Event::ArrowDown || event == Event::j) {
      auto before = scroll_offset;
      scroll_offset = std::min(scroll_offset + 1, static_cast<int>(lines.size()) - 1);
      log("down", before, scroll_offset);
      return true;
    }
    if (event == Event::ArrowUp || event == Event::k) {
      auto before = scroll_offset;
      scroll_offset = std::max(scroll_offset - 1, 0);
      log("up", before, scroll_offset);
      return true;
    }
    if (event == Event::PageDown) {
      auto before = scroll_offset;
      scroll_offset = std::min(scroll_offset + 20, static_cast<int>(lines.size()) - 1);
      log("pagedown", before, scroll_offset);
      return true;
    }
    if (event == Event::PageUp) {
      auto before = scroll_offset;
      scroll_offset = std::max(scroll_offset - 20, 0);
      log("pageup", before, scroll_offset);
      return true;
    }
    if (event == Event::Home) {
      auto before = scroll_offset;
      scroll_offset = 0;
      log("home", before, scroll_offset);
      return true;
    }
    if (event == Event::End) {
      auto before = scroll_offset;
      scroll_offset = static_cast<int>(lines.size()) - 1;
      log("end", before, scroll_offset);
      return true;
    }
    return false;
  });

  screen.Loop(component);
  return EXIT_SUCCESS;
}
