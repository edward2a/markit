#ifndef MARKIT_THEME_HPP
#define MARKIT_THEME_HPP

#include <ftxui/screen/color.hpp>

namespace markit {

// Centralized color scheme. Defaults match the original hardcoded colors so a
// missing config file renders identically to the pre-config behavior.
struct Theme {
  ftxui::Color heading_h1 = ftxui::Color::Red;
  ftxui::Color heading_h2 = ftxui::Color::Yellow;
  ftxui::Color heading_h3 = ftxui::Color::Green;
  ftxui::Color heading_h4 = ftxui::Color::CyanLight;
  ftxui::Color link = ftxui::Color::CyanLight;
  ftxui::Color inline_code_fg = ftxui::Color::Green;
  ftxui::Color inline_code_bg = ftxui::Color::GrayDark;
  ftxui::Color code_block_fg = ftxui::Color::GrayLight;
  ftxui::Color code_block_bg = ftxui::Color::GrayDark;
  ftxui::Color quote_marker = ftxui::Color::GrayDark;
  // Whole-window background. Default ("none") keeps the terminal background.
  ftxui::Color background = ftxui::Color::Default;
};

}  // namespace markit

#endif  // MARKIT_THEME_HPP
