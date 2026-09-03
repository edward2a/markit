#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "scroller.hpp"

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

  int selected = 0;
  int viewport_height = Terminal::Size().dimy;
  auto scroller =
      Scroller(std::move(content), &selected, &viewport_height,
               [&](int before, int after) { log("scroll", before, after); });

  auto screen = App::Fullscreen();
  auto component = CatchEvent(scroller, [&](Event event) -> bool {
    if (debug.is_open()) {
      debug << "input: " << event.DebugString()
            << " (character: '" << event.character() << "')\n";
      debug.flush();
    }

    if (event == Event::q || event == Event::Escape || event == Event::CtrlC) {
      screen.Exit();
      return true;
    }
    return false;
  });

  screen.Loop(component);
  return EXIT_SUCCESS;
}
