#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "config.hpp"
#include "markdown.hpp"
#include "scroller.hpp"

using namespace ftxui;

int main(int argc, char** argv) {
  std::string debug_file;
  std::string config_file;
  bool dump_config = false;
  const char* input_file = nullptr;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_file = argv[++i];
    } else if (arg == "--debug" && i + 1 < argc) {
      debug_file = argv[++i];
    } else if (arg == "--dump-config") {
      dump_config = true;
    } else if (input_file == nullptr) {
      input_file = argv[i];
    } else {
      std::cerr << "error: unexpected argument '" << arg << "'\n";
      return EXIT_FAILURE;
    }
  }

  if (dump_config) {
    markit::DumpDefaultConfig(std::cout);
    return EXIT_SUCCESS;
  }

  if (input_file == nullptr) {
    std::cerr << "usage: markit [--debug <file>] [--config <file>]"
                 " [--dump-config] <file>\n";
    return EXIT_FAILURE;
  }

  const std::string config_path =
      config_file.empty() ? markit::DefaultConfigPath() : config_file;
  markit::Theme theme;
  try {
    theme = markit::LoadConfig(config_path);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return EXIT_FAILURE;
  }

  std::ifstream file(input_file);
  if (!file.is_open()) {
    std::cerr << "error: cannot open '" << input_file << "'\n";
    return EXIT_FAILURE;
  }

  std::string contents((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());

  if (contents.empty()) {
    contents = "(empty file)";
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

  auto content = Renderer([&] { return markit::RenderMarkdown(contents, theme); });

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
